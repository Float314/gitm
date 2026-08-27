/* Login manager for mail. Uses system credential stores on Win32, macOS and
   Linux; falls back to GNU Pass elsewhere. */

#include "login_mgr.hpp"

#include <iostream>
#include <optional>
#include <string>

#if defined(_WIN32) || defined(_WIN64)
    #ifndef _WIN32_WINNT
        #define _WIN32_WINNT 0x0601
    #endif
    #include <windows.h>
    #include <wincred.h>
    #include <vector>
    #pragma comment(lib, "Advapi32.lib")
#elif defined(__APPLE__)
    #include <CoreFoundation/CoreFoundation.h>
    #include <Security/Security.h>
#elif defined(__linux__) || defined(__linux) || defined(__LINUX__)
    #include <libsecret/secret.h>
#else
    #warning The detected system may or may not support/have a secrets manager; gitm will use GNU Pass as its cred. manager.
    #include <cstdio>
    #include <cstdlib>
#endif

#if defined(_WIN32) || defined(_WIN64)
    constexpr const char* __plat = "WINDOWS";
#elif defined(__APPLE__)
    constexpr const char* __plat = "MACOSX";
#elif defined(__linux__) || defined(__linux) || defined(__LINUX__)
    constexpr const char* __plat = "LINUX";
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonflyBSD__)
    constexpr const char* __plat = "BSD";
#else
    constexpr const char* __plat = "OTX";
#endif

void printUsage_lgn() {
    if (std::string(__plat) == "BSD") {
        std::cout << "BSD and other systems will store passwords using GNU Pass. Make sure it is installed!";
    }
}

namespace {

std::string credTarget(const std::string& email) {
    return "gitm/" + email;
}


// platform backends: storeSecret / lookupSecret / forgetSecret
#if defined(_WIN32) || defined(_WIN64)

std::wstring toWide(const std::string& str) {
    if (str.empty())
        return L"";
    const int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 0)
        return L"";
    std::wstring wide(static_cast<std::size_t>(len) - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, wide.data(), len);
    return wide;
}

bool storeSecret(const std::string& target, const std::string& secret) {
    const std::wstring wtarget = toWide(target);

    // store the secret as raw UTF-8 bytes
    std::vector<BYTE> blob(secret.begin(), secret.end());
    blob.push_back(0); // NUL terminator

    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(wtarget.c_str());
    cred.UserName = const_cast<LPWSTR>(wtarget.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
    cred.CredentialBlob = blob.data();
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

    return CredWriteW(&cred, 0) != FALSE;
}

std::optional<std::string> lookupSecret(const std::string& target) {
    const std::wstring wtarget = toWide(target);

    PCREDENTIALW pcred = nullptr;
    if (!CredReadW(wtarget.c_str(), CRED_TYPE_GENERIC, 0, &pcred))
        return std::nullopt;

    std::optional<std::string> secret;
    if (pcred && pcred->CredentialBlob && pcred->CredentialBlobSize > 0) {
        const char* bytes = reinterpret_cast<const char*>(pcred->CredentialBlob);
        secret = std::string(bytes, pcred->CredentialBlobSize);
        while (!secret->empty() && secret->back() == '\0')
            secret->pop_back();
    }

    CredFree(pcred);
    return secret;
}

bool forgetSecret(const std::string& target) {
    const std::wstring wtarget = toWide(target);
    return CredDeleteW(wtarget.c_str(), CRED_TYPE_GENERIC, 0) != FALSE;
}

#elif defined(__APPLE__)

bool storeSecret(const std::string& target, const std::string& secret) {
    CFStringRef service =
        CFStringCreateWithCString(kCFAllocatorDefault, "gitm", kCFStringEncodingUTF8);
    CFStringRef account =
        CFStringCreateWithCString(kCFAllocatorDefault, target.c_str(), kCFStringEncodingUTF8);
    CFDataRef data =
        CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(secret.data()), secret.size());

    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData};
    const void* values[] = {kSecClassGenericPassword, service, account, data};
    CFDictionaryRef query = CFDictionaryCreate(
        kCFAllocatorDefault, keys, values, 4,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );

    // replace any existing entry for this account
    SecItemDelete(query);
    const OSStatus status = SecItemAdd(query, nullptr);

    CFRelease(query);
    CFRelease(service);
    CFRelease(account);
    CFRelease(data);
    return status == errSecSuccess;
}

std::optional<std::string> lookupSecret(const std::string& target) {
    CFStringRef service =
        CFStringCreateWithCString(kCFAllocatorDefault, "gitm", kCFStringEncodingUTF8);
    CFStringRef account =
        CFStringCreateWithCString(kCFAllocatorDefault, target.c_str(), kCFStringEncodingUTF8);

    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData, kSecMatchLimitOne};
    const void* values[] = {kSecClassGenericPassword, service, account, kCFBooleanTrue, kSecMatchLimitOne};
    CFDictionaryRef query = CFDictionaryCreate(
        kCFAllocatorDefault, keys, values, 5,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );

    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(query, &result);

    std::optional<std::string> secret;
    if (status == errSecSuccess && result) {
        const CFDataRef data = static_cast<CFDataRef>(result);
        const UInt8* bytes = CFDataGetBytePtr(data);
        const CFIndex length = CFDataGetLength(data);
        secret = std::string(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(length));
    }

    if (result)
        CFRelease(result);
    CFRelease(query);
    CFRelease(service);
    CFRelease(account);
    return secret;
}

bool forgetSecret(const std::string& target) {
    CFStringRef service =
        CFStringCreateWithCString(kCFAllocatorDefault, "gitm", kCFStringEncodingUTF8);
    CFStringRef account =
        CFStringCreateWithCString(kCFAllocatorDefault, target.c_str(), kCFStringEncodingUTF8);

    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
    const void* values[] = {kSecClassGenericPassword, service, account};
    CFDictionaryRef query = CFDictionaryCreate(
        kCFAllocatorDefault, keys, values, 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );

    const OSStatus status = SecItemDelete(query);

    CFRelease(query);
    CFRelease(service);
    CFRelease(account);
    return status == errSecSuccess || status == errSecItemNotFound;
}

#elif defined(__linux__) || defined(__linux) || defined(__LINUX__)

const SecretSchema* credSchema() {
    static const SecretSchema schema = {
        "gitm", SECRET_SCHEMA_NONE,
        {
            {"user", SECRET_SCHEMA_ATTRIBUTE_STRING},
        },
    };
    return &schema;
}

bool storeSecret(const std::string& target, const std::string& secret) {
    const gchar* attributes[] = {"user", target.c_str(), nullptr};
    GError* error = nullptr;
    const gboolean ok = secret_password_store_sync(
        credSchema(), attributes, nullptr, "gitm", secret.c_str(), nullptr, &error
    );
    if (error)
        g_error_free(error);
    return ok != FALSE;
}

std::optional<std::string> lookupSecret(const std::string& target) {
    const gchar* attributes[] = {"user", target.c_str(), nullptr};
    GError* error = nullptr;
    gchar* stored = secret_password_lookup_sync(credSchema(), attributes, nullptr, &error);
    if (error)
        g_error_free(error);
    if (!stored)
        return std::nullopt;
    std::optional<std::string> secret(stored);
    g_free(stored);
    return secret;
}

bool forgetSecret(const std::string& target) {
    const gchar* attributes[] = {"user", target.c_str(), nullptr};
    GError* error = nullptr;
    const gboolean ok = secret_password_clear_sync(credSchema(), attributes, nullptr, &error);
    if (error)
        g_error_free(error);
    return ok != FALSE;
}

#else // GNU Pass fallback

bool storeSecret(const std::string& target, const std::string& secret) {
    const std::string cmd = "pass insert -f gitm/" + target + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "w");
    if (!pipe)
        return false;
    std::fputs(secret.c_str(), pipe);
    std::fputc('\n', pipe);
    const int rc = pclose(pipe);
    return rc == 0;
}

std::optional<std::string> lookupSecret(const std::string& target) {
    const std::string cmd = "pass show gitm/" + target + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        return std::nullopt;
    std::string secret;
    char buf[256];
    while (std::fgets(buf, sizeof(buf), pipe))
        secret += buf;
    pclose(pipe);
    while (!secret.empty() && (secret.back() == '\n' || secret.back() == '\r'))
        secret.pop_back();
    if (secret.empty())
        return std::nullopt;
    return secret;
}

bool forgetSecret(const std::string& target) {
    const std::string cmd = "pass rm -f gitm/" + target + " 2>/dev/null";
    return std::system(cmd.c_str()) == 0;
}

#endif

} // namespace

std::string promptPassword(const std::string& prompt) {
    std::cout << prompt;
    std::cout.flush();

#if defined(_WIN32) || defined(_WIN64)
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD oldMode = 0;
    const bool restore = GetConsoleMode(hStdin, &oldMode) != FALSE;
    if (restore)
        SetConsoleMode(hStdin, oldMode & ~ENABLE_ECHO_INPUT);
#endif

    std::string password;
    std::getline(std::cin, password);

#if defined(_WIN32) || defined(_WIN64)
    if (restore)
        SetConsoleMode(hStdin, oldMode);
#endif

    std::cout << "\n";
    return password;
}

bool saveCredential(const std::string& email, const std::string& password) {
    return storeSecret(credTarget(email), password);
}

std::optional<std::string> loadCredential(const std::string& email) {
    return lookupSecret(credTarget(email));
}

bool hasCredential(const std::string& email) {
    return lookupSecret(credTarget(email)).has_value();
}

bool deleteCredential(const std::string& email) {
    return forgetSecret(credTarget(email));
}
