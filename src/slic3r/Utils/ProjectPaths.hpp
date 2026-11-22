#pragma once

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/path.hpp>

namespace Slic3r {
namespace ProjectPaths {

// The folder Save writes to without asking, for a project named after `file`. Only a 3MF has one: part.stl would
// be saved as part.3mf, over any file of that name. A G-code preview, which cannot be saved, keeps its folder.
inline boost::filesystem::path project_folder(const boost::filesystem::path& file, bool gcode_preview)
{
    if (gcode_preview || boost::algorithm::iends_with(file.extension().string(), ".3mf"))
        return file.parent_path();
    return {};
}

} // namespace ProjectPaths
} // namespace Slic3r
