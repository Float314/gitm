#include "commits.hpp"

#include <stdexcept>

namespace {

git_oid parseOid(const std::string &hash) {
    git_oid oid;
    if (git_oid_fromstr(&oid, hash.c_str()) != 0)
        throw std::invalid_argument("Invalid commit hash format!");
    return oid;
}

std::string oidToString(const git_oid &oid) {
    char buf[GIT_OID_SHA1_HEXSIZE + 1];
    git_oid_tostr(buf, sizeof(buf), &oid);
    return buf;
}

} // namespace

commit_collect::commit_collect(const std::string &repoPath, const std::string &baseHash)
    : base(parseOid(baseHash)) {
    git_libgit2_init();
    git_repository *opened = nullptr;
    if (git_repository_open(&opened, repoPath.c_str()) != 0) {
        git_libgit2_shutdown();
        throw std::runtime_error("Repository is not valid!");
    }
    repo = opened;
}

commit_collect::~commit_collect() {
    git_repository_free(repo);
    git_libgit2_shutdown();
}

git_revwalk *commit_collect::revwalkFromBase() {
    git_revwalk *walk = nullptr;
    if (git_revwalk_new(&walk, repo) != 0)
        throw std::runtime_error("Failed to create a revwalk");

    git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);

    if (git_revwalk_push_head(walk) != 0) {
        git_revwalk_free(walk);
        return nullptr;
    }

    if (git_revwalk_hide(walk, &base) != 0) {
        git_revwalk_free(walk);
        throw std::runtime_error("Base commit is not in HEAD history");
    }

    return walk;
}

std::vector<std::string> commit_collect::commitsAfter() {
    std::vector<std::string> commits;
    git_revwalk *walk = revwalkFromBase();
    if (!walk)
        return commits;

    git_oid oid;
    while (git_revwalk_next(&oid, walk) == 0)
        commits.push_back(oidToString(oid));

    git_revwalk_free(walk);
    return commits;
}

int commit_collect::countAfter() {
    git_revwalk *walk = revwalkFromBase();
    if (!walk)
        return 0;

    int count = 0;
    git_oid oid;
    while (git_revwalk_next(&oid, walk) == 0)
        ++count;

    git_revwalk_free(walk);
    return count;
}
