#pragma once

#include <cstdint>
#include <git2.h>
#include <optional>
#include <string>
#include <vector>

// per-commit metadata, used to build the per-commit TOML manifest
struct commit_det {
    // git commit
    const git_commit* commit = nullptr;
    std::string hash;

    // the one who wrote the code
    std::string author_name;
    std::string author_email;

    // the one who committed it
    std::string committer_name;
    std::string committer_email;

    // time offset
    std::int64_t timestamp;
    int timezone_offset;

    // signature hash
    std::optional<std::string> signature;

    std::string message; // commit message

    std::vector<std::string> parent_hashes;
};

// serializes a single commit into a self-contained "commit file"
class cmt_file {
  public:
    cmt_file(git_repository* repo, git_commit* commit);
    ~cmt_file();

    cmt_file(const cmt_file&) = delete;
    cmt_file& operator=(const cmt_file&) = delete;

    // TOML manifest describing this commit
    std::string config_file() const;

    // the raw git patch (diff against the first parent, or against an
    // empty tree for a root commit)
    std::vector<std::uint8_t> git_patch() const;

    // fully assembled commit file:
    // [GITM_CMTFF][u32 manifest size][u64 patch size][manifest][patch]
    std::vector<std::uint8_t> commit_file() const;

    const commit_det& details() const { return cmt; }

  private:
    void assign_info(git_commit* commit);

    git_repository* repo = nullptr;
    commit_det cmt;
};
