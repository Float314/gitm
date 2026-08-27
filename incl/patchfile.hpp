#pragma once

#include <cstdint>
#include <string>
#include <vector>

// builds the top-level gitm patch file:
// [4 bytes] magic "GITM"
// [n bytes] gitm.manifest (TOML, augmented with the collected commits)
// [n bytes] git commits object (concatenated per-commit files)
class patchfile {
  public:
    patchfile(const std::string& repoPath, const std::string& baseHash,
              const std::string& manifestPath);

    patchfile(const patchfile&) = delete;
    patchfile& operator=(const patchfile&) = delete;

    // generate the full patch file bytes
    std::vector<std::uint8_t> generate() const;

    // generate and write to `path`; returns false on I/O failure
    bool write(const std::string& path) const;

    const std::string& repoPath() const { return repoPath_; }
    const std::string& baseHash() const { return baseHash_; }
    const std::string& manifestPath() const { return manifestPath_; }

  private:
    std::string repoPath_;
    std::string baseHash_;
    std::string manifestPath_;
};
