#pragma once

#include <git2.h>
#include <string>
#include <vector>

class commit_collect {
  public:
    commit_collect(const std::string &repo, const std::string &baseHash);
    ~commit_collect();

    commit_collect(const commit_collect &) = delete;
    commit_collect &operator=(const commit_collect &) = delete;

    std::vector<std::string> commitsAfter();
    int countAfter();

  private:
    git_revwalk *revwalkFromBase();

    git_repository *repo = nullptr;
    git_oid base;
};
