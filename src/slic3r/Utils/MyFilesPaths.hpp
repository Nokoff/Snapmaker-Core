#pragma once

#include <boost/filesystem/path.hpp>
#include <string>

namespace Slic3r {

// File system rules of the home screen's My Files library. Kept free of the GUI so they can be tested,
// since they decide which of the user's files the library may open, move or delete.
namespace MyFilesPaths {

// `rel` as a folder inside `root`, with '/' separators, or empty (the root itself) when it names no such folder.
std::string sanitize_subdir(const boost::filesystem::path& root, const std::string& rel);

// Whether `path` is below `root`, or is it when `allow_equal`. Both are resolved first, so links and ".." cannot lead out.
bool is_inside(const boost::filesystem::path& root, const boost::filesystem::path& path, bool allow_equal);

// Moves a file, replacing an existing one only when `overwrite`. A failed move leaves both files as they were.
bool move_file(const boost::filesystem::path& from, const boost::filesystem::path& to, bool overwrite, std::string& error);

// The part of move_file() used across volumes: the copy is completed beside `to` before it takes its place, and only
// then is `from` removed.
bool move_by_copy(const boost::filesystem::path& from, const boost::filesystem::path& to, std::string& error);

} // namespace MyFilesPaths
} // namespace Slic3r
