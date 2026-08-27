#include "patchfile.hpp"

#include "commit_file.hpp"
#include "commits.hpp"

#include <algorithm>
#include <fstream>
#include <git2.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <toml.hpp>
#include <vector>

namespace {

// Reads the user-authored gitm.manifest. An empty or missing manifest is
// turned into an empty table so the patch can always carry a valid manifest.
toml::value loadManifest(const std::string& manifestPath) {
    std::ifstream file(manifestPath, std::ios::binary);
    if (!file.good()) {
        toml::table empty;
        return toml::value(std::move(empty));
    }

    std::stringstream content;
    content << file.rdbuf();
    if (content.str().empty()) {
        toml::table empty;
        return toml::value(std::move(empty));
    }

    return toml::parse(content, manifestPath);
}

} // namespace

patchfile::patchfile(const std::string& repoPath, const std::string& baseHash,
                     const std::string& manifestPath)
    : repoPath_(repoPath), baseHash_(baseHash), manifestPath_(manifestPath) {}

std::vector<std::uint8_t> patchfile::generate() const {
    git_libgit2_init();

    git_repository* repo = nullptr;
    if (git_repository_open(&repo, repoPath_.c_str()) != 0) {
        git_libgit2_shutdown();
        throw std::runtime_error("Repository is not valid: " + repoPath_);
    }

    std::vector<std::uint8_t> buffer;

    try {
        commit_collect collector(repoPath_, baseHash_);
        std::vector<std::string> hashes = collector.commitsAfter();
        if (hashes.empty())
            throw std::runtime_error("No commits found after base " + baseHash_);

        // oldest-first so the patches can be applied in order
        std::reverse(hashes.begin(), hashes.end());

        // augment the user manifest with the collected commits
        toml::value manifest = loadManifest(manifestPath_);
        toml::table& manifest_table = manifest.as_table();
        manifest_table["base"] = baseHash_;

        toml::array commit_hashes;
        for (const auto& hash : hashes)
            commit_hashes.push_back(hash);

        manifest_table["commits"] = std::move(commit_hashes);
        manifest_table["commit_count"] = static_cast<long long>(hashes.size());

        const std::string manifest_text = toml::format(manifest);

        // [4 bytes] magic
        const char magic[] = "GITM";
        buffer.insert(
            buffer.end(),
            reinterpret_cast<const std::uint8_t*>(magic),
            reinterpret_cast<const std::uint8_t*>(magic) + sizeof(magic) - 1
        );

        // [n bytes] gitm.manifest
        buffer.insert(
            buffer.end(),
            reinterpret_cast<const std::uint8_t*>(manifest_text.data()),
            reinterpret_cast<const std::uint8_t*>(manifest_text.data() + manifest_text.size())
        );

        // [n bytes] git commits object
        for (const auto& hash : hashes) {
            git_oid oid;
            if (git_oid_fromstr(&oid, hash.c_str()) != 0)
                throw std::runtime_error("Invalid commit hash: " + hash);

            git_commit* commit = nullptr;
            if (git_commit_lookup(&commit, repo, &oid) != 0)
                throw std::runtime_error("Failed to look up commit: " + hash);

            cmt_file commit_file(repo, commit);
            const std::vector<std::uint8_t> commit_bytes = commit_file.commit_file();

            buffer.insert(buffer.end(), commit_bytes.begin(), commit_bytes.end());

            git_commit_free(commit);
        }
    } catch (...) {
        git_repository_free(repo);
        git_libgit2_shutdown();
        throw;
    }

    git_repository_free(repo);
    git_libgit2_shutdown();

    return buffer;
}

bool patchfile::write(const std::string& path) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.good())
        return false;

    const std::vector<std::uint8_t> buffer = generate();
    file.write(
        reinterpret_cast<const char*>(buffer.data()),
        static_cast<std::streamsize>(buffer.size())
    );

    return file.good();
}
