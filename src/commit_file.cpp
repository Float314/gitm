#include "commit_file.hpp"

#include <git2/buffer.h>
#include <git2/commit.h>
#include <git2/diff.h>
#include <git2/errors.h>
#include <git2/oid.h>
#include <git2/tree.h>
#include <git2/types.h>
#include <stdexcept>
#include <string>
#include <toml.hpp>
#include <vector>

/* Commit files are basically individual metadata of commits and all. The commit files are later to be 
   combined and shrunk their sizes using zlib. Commit files themselves to not be shrunk. Eg. 
   
   Commit -> commit file -> given to .gitm generator -> combine all git commit files and metadata -> generate gitm file*/

cmt_file::cmt_file(git_repository* repo, git_commit* commit) : repo(repo) {
    assign_info(commit);
}

cmt_file::~cmt_file() = default;

void cmt_file::assign_info(git_commit* commit) {
    cmt.commit = commit;

    // hash
    const git_oid* oid = git_commit_id(commit);
    char hash[GIT_OID_HEXSZ + 1];
    git_oid_tostr(hash, sizeof(hash), oid);
    cmt.hash = hash;

    // author name and email
    const git_signature* author = git_commit_author(commit);
    cmt.author_name = author && author->name ? author->name : "";
    cmt.author_email = author && author->email ? author->email : "";

    // committer name and email
    const git_signature* committer = git_commit_committer(commit);
    cmt.committer_name = committer && committer->name ? committer->name : "";
    cmt.committer_email = committer && committer->email ? committer->email : "";

    // time and its offset
    cmt.timestamp = committer ? committer->when.time : 0;
    /* we use commit time here cause it is relevant */
    cmt.timezone_offset = committer ? committer->when.offset : 0;

    // commit message
    const char* message = git_commit_message(commit);
    cmt.message = message ? message : "";

    // parent hashes
    const size_t parent_hash_cnt = git_commit_parentcount(commit);
    for (size_t i = 0; i < parent_hash_cnt; ++i) {
        git_commit* parent = nullptr;
        if (git_commit_parent(&parent, commit, i) != 0)
            continue;

        const git_oid* parent_oid = git_commit_id(parent);
        char parent_hash[GIT_OID_HEXSZ + 1];
        git_oid_tostr(parent_hash, sizeof(parent_hash), parent_oid);

        cmt.parent_hashes.push_back(parent_hash);

        git_commit_free(parent);
    }

    // signature -
    git_buf signature = GIT_BUF_INIT;
    git_buf sign_data = GIT_BUF_INIT;

    // const-ness workaround: extract_signature takes a non-const oid
    const git_oid* commit_oid = git_commit_id(commit);
    git_oid commit_oid_copy = *commit_oid;

    const int error =
        git_commit_extract_signature(&signature, &sign_data, repo, &commit_oid_copy, nullptr);

    if (error == 0) {
        cmt.signature = signature.ptr ? signature.ptr : "";
    } else if (error == GIT_ENOTFOUND) {
        cmt.signature = std::nullopt;
    } else {
        git_buf_dispose(&signature);
        git_buf_dispose(&sign_data);
        throw std::invalid_argument("Signature is NOT Valid!");
    }

    git_buf_dispose(&signature);
    git_buf_dispose(&sign_data);
}

std::string cmt_file::config_file() const {
    toml::value root;

    root["commit"]["hash"] = cmt.hash;

    root["author"]["name"] = cmt.author_name;
    root["author"]["email"] = cmt.author_email;

    root["committer"]["name"] = cmt.committer_name;
    root["committer"]["email"] = cmt.committer_email;

    root["time"]["timestamp"] = cmt.timestamp;
    root["time"]["timezone_offset"] = cmt.timezone_offset;

    root["signature"]["present"] = cmt.signature.has_value();

    if (cmt.signature.has_value()) {
        root["signature"]["value"] = *cmt.signature;
    }

    root["message"]["text"] = cmt.message;

    toml::array parents;

    for (const auto& hash : cmt.parent_hashes) {
        parents.push_back(hash);
    }

    root["parents"]["hashes"] = std::move(parents);

    return toml::format(root);
}

std::vector<std::uint8_t> cmt_file::git_patch() const {
    git_tree* tree = nullptr;
    git_tree* parent_tree = nullptr;

    if (git_commit_tree(&tree, cmt.commit) != 0)
        throw std::runtime_error("Failed to load commit tree");

    git_commit* parent = nullptr;
    const int parent_error = git_commit_parent(&parent, cmt.commit, 0);

    if (parent_error == 0) {
        if (git_commit_tree(&parent_tree, parent) != 0) {
            git_commit_free(parent);
            git_tree_free(tree);
            throw std::runtime_error("Failed to load parent tree");
        }
        git_commit_free(parent);
    } else {
        // root commit - diff against an empty tree
        git_treebuilder* builder = nullptr;
        if (git_treebuilder_new(&builder, repo, nullptr) != 0) {
            git_tree_free(tree);
            throw std::runtime_error("Failed to create empty tree");
        }

        git_oid empty_oid;
        const int write_error = git_treebuilder_write(&empty_oid, builder);
        git_treebuilder_free(builder);

        if (write_error != 0) {
            git_tree_free(tree);
            throw std::runtime_error("Failed to write empty tree");
        }

        if (git_tree_lookup(&parent_tree, repo, &empty_oid) != 0) {
            git_tree_free(tree);
            throw std::runtime_error("Failed to look up empty tree");
        }
    }

    git_diff* diff = nullptr;
    if (git_diff_tree_to_tree(&diff, repo, parent_tree, tree, nullptr) != 0) {
        git_tree_free(tree);
        git_tree_free(parent_tree);
        throw std::runtime_error("Failed to generate diff");
    }

    git_buf buf = GIT_BUF_INIT;
    if (git_diff_to_buf(&buf, diff, GIT_DIFF_FORMAT_PATCH) != 0) {
        git_diff_free(diff);
        git_tree_free(tree);
        git_tree_free(parent_tree);
        throw std::runtime_error("Failed to render diff");
    }

    std::vector<std::uint8_t> result(
        reinterpret_cast<const std::uint8_t*>(buf.ptr),
        reinterpret_cast<const std::uint8_t*>(buf.ptr) + buf.size
    );

    git_buf_dispose(&buf);
    git_diff_free(diff);
    git_tree_free(tree);
    git_tree_free(parent_tree);

    return result;
}

std::vector<std::uint8_t> cmt_file::commit_file() const {
    const char magic[] = "GITM_CMTFF"; // magic number

    const std::string manifest = config_file();
    const std::vector<std::uint8_t> patch = git_patch();

    std::vector<std::uint8_t> buffer;
    buffer.reserve(
        sizeof(magic) - 1 + sizeof(std::uint32_t) + sizeof(std::uint64_t) +
        manifest.size() + patch.size()
    );

    const auto appendBytes = [&buffer](const std::uint8_t* data, const size_t len) {
        buffer.insert(buffer.end(), data, data + len);
    };

    // magic
    appendBytes(reinterpret_cast<const std::uint8_t*>(magic), sizeof(magic) - 1);

    // append manifest size
    const std::uint32_t manifest_size = static_cast<std::uint32_t>(manifest.size());
    appendBytes(reinterpret_cast<const std::uint8_t*>(&manifest_size), sizeof(manifest_size));

    // append patch size
    const std::uint64_t patch_size = static_cast<std::uint64_t>(patch.size());
    appendBytes(reinterpret_cast<const std::uint8_t*>(&patch_size), sizeof(patch_size));

    // append manifest
    appendBytes(reinterpret_cast<const std::uint8_t*>(manifest.data()), manifest.size());

    // append patch
    appendBytes(patch.data(), patch.size());

    return buffer;
}
