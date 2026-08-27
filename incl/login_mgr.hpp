#pragma once

#include <optional>
#include <string>

// A gitm mail account credential
struct creds {
    std::optional<std::string> target; // platform-specific credential target
    std::string email;
    std::string password;
};

// Reads a password from the console without echoing it.
std::string promptPassword(const std::string& prompt);

// Stores the password for `email` in the platform credential manager.
// Returns true on success.
bool saveCredential(const std::string& email, const std::string& password);

// Reads back the stored password for `email`. Returns std::nullopt when no
// credential is stored.
std::optional<std::string> loadCredential(const std::string& email);

// True if a credential is stored for `email`.
bool hasCredential(const std::string& email);

// Deletes the stored credential for `email`. Returns false on failure.
bool deleteCredential(const std::string& email);
