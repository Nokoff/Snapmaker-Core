#include "MyFilesLibrary.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "WebViewDialog.hpp"
#include "format.hpp"
#include "slic3r/Utils/MyFilesPaths.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include <wx/base64.h>
#include <wx/dirdlg.h>
#include <wx/datetime.h>
#include <wx/image.h>
#include <wx/log.h>
#include <wx/mstream.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

namespace {

constexpr const char *k_config_key    = "my_files_path";
constexpr const char *k_recursive_key = "my_files_recursive";
constexpr int         k_max_files     = 500;
constexpr int         k_max_folders   = 500;
// Depth limit for the "include sub-folders" mode, so a deep or looping tree cannot run away.
constexpr int         k_max_depth     = 8;
// Upper bound on the shallow scan behind each folder tile's "N files" badge, so a directory
// holding tens of thousands of entries cannot stall the listing.
constexpr int         k_max_count_scan = 2000;
// Shared by all folder tiles of one listing, which is built on the UI thread
constexpr int         k_max_preview_scan = 20000;
constexpr unsigned    k_thumb_size   = 192;
constexpr int         k_max_thumbs   = 300;
// Bumped whenever the renderer changes so previously cached (and possibly broken) tiles are redrawn.
constexpr int         k_thumb_cache_version = 2;

constexpr float k_inv_sqrt2 = 0.70710678f;
constexpr float k_inv_sqrt3 = 0.57735027f;
constexpr float k_inv_sqrt6 = 0.40824829f;

// Thumbnail generation runs on background threads: loading and rasterising a model takes
// long enough that doing it on the UI thread froze the whole application (including the 3D
// viewport) until the whole library had been processed.
std::mutex               g_thumb_mutex;
std::condition_variable  g_thumb_cv;
std::deque<std::string>  g_pending_thumbs;   // guarded by g_thumb_mutex
unsigned                 g_thumb_generation = 0; // bumped when the list is rebuilt
int                      g_thumb_total = 0;   // queue size of the current generation, for progress
int                      g_thumb_done  = 0;
std::vector<std::thread> g_thumb_workers;
bool                     g_thumb_workers_started = false; // guarded by g_thumb_mutex
std::atomic<bool>        g_thumb_abort{false};

// Kept deliberately small: each worker can hold a fully expanded mesh, so a big library of
// large STLs would otherwise multiply peak memory by the thread count.
size_t thumbnail_worker_count()
{
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0)
        hw = 2;
    return (size_t) std::min<unsigned>(3u, std::max<unsigned>(1u, hw / 2));
}

const std::unordered_set<std::string> &supported_extensions()
{
    static const std::unordered_set<std::string> exts = {
        ".3mf", ".stl", ".oltp", ".stp", ".step", ".svg", ".amf", ".obj",
#ifdef __APPLE__
        ".usd", ".usda", ".usdc", ".usdz", ".abc", ".ply",
#endif
    };
    return exts;
}

std::string lower_extension(const std::string &path)
{
    std::string ext = fs::path(path).extension().string();
    boost::algorithm::to_lower(ext);
    return ext;
}

bool can_generate_mesh_thumbnail(const std::string &path)
{
    const std::string ext = lower_extension(path);
    // Mesh formats we can load quickly enough for library icons.
    // STEP/SVG are skipped (too heavy / not triangle meshes).
    return ext == ".stl" || ext == ".oltp" || ext == ".obj" || ext == ".glb" || ext == ".gltf" || ext == ".3mf" || ext == ".amf"
#ifdef __APPLE__
           || ext == ".ply"
#endif
        ;
}

std::string to_data_url_png(const void *data, size_t size)
{
    if (!data || size == 0)
        return {};
    std::stringstream ss;
    ss << "data:image/png;base64,";
    ss << wxBase64Encode(data, size);
    return ss.str();
}

fs::path thumbnail_cache_dir()
{
    return fs::path(data_dir()) / "cache" / "my_files_thumbs";
}

std::string cache_file_name(const std::string &path, std::time_t mtime)
{
    const size_t h = std::hash<std::string>{}(path) ^ (static_cast<size_t>(mtime) * 1315423911ull);
    std::ostringstream ss;
    ss << std::hex << h << "_" << static_cast<long long>(mtime) << "_v" << k_thumb_cache_version << ".png";
    return ss.str();
}

fs::path cache_path_for(const std::string &path, std::time_t mtime)
{
    return thumbnail_cache_dir() / cache_file_name(path, mtime);
}

bool save_png_file(const fs::path &png_path, const void *data, size_t size)
{
    if (!data || size == 0)
        return false;
    boost::system::error_code ec;
    fs::create_directories(png_path.parent_path(), ec);
    boost::nowide::ofstream ofs(png_path.string(), std::ios::binary);
    if (!ofs)
        return false;
    ofs.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(ofs);
}

// Embedded 3MF covers are full-size plate renders (often 300-400 kB each). Decode them,
// scale down to tile size and re-encode, so 300 of them stay cheap to serve and display.
bool save_scaled_png_file(const fs::path &png_path, const std::string &image_bytes)
{
    if (image_bytes.empty())
        return false;

    // Runs on worker threads: use the per-image quiet flag rather than wxLogNull, which
    // toggles a global logging counter and would corrupt it when threads overlap.
    wxImage img;
    img.SetLoadFlags(img.GetLoadFlags() & ~wxImage::Load_Verbose);
    {
        wxMemoryInputStream mis(image_bytes.data(), image_bytes.size());
        if (!img.LoadFile(mis, wxBITMAP_TYPE_ANY))
            return false;
    }
    if (!img.IsOk() || img.GetWidth() <= 0 || img.GetHeight() <= 0)
        return false;

    const int longest = std::max(img.GetWidth(), img.GetHeight());
    if (longest > (int) k_thumb_size) {
        const double f = double(k_thumb_size) / double(longest);
        img            = img.Scale(std::max(1, (int) std::lround(img.GetWidth() * f)),
                                   std::max(1, (int) std::lround(img.GetHeight() * f)), wxIMAGE_QUALITY_HIGH);
        if (!img.IsOk())
            return false;
    }

    boost::system::error_code ec;
    fs::create_directories(png_path.parent_path(), ec);
    return img.SaveFile(wxString::FromUTF8(png_path.string()), wxBITMAP_TYPE_PNG);
}

// Returns true when a cover was extracted from the package and cached.
bool cache_embedded_3mf_thumbnail(const std::string &path, const fs::path &cache)
{
    std::string png;
    try {
        png = bbs_3mf_get_thumbnail(path.c_str());
    } catch (...) {
        return false;
    }
    if (png.empty())
        return false;
    return save_scaled_png_file(cache, png);
}

// The home page is a file:// page, which WKWebView keeps out of the data folder, so tiles travel as data URLs. One
// message per tile also keeps each script small enough for the web view.
std::string cached_tile_url(const fs::path &cache)
{
    boost::nowide::ifstream ifs(cache.string(), std::ios::binary);
    if (!ifs)
        return {};
    const std::string png((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return to_data_url_png(png.data(), png.size());
}

// CPU isometric preview — reliable without a live OpenGL context on the home page.
std::string render_model_thumbnail_png_bytes(const Model &model)
{
    struct Part {
        const indexed_triangle_set *its{nullptr};
        Transform3f                 trafo{Transform3f::Identity()};
    };
    std::vector<Part> parts;
    BoundingBoxf3     bbox;

    auto collect = [&](bool model_parts_only) {
        parts.clear();
        bbox = BoundingBoxf3();
        for (const ModelObject *object : model.objects) {
            if (!object)
                continue;
            for (const ModelVolume *volume : object->volumes) {
                if (!volume || (model_parts_only && !volume->is_model_part()))
                    continue;
                const TriangleMesh &mesh = volume->mesh();
                if (mesh.its.indices.empty())
                    continue;
                for (const ModelInstance *instance : object->instances) {
                    if (!instance)
                        continue;
                    const Transform3d trafo = instance->get_matrix() * volume->get_matrix();
                    bbox.merge(mesh.transformed_bounding_box(trafo));
                    parts.push_back({&mesh.its, trafo.cast<float>()});
                }
            }
        }
    };

    collect(true);
    if (parts.empty())
        collect(false); // Some packages only carry modifier / negative volumes.
    if (parts.empty() || !bbox.defined)
        return {};

    const unsigned w = k_thumb_size;
    const unsigned h = k_thumb_size;
    ThumbnailData  data;
    data.set(w, h);
    // Transparent background so the tile styling shows through in both themes.
    std::fill(data.pixels.begin(), data.pixels.end(), (unsigned char) 0);
    std::vector<float> zbuf(size_t(w) * size_t(h), -std::numeric_limits<float>::infinity());

    // Isometric camera sitting at (+1,+1,+1) looking at the model, world Z pointing up on screen.
    // right = (-1,1,0)/sqrt(2), up = (-1,-1,2)/sqrt(6), depth = (1,1,1)/sqrt(3) towards the camera.
    const Vec3f center = bbox.center().cast<float>();

    auto view = [&center](const Vec3f &p) -> Vec3f {
        const Vec3f q = p - center;
        return Vec3f((q.y() - q.x()) * k_inv_sqrt2,
                     (2.f * q.z() - q.x() - q.y()) * k_inv_sqrt6,
                     (q.x() + q.y() + q.z()) * k_inv_sqrt3);
    };

    // Fit the projected bounding box exactly instead of guessing from its longest edge,
    // which used to crop tall or diagonally elongated models.
    float min_r = std::numeric_limits<float>::max();
    float max_r = std::numeric_limits<float>::lowest();
    float min_u = min_r;
    float max_u = max_r;
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3f p(float((corner & 1) ? bbox.max.x() : bbox.min.x()),
                      float((corner & 2) ? bbox.max.y() : bbox.min.y()),
                      float((corner & 4) ? bbox.max.z() : bbox.min.z()));
        const Vec3f v = view(p);
        min_r = std::min(min_r, v.x());
        max_r = std::max(max_r, v.x());
        min_u = std::min(min_u, v.y());
        max_u = std::max(max_u, v.y());
    }

    const float margin = 0.08f * float(w);
    const float scale  = std::min((float(w) - 2.f * margin) / std::max(max_r - min_r, 1e-3f),
                                  (float(h) - 2.f * margin) / std::max(max_u - min_u, 1e-3f));
    const float mid_r  = 0.5f * (min_r + max_r);
    const float mid_u  = 0.5f * (min_u + max_u);

    // NOTE: rows are written bottom-up because compress_thumbnail_png() flips the buffer
    // vertically (it expects an OpenGL framebuffer).
    auto project = [&](const Vec3f &p) -> Vec3f {
        const Vec3f v = view(p);
        return Vec3f((v.x() - mid_r) * scale + float(w) * 0.5f,
                     (v.y() - mid_u) * scale + float(h) * 0.5f,
                     v.z());
    };

    const Vec3f light = Vec3f(-0.35f, 0.25f, 0.9f).normalized();

    auto put_pixel = [&](int x, int y, float z, float shade) {
        if (x < 0 || y < 0 || x >= (int) w || y >= (int) h)
            return;
        const size_t idx = size_t(y) * w + size_t(x);
        if (z <= zbuf[idx])
            return;
        zbuf[idx] = z;
        const float s = std::clamp(shade, 0.2f, 1.f);
        // Soft teal-gray shaded mesh.
        data.pixels[idx * 4 + 0] = (unsigned char) std::clamp(int(70 + 140 * s), 0, 255);
        data.pixels[idx * 4 + 1] = (unsigned char) std::clamp(int(90 + 145 * s), 0, 255);
        data.pixels[idx * 4 + 2] = (unsigned char) std::clamp(int(95 + 140 * s), 0, 255);
        data.pixels[idx * 4 + 3] = 255;
    };

    size_t faces_done = 0;
    for (const Part &part : parts) {
        for (const Vec3i32 &face : part.its->indices) {
            // Keep app shutdown from waiting on a multi-million triangle render.
            if ((++faces_done & 0xFFFF) == 0 && g_thumb_abort.load())
                return {};

            const Vec3f v0 = part.trafo * part.its->vertices[face(0)];
            const Vec3f v1 = part.trafo * part.its->vertices[face(1)];
            const Vec3f v2 = part.trafo * part.its->vertices[face(2)];

            Vec3f       n    = (v1 - v0).cross(v2 - v0);
            const float nlen = n.norm();
            if (nlen < 1e-12f)
                continue;
            n /= nlen;

            const Vec3f p0 = project(v0);
            const Vec3f p1 = project(v1);
            const Vec3f p2 = project(v2);

            const float area = (p1.x() - p0.x()) * (p2.y() - p0.y()) - (p2.x() - p0.x()) * (p1.y() - p0.y());
            if (std::fabs(area) < 1e-6f)
                continue;

            // No back-face culling: the depth buffer already resolves visibility, and open or
            // inconsistently wound meshes (AI/scan exports) would otherwise render empty.
            // Two-sided lighting for the same reason.
            const float shade = 0.35f + 0.65f * std::fabs(n.dot(light));

            const int minx = std::max(0, (int) std::floor(std::min({p0.x(), p1.x(), p2.x()})));
            const int maxx = std::min((int) w - 1, (int) std::ceil(std::max({p0.x(), p1.x(), p2.x()})));
            const int miny = std::max(0, (int) std::floor(std::min({p0.y(), p1.y(), p2.y()})));
            const int maxy = std::min((int) h - 1, (int) std::ceil(std::max({p0.y(), p1.y(), p2.y()})));

            for (int y = miny; y <= maxy; ++y) {
                for (int x = minx; x <= maxx; ++x) {
                    const float px = float(x) + 0.5f;
                    const float py = float(y) + 0.5f;
                    const float w0 = ((p1.x() - px) * (p2.y() - py) - (p2.x() - px) * (p1.y() - py)) / area;
                    const float w1 = ((p2.x() - px) * (p0.y() - py) - (p0.x() - px) * (p2.y() - py)) / area;
                    const float w2 = 1.f - w0 - w1;
                    if (w0 < 0.f || w1 < 0.f || w2 < 0.f)
                        continue;
                    const float z = w0 * p0.z() + w1 * p1.z() + w2 * p2.z();
                    put_pixel(x, y, z, shade);
                }
            }
        }
    }

    auto compressed = GCodeThumbnails::compress_thumbnail(data, GCodeThumbnailsFormat::PNG);
    if (!compressed || !compressed->data || compressed->size == 0)
        return {};
    return std::string(reinterpret_cast<const char *>(compressed->data), compressed->size);
}

bool load_model_for_thumbnail(const std::string &path, Model &model)
{
    try {
        model = Model::read_from_file(path, nullptr, nullptr,
                                      LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances);
        if (!model.objects.empty())
            return true;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(debug) << "My Files: read_from_file failed for " << path << ": " << e.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(debug) << "My Files: read_from_file failed for " << path;
    }

    // read_from_file() only ever tries load_bbs_3mf(); PrusaSlicer-flavoured packages need the
    // archive path, which sniffs the producer first. Without this they never get a preview.
    if (lower_extension(path) != ".3mf")
        return false;

    try {
        model = Model();
        DynamicPrintConfig        config;
        ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
        En3mfType                 type = En3mfType::From_Other;
        model = Model::read_from_archive(path, &config, &substitutions, type,
                                         LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances);
        return !model.objects.empty();
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "My Files: failed to load 3mf for thumbnail " << path << ": " << e.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "My Files: failed to load 3mf for thumbnail " << path;
    }
    return false;
}

// Produces the cached tile for one file: embedded 3MF cover when there is one, otherwise a
// rendered mesh preview. Returns the URL to serve, or an empty string when nothing could be made.
std::string generate_thumbnail_url(const std::string &path, std::time_t mtime)
{
    if (mtime <= 0)
        return {};

    const fs::path cache = cache_path_for(path, mtime);

    if (lower_extension(path) == ".3mf" && cache_embedded_3mf_thumbnail(path, cache))
        return cached_tile_url(cache);

    Model model;
    if (!load_model_for_thumbnail(path, model)) {
        BOOST_LOG_TRIVIAL(warning) << "My Files: no model to render a thumbnail from for " << path;
        return {};
    }

    const std::string png_bytes = render_model_thumbnail_png_bytes(model);
    if (png_bytes.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "My Files: CPU thumbnail render produced no image for " << path;
        return {};
    }

    if (!save_png_file(cache, png_bytes.data(), png_bytes.size())) {
        BOOST_LOG_TRIVIAL(warning) << "My Files: failed to write thumbnail cache " << cache.string();
    }
    return to_data_url_png(png_bytes.data(), png_bytes.size());
}

// Queues the preview of a file: always when it only has to be read from the cache, up to k_max_thumbs when it has to be made.
void queue_thumbnail(const std::string &path, std::time_t mtime, std::vector<std::string> &queued, int &to_make)
{
    if (mtime <= 0 || std::find(queued.begin(), queued.end(), path) != queued.end())
        return;
    boost::system::error_code ec;
    if (fs::is_regular_file(cache_path_for(path, mtime), ec) && !ec) {
        queued.push_back(path);
    } else if (to_make < k_max_thumbs && can_generate_mesh_thumbnail(path)) {
        queued.push_back(path);
        ++to_make;
    }
}

struct FileEntry {
    fs::path    path;
    std::time_t mtime{0};
    // Folder the file lives in, relative to the one being browsed; empty for direct children.
    std::string rel_dir;
};

struct FolderEntry {
    fs::path    path;
    fs::path    preview_path;
    std::time_t mtime{0};
    std::time_t preview_mtime{0};
    int         file_count{0};
    int         folder_count{0};
};

std::time_t last_write_time_or_zero(const fs::path &p)
{
    try {
        boost::system::error_code ec;
        const std::time_t        t = fs::last_write_time(p, ec);
        return ec ? 0 : t;
    } catch (...) {
        return 0;
    }
}

// Dot-prefixed entries are version-control / metadata noise rather than model libraries.
bool is_hidden_name(const fs::path &p)
{
    const std::string name = p.filename().string();
    return !name.empty() && name.front() == '.';
}

// One level deep only: enough to label a folder tile without walking the whole tree.
void count_folder_contents(const fs::path &dir, int &files, int &folders)
{
    files   = 0;
    folders = 0;

    boost::system::error_code ec;
    fs::directory_iterator    it(dir, ec), end;
    if (ec)
        return;

    for (int scanned = 0; it != end && scanned < k_max_count_scan; it.increment(ec), ++scanned) {
        if (ec) {
            ec.clear();
            continue;
        }
        const fs::path &p = it->path();
        if (is_hidden_name(p))
            continue;
        boost::system::error_code status_ec;
        if (fs::is_directory(p, status_ec)) {
            if (!status_ec)
                ++folders;
        } else if (MyFilesLibrary::is_supported_model_file(p.string())) {
            ++files;
        }
    }
}

// Find a recent 3MF/STL below a folder for its tile preview. Keep the walk bounded and do not
// follow symlinked directories, matching the safeguards used by the recursive file listing.
void find_folder_preview(const fs::path &dir, int depth, int &scanned, fs::path &preview,
                         std::time_t &preview_mtime)
{
    if (depth > k_max_depth || scanned >= k_max_count_scan)
        return;
    ++scanned; // an empty folder costs a read too

    boost::system::error_code ec;
    fs::directory_iterator it(dir, ec), end;
    if (ec)
        return;

    std::vector<fs::path> subdirs;
    for (; it != end && scanned < k_max_count_scan; it.increment(ec), ++scanned) {
        if (ec) {
            ec.clear();
            continue;
        }
        const fs::path &p = it->path();
        if (is_hidden_name(p))
            continue;

        boost::system::error_code status_ec;
        if (fs::is_directory(p, status_ec)) {
            if (!status_ec && !fs::is_symlink(p, status_ec) && !status_ec)
                subdirs.push_back(p);
            continue;
        }

        const std::string ext = lower_extension(p.string());
        if (ext != ".3mf" && ext != ".stl")
            continue;
        const std::time_t mtime = last_write_time_or_zero(p);
        if (mtime > preview_mtime) {
            preview = p;
            preview_mtime = mtime;
        }
    }

    std::sort(subdirs.begin(), subdirs.end());
    for (const fs::path &subdir : subdirs) {
        if (scanned >= k_max_count_scan)
            break;
        find_folder_preview(subdir, depth + 1, scanned, preview, preview_mtime);
    }
}

// Lists one directory: sub-folders and the model files directly inside it. Navigation replaced
// the old recursive flattening, so nothing here descends on its own.
// Returns false when a cap was hit and entries had to be dropped.
bool list_directory(const fs::path &dir, std::vector<FolderEntry> &folders, std::vector<FileEntry> &files)
{
    bool                      complete = true;
    int                       preview_budget = k_max_preview_scan;
    boost::system::error_code ec;
    fs::directory_iterator    it(dir, ec), end;
    if (ec)
        return complete;

    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const fs::path &p = it->path();
        if (is_hidden_name(p))
            continue;

        boost::system::error_code status_ec;
        if (fs::is_directory(p, status_ec)) {
            if (status_ec)
                continue;
            if ((int) folders.size() >= k_max_folders) {
                complete = false;
                continue;
            }
            FolderEntry entry;
            entry.path  = p;
            entry.mtime = last_write_time_or_zero(p);
            count_folder_contents(p, entry.file_count, entry.folder_count);
            if (preview_budget > 0) {
                const int already = std::max(0, k_max_count_scan - preview_budget);
                int       scanned = already;
                find_folder_preview(p, 0, scanned, entry.preview_path, entry.preview_mtime);
                preview_budget -= scanned - already;
            }
            folders.push_back(std::move(entry));
            continue;
        }
        if (!MyFilesLibrary::is_supported_model_file(p.string()))
            continue;
        if ((int) files.size() >= k_max_files) {
            complete = false;
            continue;
        }

        FileEntry entry;
        entry.path  = p;
        entry.mtime = last_write_time_or_zero(p);
        files.push_back(std::move(entry));
    }
    return complete;
}

// "Include sub-folders" mode: everything below `dir`, files only. `rel_dir` is the folder each
// file came from, relative to the folder being browsed, and is what the tile shows as its origin.
// Returns false when the file cap was hit and entries had to be dropped.
bool collect_files_recursive(const fs::path &dir, const std::string &rel_dir, int depth, std::vector<FileEntry> &files)
{
    if ((int) files.size() >= k_max_files)
        return false;
    if (depth > k_max_depth)
        return true; // Deeper than we are willing to walk, but not a cap on what the user asked for.

    boost::system::error_code ec;
    fs::directory_iterator    it(dir, ec), end;
    if (ec)
        return true;

    bool                  complete = true;
    std::vector<fs::path> subdirs;

    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const fs::path &p = it->path();
        if (is_hidden_name(p))
            continue;

        boost::system::error_code status_ec;
        if (fs::is_directory(p, status_ec)) {
            // Symlinked directories are skipped: following them can loop back on itself.
            if (!status_ec && !fs::is_symlink(p, status_ec) && !status_ec)
                subdirs.push_back(p);
            continue;
        }
        if (!MyFilesLibrary::is_supported_model_file(p.string()))
            continue;
        if ((int) files.size() >= k_max_files) {
            complete = false;
            continue;
        }

        FileEntry entry;
        entry.path    = p;
        entry.mtime   = last_write_time_or_zero(p);
        entry.rel_dir = rel_dir;
        files.push_back(std::move(entry));
    }

    // Breadth first, so a shallow file never loses its slot to a deeply nested one.
    std::sort(subdirs.begin(), subdirs.end());
    for (const fs::path &sub : subdirs) {
        const std::string name = sub.filename().string();
        const std::string rel  = rel_dir.empty() ? name : rel_dir + "/" + name;
        if (!collect_files_recursive(sub, rel, depth + 1, files))
            complete = false;
    }
    return complete;
}

// Set by collect_files() so the page can say it is not showing everything.
bool g_listing_truncated = false;

// Folder the user is currently browsing, relative to the mapped root; UI thread only.
std::string g_current_subdir;

// Must be called on the UI thread.
void dispatch_to_page(const nlohmann::json &req)
{
    WebViewPanel *webview = WebViewPanel::if_built();
    if (!webview)
        return;
    const wxString payload = wxString::FromUTF8(req.dump(-1, ' ', false, nlohmann::json::error_handler_t::ignore));
    webview->RunScript("window.postMessage(" + payload + ")");
}

// Called from the worker thread; hops onto the UI thread to touch the web view.
void post_thumbnail_update(const std::string &path, const std::string &image_url, int done, int total)
{
    nlohmann::json req;
    req["sequence_id"] = "";
    req["command"]     = "my_files_thumbnail";
    req["path"]        = path;
    req["image"]       = image_url;
    req["done"]        = done;
    req["total"]       = total;
    wxGetApp().CallAfter([req] { dispatch_to_page(req); });
}

// done == total tells the page to hide the progress bar.
void post_thumbnail_progress(int done, int total)
{
    nlohmann::json req;
    req["sequence_id"] = "";
    req["command"]     = "my_files_thumbnail_progress";
    req["done"]        = done;
    req["total"]       = total;
    wxGetApp().CallAfter([req] { dispatch_to_page(req); });
}

// Long-lived workers that sleep while the queue is empty and are joined at app exit.
// Everything they touch is either thread-local or guarded by g_thumb_mutex; the wxImage
// handlers they use are registered once at startup (wxInitAllImageHandlers) and are read-only
// from here on.
void thumbnail_worker_main()
{
    for (;;) {
        std::string path;
        unsigned    generation = 0;

        {
            std::unique_lock<std::mutex> lock(g_thumb_mutex);
            g_thumb_cv.wait(lock, [] { return g_thumb_abort.load() || !g_pending_thumbs.empty(); });
            if (g_thumb_abort.load())
                return;
            path       = g_pending_thumbs.front();
            generation = g_thumb_generation;
            g_pending_thumbs.pop_front();
        }

        std::string image_url;
        try {
            boost::system::error_code ec;
            std::time_t               mtime = fs::last_write_time(path, ec);
            if (ec)
                mtime = 0;
            if (mtime > 0) {
                const fs::path            cache = cache_path_for(path, mtime);
                boost::system::error_code exists_ec;
                if (fs::is_regular_file(cache, exists_ec) && !exists_ec)
                    image_url = cached_tile_url(cache);
                if (image_url.empty())
                    image_url = generate_thumbnail_url(path, mtime);
            }
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(warning) << "My Files: thumbnail generation failed for " << path << ": " << e.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "My Files: thumbnail generation failed for " << path;
        }

        if (g_thumb_abort.load())
            return;

        int done  = 0;
        int total = 0;
        {
            std::lock_guard<std::mutex> lock(g_thumb_mutex);
            if (generation != g_thumb_generation)
                continue; // The list was rebuilt underneath us; this result is stale.
            done  = ++g_thumb_done;
            total = g_thumb_total;
        }

        if (image_url.empty())
            post_thumbnail_progress(done, total);
        else
            post_thumbnail_update(path, image_url, done, total);
    }
}

} // namespace

std::string MyFilesLibrary::get_folder_path()
{
    if (!wxGetApp().app_config)
        return {};
    return wxGetApp().app_config->get(k_config_key);
}

void MyFilesLibrary::set_folder_path(const std::string &path)
{
    if (!wxGetApp().app_config)
        return;
    // A new root invalidates wherever the user had navigated to.
    g_current_subdir.clear();
    wxGetApp().app_config->set(k_config_key, path);
    wxGetApp().app_config->save();
}

std::string MyFilesLibrary::get_current_subdir()
{
    if (g_current_subdir.empty())
        return {};
    // Re-validated on every read: the folder may have been renamed or deleted behind our back.
    g_current_subdir = MyFilesPaths::sanitize_subdir(fs::path(get_folder_path()), g_current_subdir);
    return g_current_subdir;
}

void MyFilesLibrary::set_current_subdir(const std::string &rel)
{
    g_current_subdir = MyFilesPaths::sanitize_subdir(fs::path(get_folder_path()), rel);
}

bool MyFilesLibrary::get_recursive()
{
    if (!wxGetApp().app_config)
        return false;
    return wxGetApp().app_config->get_bool(k_recursive_key);
}

void MyFilesLibrary::set_recursive(bool on)
{
    if (!wxGetApp().app_config)
        return;
    wxGetApp().app_config->set(k_recursive_key, on ? "1" : "0");
    wxGetApp().app_config->save();
}

std::string MyFilesLibrary::get_current_dir()
{
    const std::string root = get_folder_path();
    if (root.empty())
        return {};
    const std::string sub = get_current_subdir();
    return sub.empty() ? root : (fs::path(root) / fs::path(sub)).string();
}

bool MyFilesLibrary::select_folder(wxWindow *parent)
{
    std::string current = get_folder_path();
    wxString    start   = current.empty() ? wxString() : wxString::FromUTF8(current);

    wxDirDialog dialog(parent, _L("Choose My Files Folder"), start,
                       wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST | wxDD_NEW_DIR_BUTTON);
    if (dialog.ShowModal() != wxID_OK)
        return false;

    std::string path = dialog.GetPath().ToUTF8().data();
    set_folder_path(path);
    return true;
}

namespace {

std::vector<std::string> filter_library_paths(const std::vector<std::string> &paths)
{
    std::vector<std::string> out;
    out.reserve(paths.size());
    for (const auto &path : paths) {
        if (MyFilesLibrary::is_file_in_library(path))
            out.push_back(path);
    }
    return out;
}

// The folder tile's "3 files · 1 folder" line. Built here, where plural forms can be translated.
std::string folder_summary(int files, int folders)
{
    std::string summary;
    if (files > 0)
        summary = into_u8(format_wxstr(_L_PLURAL("%1% file", "%1% files", files), files));
    if (folders > 0) {
        if (!summary.empty())
            summary += " \xC2\xB7 ";
        summary += into_u8(format_wxstr(_L_PLURAL("%1% folder", "%1% folders", folders), folders));
    }
    return summary.empty() ? _u8L("Empty") : summary;
}

// Runs on the UI thread.
void send_list()
{
    nlohmann::json files;
    MyFilesLibrary::collect_files(files);

    nlohmann::json req;
    req["sequence_id"]    = "";
    req["command"]        = "get_my_files";
    req["folder"]         = MyFilesLibrary::get_folder_path();
    // Where inside the mapped folder the listing came from, for the breadcrumb.
    req["subfolder"]      = MyFilesLibrary::get_current_subdir();
    req["current_folder"] = MyFilesLibrary::get_current_dir();
    req["recursive"]      = MyFilesLibrary::get_recursive();
    req["truncated"]      = MyFilesLibrary::last_listing_truncated();
    req["response"]       = std::move(files);
    dispatch_to_page(req);
    MyFilesLibrary::pump_thumbnail_generation();
}

void open_file(const std::string &path)
{
    if (lower_extension(path) == ".3mf") {
        wxGetApp().request_open_project(path);
        return;
    }
    // Only a 3MF is a project. Opened as one, part.stl would become the project part.3mf beside it, which the
    // next Save overwrites without asking. Models are imported like a dropped file instead.
    Plater *plater = wxGetApp().plater();
    if (!plater || plater->is_background_process_slicing()) {
        show_info(nullptr, _L("new or open project file is not allowed during the slicing process!"), _L("Open Project"));
        return;
    }
    if (wxGetApp().mainframe)
        wxGetApp().mainframe->select_tab(TAB_ID_PREPARE);
    Plater::TakeSnapshot snapshot(plater, "Import Object");
    plater->load_files(std::vector<fs::path>{fs::path(path)}, LoadStrategy::LoadModel);
}

} // namespace

bool MyFilesLibrary::delete_files(wxWindow *parent, const std::vector<std::string> &paths)
{
    const auto targets = filter_library_paths(paths);
    if (targets.empty())
        return false;

    const int count = int(targets.size());
    wxString  message;
    if (count == 1) {
        message = format_wxstr(_L("Permanently delete \"%1%\"?\n\nThis cannot be undone."),
                               from_u8(fs::path(targets.front()).filename().string()));
    } else {
        message = format_wxstr(_L_PLURAL("Permanently delete %1% selected file?\n\nThis cannot be undone.",
                                         "Permanently delete %1% selected files?\n\nThis cannot be undone.", count),
                               count);
    }

    MessageDialog dlg(parent, message, _L("Delete Files"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
    if (dlg.ShowModal() != wxID_YES)
        return false;

    int failed = 0;
    for (const auto &path : targets) {
        boost::system::error_code ec;
        if (!fs::remove(fs::path(path), ec) || ec) {
            ++failed;
            BOOST_LOG_TRIVIAL(warning) << "My Files: failed to delete " << path << ": " << ec.message();
        }
    }

    if (failed > 0) {
        MessageDialog(parent,
                      format_wxstr(_L_PLURAL("Could not delete %1% file. It may be open or in use.",
                                             "Could not delete %1% files. They may be open or in use.", failed),
                                   failed),
                      _L("Delete Files"), wxOK | wxICON_WARNING)
            .ShowModal();
    }
    return failed < (int) targets.size();
}

bool MyFilesLibrary::move_files(wxWindow *parent, const std::vector<std::string> &paths)
{
    const auto targets = filter_library_paths(paths);
    if (targets.empty())
        return false;

    // Start in the folder being browsed: moving into a sibling sub-folder is the common case.
    std::string current = get_current_dir();
    wxString    start   = current.empty() ? wxString() : wxString::FromUTF8(current);
    wxDirDialog dialog(parent, _L("Move Files To Folder"), start,
                       wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST | wxDD_NEW_DIR_BUTTON);
    if (dialog.ShowModal() != wxID_OK)
        return false;

    const fs::path dest_dir(dialog.GetPath().ToUTF8().data());
    boost::system::error_code ec;
    if (!fs::is_directory(dest_dir, ec) || ec)
        return false;

    int moved  = 0;
    int failed = 0;
    for (const auto &path : targets) {
        const fs::path from(path);
        const fs::path to = dest_dir / from.filename();
        // Already in that folder
        ec.clear();
        if (fs::equivalent(from, to, ec) && !ec)
            continue;

        boost::system::error_code exists_ec;
        const bool                exists = fs::exists(to, exists_ec);
        if (exists) {
            MessageDialog overwrite_dlg(
                parent,
                format_wxstr(_L("\"%1%\" already exists in the destination folder. Overwrite it?"),
                             from_u8(to.filename().string())),
                _L("Move Files"), wxYES_NO | wxCANCEL | wxNO_DEFAULT | wxICON_WARNING);
            const int answer = overwrite_dlg.ShowModal();
            if (answer == wxID_CANCEL)
                break;
            if (answer != wxID_YES)
                continue;
        }

        std::string error;
        if (MyFilesPaths::move_file(from, to, exists, error)) {
            ++moved;
        } else {
            ++failed;
            BOOST_LOG_TRIVIAL(warning) << "My Files: failed to move " << path << " -> " << to.string() << ": " << error;
        }
    }

    if (failed > 0) {
        MessageDialog(parent,
                      format_wxstr(_L_PLURAL("Could not move %1% file. It may be open or in use.",
                                             "Could not move %1% files. They may be open or in use.", failed),
                                   failed),
                      _L("Move Files"), wxOK | wxICON_WARNING)
            .ShowModal();
    }
    return moved > 0;
}

bool MyFilesLibrary::is_supported_model_file(const std::string &path)
{
    std::string ext = fs::path(path).extension().string();
    boost::algorithm::to_lower(ext);
    return supported_extensions().count(ext) > 0;
}

bool MyFilesLibrary::is_file_in_library(const std::string &path)
{
    const std::string folder = get_folder_path();
    if (folder.empty() || !is_supported_model_file(path))
        return false;

    boost::system::error_code ec;
    if (!fs::is_regular_file(fs::path(path), ec) || ec)
        return false;
    return MyFilesPaths::is_inside(fs::path(folder), fs::path(path), false);
}

bool MyFilesLibrary::is_folder_in_library(const std::string &path)
{
    const std::string folder = get_folder_path();
    if (folder.empty() || path.empty())
        return false;

    boost::system::error_code ec;
    if (!fs::is_directory(fs::path(path), ec) || ec)
        return false;
    // The mapped root itself counts: "open containing folder" on it is still meaningful.
    return MyFilesPaths::is_inside(fs::path(folder), fs::path(path), true);
}

bool MyFilesLibrary::last_listing_truncated() { return g_listing_truncated; }

void MyFilesLibrary::collect_files(nlohmann::json &out)
{
    out                 = nlohmann::json::array();
    g_listing_truncated = false;
    {
        // Drop whatever the previous list queued; results still in flight are discarded by
        // the generation check in the worker.
        std::lock_guard<std::mutex> lock(g_thumb_mutex);
        g_pending_thumbs.clear();
        ++g_thumb_generation;
        g_thumb_total = 0;
        g_thumb_done  = 0;
    }

    const std::string folder = get_folder_path();
    if (folder.empty())
        return;

    // get_current_dir() already drops a sub-folder that no longer exists, so this only fails when
    // the mapped root itself is gone.
    fs::path                  dir(get_current_dir());
    boost::system::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec) || ec) {
        BOOST_LOG_TRIVIAL(warning) << "My Files folder missing or inaccessible: " << folder;
        return;
    }

    std::vector<FolderEntry> folders;
    std::vector<FileEntry>   files;
    files.reserve(64);
    // Sub-folder tiles are listed either way, so drilling down still works while the recursive
    // view is on; only the set of files changes.
    bool complete = list_directory(dir, folders, files);
    if (get_recursive()) {
        files.clear();
        complete = collect_files_recursive(dir, std::string(), 0, files) && complete;
    }
    g_listing_truncated = !complete;

    std::sort(folders.begin(), folders.end(), [](const FolderEntry &a, const FolderEntry &b) {
        return a.path.filename().string() < b.path.filename().string();
    });
    std::sort(files.begin(), files.end(), [](const FileEntry &a, const FileEntry &b) {
        if (a.mtime != b.mtime)
            return a.mtime > b.mtime;
        return a.path.filename().string() < b.path.filename().string();
    });

    // dir == root / subdir, so a child's path relative to the root is just one more segment.
    const std::string subdir = get_current_subdir();
    std::vector<std::string> queued;
    int                      to_make = 0;

    for (const FolderEntry &entry : folders) {
        const std::string name = entry.path.filename().string();
        nlohmann::json    item;
        item["is_dir"]       = true;
        item["project_name"] = name;
        item["path"]         = entry.path.string();
        // Relative to the mapped root: this is what the page sends back to navigate into it.
        item["rel"]          = subdir.empty() ? name : subdir + "/" + name;
        item["summary"]      = folder_summary(entry.file_count, entry.folder_count);
        if (!entry.preview_path.empty()) {
            item["preview_path"] = entry.preview_path.string();
            queue_thumbnail(entry.preview_path.string(), entry.preview_mtime, queued, to_make);
        }
        item["time"]         = entry.mtime > 0 ? wxDateTime(entry.mtime).FormatISOCombined(' ').ToStdString()
                                               : std::string();
        out.push_back(std::move(item));
    }

    for (size_t i = 0; i < files.size(); ++i) {
        nlohmann::json item;
        const std::string path_u8 = files[i].path.string();
        item["is_dir"]            = false;
        item["project_name"]      = files[i].path.filename().string();
        item["path"]              = path_u8;
        // Only set in the recursive view; the page shows it so identically named files stay apart.
        if (!files[i].rel_dir.empty())
            item["rel_dir"] = files[i].rel_dir;
        if (files[i].mtime > 0) {
            item["time"] = wxDateTime(files[i].mtime).FormatISOCombined(' ').ToStdString();
            queue_thumbnail(path_u8, files[i].mtime, queued, to_make);
        } else {
            item["time"] = _u8L("File is missing");
        }
        out.push_back(std::move(item));
    }

    std::lock_guard<std::mutex> lock(g_thumb_mutex);
    g_pending_thumbs.assign(queued.begin(), queued.end());
    g_thumb_total = (int) g_pending_thumbs.size();
}

void MyFilesLibrary::pump_thumbnail_generation()
{
    if (g_thumb_abort.load())
        return;

    int total = 0;
    {
        std::lock_guard<std::mutex> lock(g_thumb_mutex);
        if (g_pending_thumbs.empty())
            return;
        total = g_thumb_total;
        if (!g_thumb_workers_started) {
            g_thumb_workers_started = true;
            const size_t n          = thumbnail_worker_count();
            g_thumb_workers.reserve(n);
            for (size_t i = 0; i < n; ++i)
                g_thumb_workers.emplace_back(thumbnail_worker_main);
            BOOST_LOG_TRIVIAL(info) << "My Files: started " << n << " thumbnail workers";
        }
    }
    // Show the bar straight away; the workers take over reporting from here.
    post_thumbnail_progress(0, total);
    g_thumb_cv.notify_all();
}

void MyFilesLibrary::stop_thumbnail_generation()
{
    {
        std::lock_guard<std::mutex> lock(g_thumb_mutex);
        g_thumb_abort.store(true);
        g_pending_thumbs.clear();
    }
    g_thumb_cv.notify_all();
    for (auto &worker : g_thumb_workers) {
        if (worker.joinable())
            worker.join();
    }

    std::lock_guard<std::mutex> lock(g_thumb_mutex);
    g_thumb_workers.clear();
    g_thumb_workers_started = false;
}

bool MyFilesLibrary::handle_request(const std::string &message)
{
    const auto request = nlohmann::json::parse(message, nullptr, false);
    if (!request.is_object())
        return false;
    const auto command_it = request.find("command");
    if (command_it == request.end() || !command_it->is_string())
        return false;
    const std::string command = command_it->get<std::string>();
    if (command != "get_my_files" && command.rfind("myfiles_", 0) != 0)
        return false;

    std::string              path;
    std::vector<std::string> paths;
    bool                     recursive = false;
    if (const auto data = request.find("data"); data != request.end() && data->is_object()) {
        if (const auto it = data->find("path"); it != data->end() && it->is_string())
            path = it->get<std::string>();
        if (const auto it = data->find("recursive"); it != data->end() && it->is_boolean())
            recursive = it->get<bool>();
        if (const auto it = data->find("paths"); it != data->end() && it->is_array()) {
            for (const auto &entry : *it) {
                if (entry.is_string() && !entry.get<std::string>().empty())
                    paths.push_back(entry.get<std::string>());
            }
        }
    }

    // Deferred: several of these open a dialog, which must not run inside the web view's message callback
    wxGetApp().CallAfter([command, path, paths, recursive] {
        MainFrame *mainframe = wxGetApp().mainframe;
        if (!mainframe)
            return;
        try {
            if (command == "get_my_files") {
                send_list();
            } else if (command == "myfiles_open_folder") {
                set_current_subdir(path); // empty goes back to the mapped root
                send_list();
            } else if (command == "myfiles_set_recursive") {
                set_recursive(recursive);
                send_list();
            } else if (command == "myfiles_select_folder" || command == "myfiles_change_folder") {
                if (select_folder(mainframe))
                    send_list();
            } else if (command == "myfiles_open_file") {
                if (is_file_in_library(path))
                    open_file(path);
                else
                    send_list(); // it has been moved or deleted since it was listed
            } else if (command == "myfiles_explore_file") {
                if (is_file_in_library(path) || is_folder_in_library(path))
                    desktop_open_any_folder(fs::path(path).make_preferred().string());
            } else if (command == "myfiles_delete_files") {
                if (delete_files(mainframe, paths))
                    send_list();
            } else if (command == "myfiles_move_files") {
                if (move_files(mainframe, paths))
                    send_list();
            }
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "My Files: " << command << " failed: " << e.what();
        }
    });
    return true;
}

} // namespace GUI
} // namespace Slic3r
