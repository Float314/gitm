#pragma once

#include <string>

#include "patch_reader.hpp"

// Applies the commits from a parsed .gitm patch onto the repository at
// `repoPath` by:
//   1. creating a new local branch `gitm/<suffix>` off the patch's base commit
//   2. applying each commit in the patch onto that branch
//   3. merging the branch into the original HEAD
//   4. cleaning up the temporary branch
// Returns true on success; on failure false and `error` is filled.
bool gitm_accept(const parsed_patch& patch, const std::string& repoPath,
                 const std::string& suffix, std::string& error);

// Deletes the local branch `gitm/<name>` that was created for a patch.
// Returns true when removed, false if it did not exist.
bool gitm_discard(const std::string& repoPath, const std::string& name, std::string& error);
