#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>

#include <boost/filesystem/path.hpp>

#include "slic3r/Utils/ProjectPaths.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

TEST_CASE("A model opened as a project gives Save no file to overwrite", "[ProjectPaths][Regression]")
{
    const std::string name = GENERATE(std::string("part.stl"), std::string("part.STL"), std::string("part.step"),
                                      std::string("part.obj"), std::string("part.zip.amf"), std::string("part.zip"),
                                      std::string("part.3mf.stl"), std::string("part"));
    CAPTURE(name);

    CHECK(ProjectPaths::project_folder(fs::path("models") / name, false).empty());
}

TEST_CASE("A 3MF project is saved beside where it was opened", "[ProjectPaths]")
{
    const std::string name = GENERATE(std::string("part.3mf"), std::string("part.3MF"), std::string("part.gcode.3mf"));
    CAPTURE(name);

    CHECK(ProjectPaths::project_folder(fs::path("models") / name, false) == fs::path("models"));
}

TEST_CASE("A G-code preview keeps the folder it was opened from", "[ProjectPaths]")
{
    CHECK(ProjectPaths::project_folder(fs::path("models") / "part.gcode", true) == fs::path("models"));
}

TEST_CASE("An untitled project has no folder", "[ProjectPaths]")
{
    const std::string name = GENERATE(std::string(), std::string("Untitled"));
    CAPTURE(name);

    CHECK(ProjectPaths::project_folder(fs::path(name), false).empty());
}
