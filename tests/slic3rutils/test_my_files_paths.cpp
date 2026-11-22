#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <fstream>
#include <iterator>
#include <string>

#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "slic3r/Utils/MyFilesPaths.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

void put(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    fs::ofstream out(path, std::ios::binary);
    out << content;
}

std::string get(const fs::path& path)
{
    fs::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int entries_in(const fs::path& dir)
{
    return static_cast<int>(std::distance(fs::directory_iterator(dir), fs::directory_iterator()));
}

} // namespace

TEST_CASE("A My Files folder request resolves to a folder inside the library", "[MyFilesPaths]")
{
    ScopedTemporaryDir temp("my-files");
    const fs::path     root = fs::path(temp.string()) / "library";
    fs::create_directories(root / "parts" / "small");

    CHECK(MyFilesPaths::sanitize_subdir(root, "parts") == "parts");
    CHECK(MyFilesPaths::sanitize_subdir(root, "parts/small") == "parts/small");
    CHECK(MyFilesPaths::sanitize_subdir(root, "parts\\small\\") == "parts/small");
}

TEST_CASE("A My Files folder request that leaves the library falls back to its root", "[MyFilesPaths]")
{
    ScopedTemporaryDir temp("my-files");
    const fs::path     base = fs::path(temp.string());
    fs::create_directories(base / "library" / "parts");
    fs::create_directories(base / "outside");

    const std::string request = GENERATE(std::string(".."), std::string("../outside"), std::string("parts/../../outside"),
                                         std::string("..\\outside"), std::string("/outside"), std::string("\\\\server\\share"),
                                         std::string("C:\\Windows"), std::string("missing"));
    CAPTURE(request);

    CHECK(MyFilesPaths::sanitize_subdir(base / "library", request).empty());
}

TEST_CASE("My Files treats only paths below the mapped folder as its own", "[MyFilesPaths]")
{
    ScopedTemporaryDir temp("my-files");
    const fs::path     base = fs::path(temp.string());
    const fs::path     root = base / "library";
    put(root / "parts" / "a.stl", "a");
    put(base / "library-backup" / "b.stl", "b");
    put(base / "c.stl", "c");

    CHECK(MyFilesPaths::is_inside(root, root / "parts" / "a.stl", false));
    CHECK_FALSE(MyFilesPaths::is_inside(root, base / "library-backup" / "b.stl", false));
    CHECK_FALSE(MyFilesPaths::is_inside(root, root / ".." / "c.stl", false));
    CHECK_FALSE(MyFilesPaths::is_inside(root, root / "missing.stl", false));
    CHECK_FALSE(MyFilesPaths::is_inside(fs::path(), root / "parts" / "a.stl", false));
}

TEST_CASE("My Files recognises its folder however the mapping ends", "[MyFilesPaths]")
{
    ScopedTemporaryDir temp("my-files");
    const fs::path     root = fs::path(temp.string()) / "library";
    put(root / "a.stl", "a");
    // The ending is generated rather than the path, which is another temporary folder on every run
    const std::string ending = GENERATE(std::string(), std::string("/"), std::string("/."));
    const fs::path    mapped(root.string() + ending);
    CAPTURE(mapped.string());

    CHECK(MyFilesPaths::is_inside(mapped, root / "a.stl", false));
    CHECK(MyFilesPaths::is_inside(mapped, root, true));
}

TEST_CASE("My Files accepts a whole drive as its folder", "[MyFilesPaths]")
{
    ScopedTemporaryDir temp("my-files");
    const fs::path     file = fs::path(temp.string()) / "a.stl";
    put(file, "a");

    CHECK(MyFilesPaths::is_inside(fs::canonical(file).root_path(), file, false));
}

TEST_CASE("My Files moves a file to a free name", "[MyFilesPaths]")
{
    ScopedTemporaryDir temp("my-files");
    const fs::path     from = fs::path(temp.string()) / "from" / "a.stl";
    const fs::path     to   = fs::path(temp.string()) / "to" / "a.stl";
    put(from, "model");
    fs::create_directories(to.parent_path());

    std::string error;
    REQUIRE(MyFilesPaths::move_file(from, to, false, error));

    CHECK_FALSE(fs::exists(from));
    CHECK(get(to) == "model");
}

TEST_CASE("My Files replaces an existing file only when told to", "[MyFilesPaths]")
{
    const bool overwrite = GENERATE(false, true);
    CAPTURE(overwrite);
    ScopedTemporaryDir temp("my-files");
    const fs::path     from = fs::path(temp.string()) / "from" / "a.stl";
    const fs::path     to   = fs::path(temp.string()) / "to" / "a.stl";
    put(from, "new");
    put(to, "old");

    std::string error;
    CHECK(MyFilesPaths::move_file(from, to, overwrite, error) == overwrite);

    CHECK(get(to) == (overwrite ? "new" : "old"));
    CHECK(fs::exists(from) != overwrite);
}

TEST_CASE("A failed My Files move leaves the file it would have replaced", "[MyFilesPaths][Regression]")
{
    ScopedTemporaryDir temp("my-files");
    const fs::path     base = fs::path(temp.string());
    put(base / "to" / "a.stl", "precious");

    std::string error;
    CHECK_FALSE(MyFilesPaths::move_file(base / "from" / "a.stl", base / "to" / "a.stl", true, error));
    CHECK_FALSE(MyFilesPaths::move_by_copy(base / "from" / "a.stl", base / "to" / "a.stl", error));

    CHECK(get(base / "to" / "a.stl") == "precious");
    CHECK(entries_in(base / "to") == 1);
}

TEST_CASE("A My Files move never replaces a folder or the file itself", "[MyFilesPaths]")
{
    ScopedTemporaryDir temp("my-files");
    const fs::path     base = fs::path(temp.string());
    put(base / "a.stl", "model");
    fs::create_directories(base / "to" / "a.stl");

    std::string error;
    CHECK_FALSE(MyFilesPaths::move_file(base / "a.stl", base / "to" / "a.stl", true, error));
    CHECK_FALSE(MyFilesPaths::move_file(base / "a.stl", base / "a.stl", true, error));

    CHECK(get(base / "a.stl") == "model");
    CHECK(fs::is_directory(base / "to" / "a.stl"));
}

TEST_CASE("A My Files move across volumes keeps the content and date and leaves no temporary file", "[MyFilesPaths]")
{
    ScopedTemporaryDir temp("my-files");
    const fs::path     from = fs::path(temp.string()) / "from" / "a.stl";
    const fs::path     to   = fs::path(temp.string()) / "to" / "a.stl";
    put(from, "new");
    put(to, "old");
    fs::last_write_time(from, 1700000000);

    std::string error;
    REQUIRE(MyFilesPaths::move_by_copy(from, to, error));

    CHECK_FALSE(fs::exists(from));
    CHECK(get(to) == "new");
    CHECK(fs::last_write_time(to) == 1700000000);
    CHECK(entries_in(to.parent_path()) == 1);
}

#ifdef _WIN32
TEST_CASE("My Files keeps both files when the one to replace is in use", "[MyFilesPaths][Regression]")
{
    ScopedTemporaryDir temp("my-files");
    const fs::path     base = fs::path(temp.string());
    put(base / "from" / "a.stl", "new");
    put(base / "to" / "a.stl", "precious");

    std::string error;
    {
        fs::ifstream in_use(base / "to" / "a.stl", std::ios::binary);
        REQUIRE(in_use.good());
        CHECK_FALSE(MyFilesPaths::move_file(base / "from" / "a.stl", base / "to" / "a.stl", true, error));
    }

    CHECK(get(base / "from" / "a.stl") == "new");
    CHECK(get(base / "to" / "a.stl") == "precious");
}
#endif
