#include "gitm_apply.hpp"

#include <git2.h>

#include <cstring>
#include <string>
#include <vector>

#include "patch_reader.hpp"

namespace {

std::string oidToString(const git_oid& oid) {
    char buf[GIT_OID_SHA1_HEXSIZE + 1];
    git_oid_tostr(buf, sizeof(buf), &oid);
    return buf;
}

git_oid parseOid(const std::string& hash) {
    git_oid oid;
    if (git_oid_fromstr(&oid, hash.c_str()) != 0)
        throw std::runtime_error("Invalid commit hash in patch: " + hash);
    return oid;
}

// RAII-style commit pointer guard
struct commit_guard {
    git_commit* c = nullptr;
    ~commit_guard() {
        if (c)
            git_commit_free(c);
    }
    commit_guard() = default;
    commit_guard(const commit_guard&) = delete;
    commit_guard& operator=(const commit_guard&) = delete;
};

void checkoutTarget(git_repository* repo, git_object* target) {
    if (git_checkout_tree(repo, target, nullptr) != 0)
        throw std::runtime_error("Failed to check out tree");
}

// Creates an author/committer signature from a patch commit's metadata.
void makeSignature(git_signature** out, const parsed_commit& c) {
    const std::string name =
        c.author_name.empty() ? c.committer_name.empty() ? "gitm" : c.committer_name
                              : c.author_name;
    const std::string email =
        c.author_email.empty() ? c.committer_email.empty() ? "gitm@localhost" : c.committer_email
                               : c.author_email;

    git_time_t t = c.timestamp > 0 ? static_cast<git_time_t>(c.timestamp) : 0;
    if (git_signature_new(out, name.c_str(), email.c_str(), t, c.timezone_offset) != 0)
        throw std::runtime_error("Failed to build commit signature");
}

} // namespace

bool gitm_accept(const parsed_patch& patch, const std::string& repoPath,
                 const std::string& suffix, std::string& error) {
    if (patch.commits.empty()) {
        error = "Patch contains no commits to apply";
        return false;
    }
    if (patch.base.empty()) {
        error = "Patch does not carry a base commit";
        return false;
    }

    git_libgit2_init();
    git_repository* repo = nullptr;
    if (git_repository_open(&repo, repoPath.c_str()) != 0) {
        git_libgit2_shutdown();
        error = "Repository is not valid: " + repoPath;
        return false;
    }

    std::string branchFull;
    const std::string branchShort = "gitm/" + suffix;
    std::string origBranch;

    try {
        // resolve the base commit
        const git_oid base_oid = parseOid(patch.base);
        commit_guard base;
        if (git_commit_lookup(&base.c, repo, &base_oid) != 0)
            throw std::runtime_error("Base commit not found in repository: " + patch.base.substr(0, 8));

        // resolve the current HEAD reference + commit
        git_reference* head_ref = nullptr;
        if (git_repository_head(&head_ref, repo) != 0)
            throw std::runtime_error("Repository has no HEAD");
        const char* head_shorthand = git_reference_shorthand(head_ref);
        origBranch = head_shorthand ? head_shorthand : "HEAD";

        commit_guard head_commit;
        git_object* head_obj = nullptr;
        if (git_reference_peel(&head_obj, head_ref, GIT_OBJECT_COMMIT) != 0) {
            git_reference_free(head_ref);
            throw std::runtime_error("HEAD does not point to a commit");
        }
        head_commit.c = reinterpret_cast<git_commit*>(head_obj);
        git_reference_free(head_ref);

        branchFull = "refs/heads/" + branchShort;

        // create the new branch off the base commit
        git_reference* branch_ref = nullptr;
        if (git_branch_create(&branch_ref, repo, branchShort.c_str(), base.c, 1) != 0) {
            throw std::runtime_error("Failed to create branch " + branchShort);
        }
        git_reference_free(branch_ref);

        // check out the new branch
        checkoutTarget(repo, reinterpret_cast<git_object*>(base.c));
        if (git_repository_set_head(repo, branchFull.c_str()) != 0)
            throw std::runtime_error("Failed to check out branch " + branchShort);

        // apply each commit on top of the previous one
        commit_guard tip;
        if (git_commit_lookup(&tip.c, repo, &base_oid) != 0)
            throw std::runtime_error("Failed to resolve tip");

        for (const parsed_commit& c : patch.commits) {
            if (c.patch.empty())
                throw std::runtime_error("Commit has no patch data: " + c.hash);

            git_diff* diff = nullptr;
            if (git_diff_from_buffer(&diff, reinterpret_cast<const char*>(c.patch.data()),
                                     c.patch.size()) != 0)
                throw std::runtime_error("Failed to parse patch for commit " + c.hash);

            const int applyResult =
                git_apply(repo, diff, GIT_APPLY_LOCATION_BOTH, nullptr);
            git_diff_free(diff);
            if (applyResult != 0) {
                const git_error* err = git_error_last();
                throw std::runtime_error("Failed to apply patch for commit " + c.hash +
                                         (err && err->message ? std::string(": ") + err->message
                                                              : std::string("")));
            }

            // write the resulting tree
            git_index* index = nullptr;
            if (git_repository_index(&index, repo) != 0)
                throw std::runtime_error("Failed to open index");
            git_oid tree_oid;
            if (git_index_write_tree(&tree_oid, index) != 0) {
                git_index_free(index);
                throw std::runtime_error("Failed to write tree for commit " + c.hash);
            }
            git_index_free(index);

            git_tree* tree = nullptr;
            if (git_tree_lookup(&tree, repo, &tree_oid) != 0)
                throw std::runtime_error("Failed to look up tree");

            git_signature* author = nullptr;
            makeSignature(&author, c);
            git_signature* committer = nullptr;
            const std::string committerName =
                c.committer_name.empty() ? c.author_name : c.committer_name;
            const std::string committerEmail =
                c.committer_email.empty() ? c.author_email : c.committer_email;
            if (c.timestamp > 0) {
                if (git_signature_new(&committer, committerName.c_str(),
                                      committerEmail.empty() ? "gitm@localhost" : committerEmail.c_str(),
                                      static_cast<git_time_t>(c.timestamp), c.timezone_offset) != 0) {
                    git_signature_free(author);
                    git_tree_free(tree);
                    throw std::runtime_error("Failed to build committer signature");
                }
            } else {
                if (git_signature_now(&committer, committerName.c_str(),
                                      committerEmail.empty() ? "gitm@localhost" : committerEmail.c_str()) != 0) {
                    git_signature_free(author);
                    git_tree_free(tree);
                    throw std::runtime_error("Failed to build committer signature");
                }
            }

            const git_commit* parents[] = {tip.c};
            git_oid new_oid;
            const int createResult = git_commit_create(
                &new_oid, repo, branchFull.c_str(), author, committer, "UTF-8",
                c.message.empty() ? "gitm patch commit" : c.message.c_str(), tree, 1, parents
            );

            git_signature_free(author);
            git_signature_free(committer);
            git_tree_free(tree);

            if (createResult != 0)
                throw std::runtime_error("Failed to create commit " + c.hash);

            // advance tip
            git_commit_free(tip.c);
            tip.c = nullptr;
            if (git_commit_lookup(&tip.c, repo, &new_oid) != 0)
                throw std::runtime_error("Failed to advance branch");
        }

        // return to the original branch
        checkoutTarget(repo, reinterpret_cast<git_object*>(head_commit.c));
        {
            git_reference* h = nullptr;
            if (git_repository_head(&h, repo) != 0)
                throw std::runtime_error("Failed to resolve HEAD");
            const std::string full = "refs/heads/" + origBranch;
            if (git_repository_set_head(repo, full.c_str()) != 0) {
                git_reference_free(h);
                throw std::runtime_error("Failed to restore HEAD to " + origBranch);
            }
            git_reference_free(h);
        }

        // merge the temporary branch into HEAD
        const git_oid* tip_oid = git_commit_id(tip.c);
        git_annotated_commit* theirs = nullptr;
        if (git_annotated_commit_lookup(&theirs, repo, tip_oid) != 0)
            throw std::runtime_error("Failed to resolve branch for merge");

        git_merge_analysis_t analysis;
        git_merge_preference_t pref;
        const git_annotated_commit* their_ptr = theirs;
        if (git_merge_analysis(&analysis, &pref, repo, &their_ptr, 1) != 0) {
            git_annotated_commit_free(theirs);
            throw std::runtime_error("Failed to analyse merge");
        }

        int mergeStatus = 0;
        if (analysis & GIT_MERGE_ANALYSIS_NORMAL) {
            const git_annotated_commit* their_heads[] = {theirs};
            mergeStatus = git_merge(repo, their_heads, 1, nullptr, nullptr);
        }
        git_annotated_commit_free(theirs);

        if (mergeStatus != 0)
            throw std::runtime_error("Merge failed due to conflicts or errors");

        // check for conflicts
        git_index* index = nullptr;
        git_repository_index(&index, repo);
        const bool conflicts = index && git_index_has_conflicts(index) != 0;
        if (conflicts) {
            git_index_free(index);
            git_repository_state_cleanup(repo);
            throw std::runtime_error("Merge produced conflicts; resolve and commit manually");
        }

        git_oid merge_tree_oid;
        if (git_index_write_tree(&merge_tree_oid, index) != 0) {
            git_index_free(index);
            git_repository_state_cleanup(repo);
            throw std::runtime_error("Failed to write merge tree");
        }
        git_index_free(index);

        git_tree* merge_tree = nullptr;
        if (git_tree_lookup(&merge_tree, repo, &merge_tree_oid) != 0) {
            git_repository_state_cleanup(repo);
            throw std::runtime_error("Failed to look up merge tree");
        }

        // create the merge commit
        git_signature* sig = nullptr;
        git_signature_now(&sig, "gitm", "gitm@localhost");

        const git_commit* mergeParents[2] = {head_commit.c, tip.c};
        git_oid merge_oid;
        const std::string msg = "Merge gitm branch " + branchShort;
        const int cr = git_commit_create(&merge_oid, repo, nullptr, sig, sig, "UTF-8",
                                         msg.c_str(), merge_tree, 2, mergeParents);

        git_signature_free(sig);
        git_tree_free(merge_tree);
        git_repository_state_cleanup(repo);

        if (cr != 0)
            throw std::runtime_error("Failed to create merge commit");

        // point the original branch at the merge commit
        git_reference* orig_ref = nullptr;
        if (git_branch_lookup(&orig_ref, repo, origBranch.c_str(), GIT_BRANCH_LOCAL) == 0) {
            git_reference_set_target(&orig_ref, orig_ref, &merge_oid, "gitm: accept merge");
            git_reference_free(orig_ref);
        }

        // discard the temporary branch
        git_reference* tmp_ref = nullptr;
        if (git_branch_lookup(&tmp_ref, repo, branchShort.c_str(), GIT_BRANCH_LOCAL) == 0) {
            git_branch_delete(tmp_ref);
            git_reference_free(tmp_ref);
        }
    } catch (const std::exception& e) {
        // best-effort rollback: return to the original branch if we had
        // switched to the temporary one, then remove the temporary branch.
        if (!origBranch.empty()) {
            git_reference* head_ref = nullptr;
            if (git_repository_head(&head_ref, repo) == 0) {
                const char* cur = git_reference_shorthand(head_ref);
                const std::string current = cur ? cur : "";
                git_reference_free(head_ref);
                if (!current.empty() && current != origBranch) {
                    const std::string full = "refs/heads/" + origBranch;
                    git_repository_set_head(repo, full.c_str());
                }
            }
        }
        git_reference* tmp = nullptr;
        if (git_branch_lookup(&tmp, repo, branchShort.c_str(), GIT_BRANCH_LOCAL) == 0) {
            git_branch_delete(tmp);
            git_reference_free(tmp);
        }
        error = e.what();
        git_repository_free(repo);
        git_libgit2_shutdown();
        return false;
    }

    git_repository_free(repo);
    git_libgit2_shutdown();
    return true;
}

bool gitm_discard(const std::string& repoPath, const std::string& name, std::string& error) {
    git_libgit2_init();
    git_repository* repo = nullptr;
    if (git_repository_open(&repo, repoPath.c_str()) != 0) {
        git_libgit2_shutdown();
        error = "Repository is not valid: " + repoPath;
        return false;
    }

    const std::string branch = "gitm/" + name;
    git_reference* ref = nullptr;
    const int lookup = git_branch_lookup(&ref, repo, branch.c_str(), GIT_BRANCH_LOCAL);
    if (lookup == GIT_ENOTFOUND) {
        git_repository_free(repo);
        git_libgit2_shutdown();
        error = "Branch " + branch + " does not exist";
        return false;
    }
    if (lookup != 0) {
        git_repository_free(repo);
        git_libgit2_shutdown();
        error = "Failed to look up branch " + branch;
        return false;
    }

    const int del = git_branch_delete(ref);
    git_reference_free(ref);
    git_repository_free(repo);
    git_libgit2_shutdown();

    if (del != 0) {
        error = "Failed to delete branch " + branch;
        return false;
    }
    return true;
}
