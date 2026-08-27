#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <stdexcept>
#include <string>
#include <sstream>
#include <toml.hpp>

#include "email_sender.hpp"
#include "gitm_apply.hpp"
#include "login_mgr.hpp"
#include "manifest.hpp"
#include "patch_reader.hpp"
#include "patchfile.hpp"

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// SMTP config persistence & email helpers
// ---------------------------------------------------------------------------

std::string homeDir() {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
    if (h && *h)
        return h;
#endif
    const char* home = std::getenv("HOME");
    return home ? home : ".";
}

fs::path smtpConfigPath() { return fs::path(homeDir()) / ".gitm-smtp"; }

bool loadSmtpConfig(smtp_config& out) {
    const fs::path path = smtpConfigPath();
    std::ifstream file(path);
    if (!file.good())
        return false;
    std::stringstream content;
    content << file.rdbuf();
    if (content.str().empty())
        return false;
    try {
        toml::value cfg = toml::parse(content, path.string());
        if (!cfg.contains("host") || !cfg["host"].is_string())
            return false;
        out.host = cfg["host"].as_string();
        out.port = cfg.contains("port") && cfg["port"].is_integer()
                       ? static_cast<int>(cfg["port"].as_integer()) : 587;
        out.use_tls = cfg.contains("use_tls") && cfg["use_tls"].is_boolean() &&
                          cfg["use_tls"].as_boolean() ? true : false;
        out.username = cfg.contains("username") && cfg["username"].is_string()
                           ? cfg["username"].as_string() : "";
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool saveSmtpConfig(const smtp_config& cfg) {
    toml::value root;
    root["host"] = cfg.host;
    root["port"] = static_cast<long long>(cfg.port);
    root["use_tls"] = cfg.use_tls;
    root["username"] = cfg.username;

    std::ofstream file(smtpConfigPath(), std::ios::trunc);
    if (!file.good())
        return false;
    file << toml::format(root);
    return file.good();
}

// Loads the SMTP sender configuration: prefer the on-disk config, otherwise
// try to reconstruct it from the credential store.
bool resolveSmtpConfig(smtp_config& out) {
    if (loadSmtpConfig(out) && !out.host.empty()) {
        // the password lives in the OS credential manager, not the config file
        if (out.password.empty() && !out.username.empty()) {
            if (const auto pw = loadCredential(out.username))
                out.password = *pw;
        }
        return true;
    }
    return false;
}

// Reads a value out of the gitm.manifest in `dir`.
toml::value loadManifestTable(const std::string& dir) {
    const fs::path path = fs::path(dir) / "gitm.manifest";
    std::ifstream file(path);
    if (!file.good())
        return toml::value(toml::table{});
    std::stringstream content;
    content << file.rdbuf();
    try {
        return toml::parse(content, path.string());
    } catch (const std::exception&) {
        return toml::value(toml::table{});
    }
}

// Builds an email message from the [email] section of the manifest.
mail_message messageFromManifest(const toml::value& manifest) {
    mail_message m;
    if (!manifest.contains("email") || !manifest.at("email").is_table())
        return m;

    const toml::table& email = manifest.at("email").as_table();
    auto str = [&](const char* key) {
        return email.contains(key) && email.at(key).is_string() ? email.at(key).as_string() : "";
    };
    m.from = str("from");
    m.subject = str("subject");

    std::vector<std::string> names;
    auto addIf = [&](const char* key) {
        if (email.contains(key) && email.at(key).is_string() && !email.at(key).as_string().empty())
            names.push_back(email.at(key).as_string());
        else if (email.contains(key) && email.at(key).is_array())
            for (const auto& e : email.at(key).as_array())
                if (e.is_string())
                    names.push_back(e.as_string());
    };
    addIf("to");
    addIf("cc");
    addIf("bcc");
    m.to = names;
    return m;
}

// Sends a notification `body` for `subject` to the recipients named in the
// manifest. Logs a warning (does not throw) when the mail cannot be sent.
void notify(const std::string& subject, const std::string& body) {
    const toml::value manifest = loadManifestTable(".");
    mail_message m = messageFromManifest(manifest);
    if (m.from.empty())
        m.from = [&]() {
            if (manifest.contains("email") && manifest.at("email").is_table()) {
                const toml::table& e = manifest.at("email").as_table();
                if (e.contains("from") && e.at("from").is_string())
                    return e.at("from").as_string();
            }
            return std::string("gitm@localhost");
        }();
    if (m.to.empty()) {
        std::print(stderr, "No recipient in manifest; notification not sent.\n");
        return;
    }
    if (m.subject.empty())
        m.subject = subject;
    m.body = body;

    smtp_config cfg;
    if (!resolveSmtpConfig(cfg)) {
        std::print(stderr, "SMTP not configured; run 'gitm login-through-mail <email>' first.\n");
        return;
    }

    std::string error;
    if (send_email(cfg, m, error))
        std::print("Notification '{}' sent to {}.\n",
                   subject, m.to.empty() ? "?" : m.to.front());
    else
        std::print(stderr, "Failed to send notification: {}\n", error);
}

// ---------------------------------------------------------------------------
// text helpers
// ---------------------------------------------------------------------------

std::string readTextFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.good())
        return "";
    std::stringstream content;
    content << file.rdbuf();
    return content.str();
}

const char* shortLicense() {
    return R"LIC(gitm - send git patches over email
Copyright (C) 2026 Float314

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY.  See the GNU General Public License for
more details: gitm license-full
)LIC";
}

std::string branchSuffixFor(const std::string& patchPath) {
    fs::path p(patchPath);
    return p.stem().string();
}

} // namespace

std::string helpText() {
    return R"md(
gitm - help 

Short Introduction - Gitm is the new way of sending patches over emails! 

commands: 
manifest              Create a new manifest for gitm patch 

--- Review ---
review <location>     Review a sent gitm patch (manual download in some folder) 
deny <patch/branch>   Denies the branch (sends an email to show denial) 
accept <patch>        Accepts the patch (merge Branch to HEAD) 
yochoppedlowk <pat>   Denies the patch So hard it sends a funny email roasting the patcher 

--- Patches --- 
make <patch>          Compiles the batch 
delete <patch>        Deletes a patch (AND THE ASSOCIATED COMMITS!) 
delete-no-commit <p..>Deletes a patch and associated files, not editing the commits made 

--- Settings --- 
login-through-mail <e>Saves login info for emails 
logout                Logout

--- Details --- 
license               Shows likcense 
license-full          ... 
privacy-policy        Shows privact policy (for the concerned) 
    )md";
}

int cmdManifest() {
    std::string baseHash;
    if (createManifest(".", "gitm.manifest", baseHash)) {
        std::print("Started tracking git repo changes from commit: {}...\n",
                   baseHash.substr(0, 6));
        std::print("Created manifest file!\n");
        return 0;
    }
    std::print(stderr, "Failed to create manifest file!\n");
    return 1;
}

int cmdMake(const std::string& patchName) {
    const std::string outputPath = patchName + ".gitm";

    std::string baseHash;
    if (!readManifestBase("gitm.manifest", baseHash)) {
        std::print(stderr, "No valid gitm.manifest found. Run 'gitm manifest' first.\n");
        return 1;
    }

    try {
        const patchfile patch(".", baseHash, "gitm.manifest");
        if (patch.write(outputPath)) {
            std::print("Compiled {}.gitm patch (base {}...)\n", patchName,
                       baseHash.substr(0, 6));
            return 0;
        }
        std::print(stderr, "Failed to write {}\n", outputPath);
        return 1;
    } catch (const std::exception& e) {
        std::print(stderr, "Failed to compile patch: {}\n", e.what());
        return 1;
    }
}

int cmdReview(const std::string& location) {
    parsed_patch patch;
    std::string error;
    if (!readPatches(location, patch, error)) {
        std::print(stderr, "Failed to read patch: {}\n", error);
        return 1;
    }

    std::print("gitm patch: {}\n", location);
    std::print("  base commit : {}\n", patch.base.empty() ? "<unknown>" : patch.base);
    std::print("  commits     : {}\n", patch.commits.size());

    for (std::size_t i = 0; i < patch.commits.size(); ++i) {
        const parsed_commit& c = patch.commits[i];
        const std::string subject = [&]() {
            const std::string msg = c.message.empty() ? "" : c.message;
            const std::size_t nl = msg.find('\n');
            return msg.substr(0, nl == std::string::npos ? msg.size() : nl);
        }();
        std::print("  [{:>3}/{:<3}] {:<10} {} <{}>  {}\n",
                   i + 1, patch.commits.size(),
                   c.hash.empty() ? "<none>" : c.hash.substr(0, 10),
                   c.author_name, c.author_email, subject);
    }

    if (patch.manifest.contains("email") && patch.manifest["email"].is_table()) {
        const toml::table& email = patch.manifest["email"].as_table();
        if (email.contains("subject") && email.at("subject").is_string())
            std::print("  subject     : {}\n", email.at("subject").as_string());
    }
    return 0;
}

int cmdAccept(const std::string& patchPath) {
    parsed_patch patch;
    std::string error;
    if (!readPatches(patchPath, patch, error)) {
        std::print(stderr, "Failed to read patch: {}\n", error);
        return 1;
    }

    const std::string suffix = branchSuffixFor(patchPath);
    if (!gitm_accept(patch, ".", suffix, error)) {
        std::print(stderr, "Failed to accept patch: {}\n", error);
        return 1;
    }

    std::print("Accepted {} commits from {} onto the working branch.\n",
               patch.commits.size(), patchPath);

    // notify the author
    notify("gitm: patch accepted",
           "Your gitm patch has been accepted and merged.\n");
    return 0;
}

int cmdDeny(const std::string& patchPath, bool roast, const std::string& branchOverride) {
    parsed_patch patch;
    std::string error;
    const bool parsed = readPatches(patchPath, patch, error);
    if (!parsed) {
        std::print(stderr, "Failed to read patch: {}\n", error);
        return 1;
    }

    std::string branch = branchOverride.empty() ? branchSuffixFor(patchPath) : branchOverride;
    std::string derr;
    if (gitm_discard(".", branch, derr)) {
        std::print("Discarded branch gitm/{}.\n", branch);
    } else {
        std::print(stderr, "Info: {}\n", derr);
    }

    std::string body;
    if (roast) {
        body =
            "Your gitm patch has been DENIED. And honestly? It deserved it.\n\n"
            "Your code was so bad that if it were a crime scene, your diff would\n"
            "be the chalk outline. Merge requests weep, linters file grievances,\n"
            "and the compiler filed a restraining order. We roasted your patch\n"
            "over git log and it still came out raw.\n\n"
            "Better luck next time. (Read the feedback. Take it. Grow.)\n";
    } else {
        body = "Your gitm patch was not accepted.\n\n"
               "The maintainer has declined to merge your changes. Please review\n"
               "any feedback attached to this message and resubmit if appropriate.\n";
    }

    notify("gitm: patch denied", body);
    std::print("Denied {}.\n", patchPath);
    return 0;
}

int cmdDelete(const std::string& patchPath, bool commitAware) {
    // commitAware == true: also discard the associated branch (its commits)
    if (commitAware) {
        const std::string branch = branchSuffixFor(patchPath);
        std::string derr;
        if (gitm_discard(".", branch, derr))
            std::print("Discarded branch gitm/{} (associated commits removed).\n", branch);
        else
            std::print(stderr, "Info: {}\n", derr);
    }

    if (fs::exists(patchPath)) {
        std::error_code ec;
        fs::remove(patchPath, ec);
        if (ec) {
            std::print(stderr, "Failed to delete {}: {}\n", patchPath, ec.message());
            return 1;
        }
        std::print("Deleted patch file {}.\n", patchPath);
    } else {
        std::print("Patch file {} does not exist.\n", patchPath);
        return 1;
    }
    return 0;
}

int cmdLoginThroughMail(const std::string& email) {
    const std::string password = promptPassword("Password for " + email + ": ");
    if (password.empty()) {
        std::print(stderr, "Empty password; aborting.\n");
        return 1;
    }

    if (!saveCredential(email, password)) {
        std::print(stderr, "Failed to store credential.\n");
        return 1;
    }

    smtp_config cfg;
    if (guess_smtp_config(email, password, cfg) && saveSmtpConfig(cfg)) {
        std::print("Saved login for {}.\n", email);
        std::print("SMTP server: {}:{} ({}TLS)\n",
                   cfg.host, cfg.port, cfg.use_tls ? "" : "START");
        return 0;
    }

    std::print("Saved login for {} (SMTP not auto-configured).\n", email);
    return 0;
}

int cmdLogout(const std::string& email) {
    if (deleteCredential(email)) {
        std::print("Logged out of {}.\n", email);
        return 0;
    }
    std::print(stderr, "No stored credential for {}.\n", email);
    return 1;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::print("{}", helpText());
        return 0;
    }

    const std::string cmd = argv[1];

    if (cmd == "manifest")
        return cmdManifest();
    if (cmd == "make") {
        if (argc < 3) {
            std::print(stderr, "Usage: gitm make <patch>\n");
            return 1;
        }
        return cmdMake(argv[2]);
    }
    if (cmd == "review") {
        if (argc < 3) {
            std::print(stderr, "Usage: gitm review <location>\n");
            return 1;
        }
        return cmdReview(argv[2]);
    }
    if (cmd == "accept") {
        if (argc < 3) {
            std::print(stderr, "Usage: gitm accept <patch>\n");
            return 1;
        }
        return cmdAccept(argv[2]);
    }
    if (cmd == "deny" || cmd == "yochoppedlowk") {
        if (argc < 3) {
            std::print(stderr, "Usage: gitm {} <patch/branch>\n", cmd);
            return 1;
        }
        const std::string branchOverride = argc > 3 ? argv[3] : "";
        return cmdDeny(argv[2], cmd == "yochoppedlowk", branchOverride);
    }
    if (cmd == "delete" || cmd == "delete-no-commit") {
        if (argc < 3) {
            std::print(stderr, "Usage: gitm {} <patch>\n", cmd);
            return 1;
        }
        return cmdDelete(argv[2], cmd == "delete");
    }
    if (cmd == "login-through-mail") {
        if (argc < 3) {
            std::print(stderr, "Usage: gitm login-through-mail <email>\n");
            return 1;
        }
        return cmdLoginThroughMail(argv[2]);
    }
    if (cmd == "logout") {
        if (argc < 3) {
            std::print(stderr, "Usage: gitm logout <email>\n");
            return 1;
        }
        return cmdLogout(argv[2]);
    }
    if (cmd == "license") {
        std::print("{}", shortLicense());
        return 0;
    }
    if (cmd == "license-full") {
        std::print("{}", readTextFile("LICENSE"));
        return 0;
    }
    if (cmd == "privacy-policy") {
        std::print("{}", readTextFile("docs/privacy_policy.txt"));
        return 0;
    }

    std::print(stderr, "Unknown command: {}\n", cmd);
    std::print("{}", helpText());
    return 1;
}
