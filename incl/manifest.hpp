#pragma once

#include <string>

// Creates (or updates) the user-authored gitm.manifest for the repository at
// `repoPath`. On success the full base commit hash is written into `baseHash`
// and the function returns true; otherwise false.
bool createManifest(const std::string& repoPath, const std::string& manifestPath,
                    std::string& baseHash);

// Reads the base commit hash recorded in the manifest at `manifestPath`.
// On success the hash is stored in `baseHash` and true is returned; false
// otherwise (missing file, invalid TOML or no "base" key).
bool readManifestBase(const std::string& manifestPath, std::string& baseHash);
