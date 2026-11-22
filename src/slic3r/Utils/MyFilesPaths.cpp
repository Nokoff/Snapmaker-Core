#include "MyFilesPaths.hpp"

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace fs = boost::filesystem;

namespace Slic3r {
namespace MyFilesPaths {

namespace {

// A rename that replaces `to` in one step, and fails across volumes instead of copying over it.
void replace_by_rename(const fs::path& from, const fs::path& to, boost::system::error_code& ec)
{
#ifdef _WIN32
    // fs::rename() adds MOVEFILE_COPY_ALLOWED, with which a failed copy has already destroyed the destination
    if (::MoveFileExW(from.wstring().c_str(), to.wstring().c_str(), MOVEFILE_REPLACE_EXISTING))
        ec.clear();
    else
        ec.assign(static_cast<int>(::GetLastError()), boost::system::system_category());
#else
    fs::rename(from, to, ec);
#endif
}

} // namespace

std::string sanitize_subdir(const fs::path& root, const std::string& rel_in)
{
    if (root.empty() || rel_in.empty())
        return {};

    std::string rel = rel_in;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    if (rel.front() == '/')
        return {};

    std::vector<std::string> parts;
    boost::algorithm::split(parts, rel, boost::algorithm::is_any_of("/"));

    fs::path relative;
    for (const std::string& part : parts) {
        if (part.empty() || part == ".")
            continue;
        if (part == "..")
            return {}; // Never walk above the mapped root.
#ifdef _WIN32
        // A drive, or an alternate data stream
        if (part.find(':') != std::string::npos)
            return {};
#endif
        relative /= part;
    }
    if (relative.empty() || relative.is_absolute() || relative.has_root_name())
        return {};

    boost::system::error_code ec;
    if (!fs::is_directory(root / relative, ec) || ec)
        return {};
    return relative.generic_string();
}

bool is_inside(const fs::path& root_in, const fs::path& path_in, bool allow_equal)
{
    if (root_in.empty() || path_in.empty())
        return false;

    boost::system::error_code ec;
    const fs::path root = fs::canonical(root_in, ec);
    if (ec)
        return false;
    const fs::path path = fs::canonical(path_in, ec);
    if (ec)
        return false;

    auto path_it = path.begin();
    for (const fs::path& part : root) {
        // What a trailing separator on the mapped folder leaves behind
        if (part.empty() || part == ".")
            continue;
        if (path_it == path.end() || *path_it != part)
            return false;
        ++path_it;
    }
    return allow_equal || path_it != path.end();
}

bool move_by_copy(const fs::path& from, const fs::path& to, std::string& error)
{
    boost::system::error_code ec;
    const fs::path temporary = to.parent_path() / fs::unique_path(".%%%%-%%%%-%%%%.moving", ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    const auto give_up = [&] {
        error = ec.message();
        boost::system::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    };

    fs::copy_file(from, temporary, ec);
    if (ec)
        return give_up();

    // The library sorts by date, so a moved file keeps its own
    boost::system::error_code time_ec;
    const std::time_t modified = fs::last_write_time(from, time_ec);
    if (!time_ec)
        fs::last_write_time(temporary, modified, time_ec);

    replace_by_rename(temporary, to, ec);
    if (ec)
        return give_up();

    fs::remove(from, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

bool move_file(const fs::path& from, const fs::path& to, bool overwrite, std::string& error)
{
    boost::system::error_code ec;
    if (!fs::is_regular_file(from, ec)) {
        error = "The source is not a file";
        return false;
    }

    boost::system::error_code status_ec;
    const fs::file_status     target = fs::symlink_status(to, status_ec);
    if (fs::exists(target)) {
        if (fs::is_directory(target) || !overwrite) {
            error = "The destination already exists";
            return false;
        }
        if (fs::equivalent(from, to, ec) && !ec) {
            error = "The source and the destination are the same file";
            return false;
        }
    }

    replace_by_rename(from, to, ec);
    if (!ec)
        return true;

#ifdef _WIN32
    const bool other_volume = ec.value() == ERROR_NOT_SAME_DEVICE;
#else
    const bool other_volume = ec == boost::system::errc::cross_device_link;
#endif
    if (!other_volume) {
        error = ec.message();
        return false;
    }
    return move_by_copy(from, to, error);
}

} // namespace MyFilesPaths
} // namespace Slic3r
