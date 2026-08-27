#include "manifest.hpp"

#include "gitignore.hpp"

#include <fstream>
#include <git2.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <toml.hpp>

namespace {

// Empty email defaults. cc/bcc are arrays so the user can list multiple
// recipients; fields are only inserted when missing so existing values
// survive a re-run of `gitm manifest`.
toml::table emailDefaults() {
    toml::table email;
    email["from"] = "";
    email["to"] = "";
    email["cc"] = toml::array{};
    email["bcc"] = toml::array{};
    email["subject"] = "";
    return email;
}

void ensureEmailSection(toml::value& manifest) {
    if (manifest.contains("email") && manifest["email"].is_table()) {
        toml::table& email = manifest["email"].as_table();
        if (!email.contains("from"))
            email["from"] = "";
        if (!email.contains("to"))
            email["to"] = "";
        if (!email.contains("cc"))
            email["cc"] = toml::array{};
        if (!email.contains("bcc"))
            email["bcc"] = toml::array{};
        if (!email.contains("subject"))
            email["subject"] = "";
    } else {
        manifest["email"] = emailDefaults();
    }
}

std::string headHash(git_repository* repo) {
    git_reference* head = nullptr;
    if (git_repository_head(&head, repo) != 0)
        throw std::runtime_error("Failed to resolve HEAD");

    const git_oid* oid = git_reference_target(head);
    if (oid == nullptr) {
        git_reference_free(head);
        throw std::runtime_error("HEAD does not point to a commit");
    }

    char hash[GIT_OID_SHA1_HEXSIZE + 1];
    git_oid_tostr(hash, sizeof(hash), oid);

    git_reference_free(head);
    return hash;
}

std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.good())
        return "";

    std::stringstream content;
    content << file.rdbuf();
    return content.str();
}

// Writes the manifest with the base commit recorded. Existing user-authored
// keys are preserved; the base commit is (re)set to the current HEAD and the
// email section is filled in with defaults for any missing fields.
void writeManifest(const std::string& path, const std::string& baseHash) {
    toml::value manifest;
    const std::string existing = readFile(path);
    if (!existing.empty()) {
        try {
            std::istringstream stream(existing);
            manifest = toml::parse(stream, path);
        } catch (const std::exception&) {
            manifest = toml::value(toml::table{});
        }
    }

    manifest["base"] = baseHash;

    ensureEmailSection(manifest);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.good())
        throw std::runtime_error("Failed to open manifest for writing: " + path);

    file << toml::format(manifest);
    if (!file.good())
        throw std::runtime_error("Failed to write manifest: " + path);
}

void ensureGitignore(const std::string& gitignorePath) {
    std::string content = readFile(gitignorePath);
    if (containsGitignoreGitm(content))
        return;

    editGitignore(content);

    std::ofstream file(gitignorePath, std::ios::binary | std::ios::trunc);
    if (file.good())
        file << content;
}

} // namespace

bool createManifest(const std::string& repoPath, const std::string& manifestPath,
                    std::string& baseHash) {
    git_libgit2_init();

    git_repository* repo = nullptr;
    if (git_repository_open(&repo, repoPath.c_str()) != 0) {
        git_libgit2_shutdown();
        return false;
    }

    try {
        baseHash = headHash(repo);

        const char* workdir = git_repository_workdir(repo);
        const std::string gitignorePath =
            workdir ? std::string(workdir) + ".gitignore" : ".gitignore";

        writeManifest(manifestPath, baseHash);
        ensureGitignore(gitignorePath);
    } catch (...) {
        git_repository_free(repo);
        git_libgit2_shutdown();
        return false;
    }

    git_repository_free(repo);
    git_libgit2_shutdown();
    return true;
}

bool readManifestBase(const std::string& manifestPath, std::string& baseHash) {
    const std::string content = readFile(manifestPath);
    if (content.empty())
        return false;

    try {
        std::istringstream stream(content);
        toml::value manifest = toml::parse(stream, manifestPath);
        if (!manifest.contains("base") || !manifest["base"].is_string())
            return false;

        baseHash = manifest["base"].as_string();
        return !baseHash.empty();
    } catch (const std::exception&) {
        return false;
    }
}
