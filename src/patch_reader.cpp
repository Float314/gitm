#include "patch_reader.hpp"

#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace {

constexpr char kMagic[] = "GITM";
constexpr char kCommitMagic[] = "GITM_CMTFF";

std::vector<std::uint8_t> readBinary(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.good())
        throw std::runtime_error("Failed to open file: " + path);

    std::vector<std::uint8_t> data;
    file.seekg(0, std::ios::end);
    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    data.resize(static_cast<std::size_t>(size));
    if (size > 0)
        file.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

// Returns the index of the first occurrence of `needle` in `haystack`
// starting at `from`, or npos when absent.
std::size_t findBytes(const std::vector<std::uint8_t>& haystack, const char* needle,
                      std::size_t from) {
    const std::size_t needleLen = std::strlen(needle);
    if (needleLen == 0 || haystack.size() < needleLen || from > haystack.size() - needleLen)
        return std::string::npos;

    for (std::size_t i = from; i <= haystack.size() - needleLen; ++i) {
        if (std::memcmp(haystack.data() + i, needle, needleLen) == 0)
            return i;
    }
    return std::string::npos;
}

template <typename T>
T readLE(const std::vector<std::uint8_t>& data, std::size_t& pos) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (pos + sizeof(T) > data.size())
        throw std::runtime_error("Truncated gitm file while reading header");
    T value = 0;
    std::memcpy(&value, data.data() + pos, sizeof(T));
    pos += sizeof(T);
    return value;
}

std::string readString(const std::vector<std::uint8_t>& data, std::size_t& pos,
                       std::size_t length) {
    if (pos + length > data.size())
        throw std::runtime_error("Truncated gitm file while reading string");
    std::string out(reinterpret_cast<const char*>(data.data() + pos), length);
    pos += length;
    return out;
}

toml::value parseManifestText(const std::string& text, const std::string& label) {
    std::istringstream stream(text);
    try {
        return toml::parse(stream, label);
    } catch (const std::exception&) {
        throw std::runtime_error("Failed to parse gitm manifest (" + label + ")");
    }
}

std::string asString(const toml::value& v, const std::string& fallback = "") {
    if (v.is_string())
        return v.as_string();
    return fallback;
}

} // namespace

bool readPatches(const std::string& path, parsed_patch& out, std::string& error) {
    try {
        const std::vector<std::uint8_t> data = readBinary(path);

        // validate the top-level magic
        if (data.size() < sizeof(kMagic) - 1 ||
            std::memcmp(data.data(), kMagic, sizeof(kMagic) - 1) != 0) {
            error = "File is not a valid gitm patch (bad magic)";
            return false;
        }

        // The manifest runs until the first per-commit file, which is always
        // introduced by the per-commit magic.
        const std::size_t commitMagicPos = findBytes(data, kCommitMagic, sizeof(kMagic) - 1);
        if (commitMagicPos == std::string::npos) {
            error = "Patch contains no commit data";
            return false;
        }

        const std::string manifestText(
            reinterpret_cast<const char*>(data.data() + sizeof(kMagic) - 1),
            commitMagicPos - (sizeof(kMagic) - 1)
        );

        toml::value manifest = parseManifestText(manifestText, path);

        out.manifest = manifest;
        if (manifest.contains("base") && manifest["base"].is_string())
            out.base = manifest["base"].as_string();

        // parse the concatenated per-commit files
        std::size_t pos = commitMagicPos;
        while (pos < data.size()) {
            if (data.size() - pos < sizeof(kCommitMagic) - 1) {
                error = "Trailing garbage in gitm file";
                return false;
            }
            if (std::memcmp(data.data() + pos, kCommitMagic, sizeof(kCommitMagic) - 1) != 0) {
                error = "Corrupt commit header in gitm file";
                return false;
            }
            pos += sizeof(kCommitMagic) - 1;

            const std::uint32_t manifestSize = readLE<std::uint32_t>(data, pos);
            const std::uint64_t patchSize = readLE<std::uint64_t>(data, pos);

            const std::string commitManifest = readString(data, pos, manifestSize);

            if (patchSize > data.size() - pos)
                throw std::runtime_error("Truncated gitm file while reading patch");

            const std::vector<std::uint8_t> patch(
                data.data() + pos, data.data() + pos + static_cast<std::size_t>(patchSize)
            );
            pos += static_cast<std::size_t>(patchSize);

            parsed_commit commit;
            const toml::value parsed = parseManifestText(commitManifest, path);

            auto tableField = [&parsed](const char* table, const char* field) -> toml::value {
                if (parsed.contains(table) && parsed.at(table).is_table()) {
                    const toml::table& t = parsed.at(table).as_table();
                    if (t.contains(field))
                        return t.at(field);
                }
                return toml::value();
            };

            commit.hash = asString(tableField("commit", "hash"));
            commit.message = asString(tableField("message", "text"));
            commit.author_name = asString(tableField("author", "name"));
            commit.author_email = asString(tableField("author", "email"));
            commit.committer_name = asString(tableField("committer", "name"));
            commit.committer_email = asString(tableField("committer", "email"));

            const toml::value time = tableField("time", "timestamp");
            if (time.is_integer())
                commit.timestamp = time.as_integer();
            const toml::value tz = tableField("time", "timezone_offset");
            if (tz.is_integer())
                commit.timezone_offset = static_cast<int>(tz.as_integer());

            if (parsed.contains("signature") && parsed.at("signature").is_table()) {
                const toml::table& s = parsed.at("signature").as_table();
                if (s.contains("present") && s.at("present").as_boolean())
                    commit.signature = asString(tableField("signature", "value"));
            }

            if (parsed.contains("parents") && parsed.at("parents").is_table()) {
                const toml::table& p = parsed.at("parents").as_table();
                if (p.contains("hashes") && p.at("hashes").is_array()) {
                    for (const auto& h : p.at("hashes").as_array())
                        commit.parent_hashes.push_back(asString(h));
                }
            }

            commit.patch = std::move(patch);
            out.commits.push_back(std::move(commit));
        }

        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
