#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <toml.hpp>
#include <vector>

// A single commit extracted from a .gitm file. The payload of each commit
// (its metadata TOML and its raw git diff) is kept so it can be applied onto
// a repository.
struct parsed_commit {
    std::string hash;
    std::string message;
    std::string author_name;
    std::string author_email;
    std::string committer_name;
    std::string committer_email;
    std::int64_t timestamp = 0;
    int timezone_offset = 0;
    std::optional<std::string> signature;
    std::vector<std::string> parent_hashes;
    std::vector<std::uint8_t> patch;
};

// The parsed contents of a .gitm patch file:
//   [4 bytes] magic "GITM"
//   [n bytes] gitm.manifest (TOML, augmented with base + commits)
//   [n bytes] git commits object (concatenated per-commit files)
struct parsed_patch {
    toml::value manifest;
    std::string base;
    std::vector<parsed_commit> commits;
};

// Reads and parses the .gitm patch file at `path`. On success `out` is filled
// and true returned. On failure false is returned and `error` holds a
// human-readable message.
bool readPatches(const std::string& path, parsed_patch& out, std::string& error);
