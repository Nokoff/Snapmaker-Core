#include "SnapmakerPrinterAgent.hpp"
#include "Http.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "IPrinterAgent.hpp"
#include "NetworkAgentFactory.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/format.hpp"
#include "bambu_networking.hpp"

#include "nlohmann/json.hpp"
#include <chrono>
#include <boost/log/trivial.hpp>
#include <vector>
#include <string>
#include "libslic3r/Preset.hpp"
#include <cstddef>
#include <utility>
#include <algorithm>
#include <cctype>

namespace Slic3r {

namespace {

constexpr const char* SNAPMAKER_AGENT_VERSION = "0.0.1";

// print_task_config.py in the U1's Klipper: G-code tools are logical and are mapped to toolheads at print time
constexpr int LOGICAL_EXTRUDER_COUNT  = 32;
constexpr int PHYSICAL_EXTRUDER_COUNT = 4;

// The printer answers only once it has scanned the file's metadata and Klipper has started it
constexpr std::chrono::minutes START_PRINT_TIMEOUT{3};

constexpr int JSONRPC_METHOD_NOT_FOUND = -32601;

// Ranks a candidate that declares no usable colour behind every candidate that does, while
// still leaving it ahead of no match at all. The largest real distance is 3 * 255^2 = 195075.
constexpr unsigned int UNRANKED_COLOR_DISTANCE = 0xfffffffeu;

// filament_vendor of the presets Orca ships in OrcaFilamentLibrary, set in that library's own
// filament/base/fdm_filament_common.json, so every "Generic ..." preset inherits it.
constexpr const char* GENERIC_FILAMENT_VENDOR = "Generic";

// Safely access a parallel array by index, returning a fallback if out of bounds.
template<typename T>
T safe_at(const std::vector<T>& vec, int index, const T& fallback)
{
    return (index >= 0 && index < static_cast<int>(vec.size())) ? vec[index] : fallback;
}

// Parse the RRGGBB part of a colour into 0xRRGGBB, skipping a leading '#' and
// dropping any alpha channel. Returns false rather than letting std::stoul throw
// on an empty or malformed value.
bool parse_rgb(const std::string& value, unsigned int& out)
{
    const size_t hash_pos = value.find('#');
    std::string  digits   = value.substr(hash_pos != std::string::npos ? hash_pos + 1 : 0);
    if (digits.size() > 6)
        digits.resize(6);
    if (digits.size() != 6)
        return false;
    for (char c : digits) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    }
    out = static_cast<unsigned int>(std::stoul(digits, nullptr, 16));
    return true;
}

// Case-insensitive whole-word search. The needle must not be flanked by alphanumeric
// characters, so "CF" matches "PETG-CF" and "PLA CF" but not "Scaffold" or "CF10".
bool contains_word_ci(const std::string& haystack, const std::string& needle)
{
    if (needle.empty() || needle.size() > haystack.size())
        return false;

    const auto equal_ci = [](unsigned char a, unsigned char b) { return std::toupper(a) == std::toupper(b); };
    const auto is_word  = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };

    for (auto it = haystack.begin(); it != haystack.end(); ++it) {
        it = std::search(it, haystack.end(), needle.begin(), needle.end(), equal_ci);
        if (it == haystack.end())
            return false;

        const auto after    = it + static_cast<std::string::difference_type>(needle.size());
        const bool left_ok  = it == haystack.begin() || !is_word(*(it - 1));
        const bool right_ok = after == haystack.end() || !is_word(*after);
        if (left_ok && right_ok)
            return true;
    }
    return false;
}

// A tag reports the fibre loading in its sub type ("CF10", "GF30"), while the material types in
// MaterialType::all() and the profile names only ever say CF or GF. Expects upper case input.
std::string normalize_fibre_sub_type(const std::string& sub)
{
    for (const char* fibre : {"CF", "GF"}) {
        const std::string prefix = fibre;
        if (sub.size() >= prefix.size() && sub.compare(0, prefix.size(), prefix) == 0 &&
            std::all_of(sub.begin() + static_cast<std::string::difference_type>(prefix.size()), sub.end(),
                        [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
            return prefix;
    }
    return sub;
}

// Like contains_word_ci, but a trailing digit run belongs to the same word, so a CF sub type
// matches a "PA12-CF10" preset as well as "PA12-CF". A trailing letter does not: "CFX" is
// a different product.
bool contains_sub_type_ci(const std::string& haystack, const std::string& needle)
{
    if (needle.empty() || needle.size() > haystack.size())
        return false;

    const auto equal_ci = [](unsigned char a, unsigned char b) { return std::toupper(a) == std::toupper(b); };
    const auto is_word  = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };

    for (auto it = haystack.begin(); it != haystack.end(); ++it) {
        it = std::search(it, haystack.end(), needle.begin(), needle.end(), equal_ci);
        if (it == haystack.end())
            return false;

        auto after = it + static_cast<std::string::difference_type>(needle.size());
        while (after != haystack.end() && std::isdigit(static_cast<unsigned char>(*after)) != 0)
            ++after;

        const bool left_ok  = it == haystack.begin() || !is_word(*(it - 1));
        const bool right_ok = after == haystack.end() || !is_word(*after);
        if (left_ok && right_ok)
            return true;
    }
    return false;
}

// Tags written by older tools (or as a workaround) carry the full product name in the vendor
// field, e.g. vendor "Anycubic PLA Basic" with an empty sub type, while profiles may use either
// "Anycubic" or "Anycubic PLA Basic". Treat the vendors as matching when they are equal or when
// either contains the other as whole words, ignoring case.
bool vendor_matches(const std::string& tag_vendor, const std::string& profile_vendor)
{
    if (tag_vendor.empty() || profile_vendor.empty())
        return false;
    return contains_word_ci(tag_vendor, profile_vendor) || contains_word_ci(profile_vendor, tag_vendor);
}
// Rank the visible, compatible base presets of one filament type by colour distance, preferring a
// name that carries the spool's sub type. Candidates are limited to vendor_filter, which nothing
// matches when it is empty; tag_vendor is the tag's own vendor field, which a legacy tag uses to
// name the variant.
std::string closest_preset_by_colour(const PresetCollection& filaments, const std::string& vendor_filter,
                                     const std::string& tag_vendor, const std::string& filament_type,
                                     const std::string& sub_wanted, unsigned int target_color_value)
{
    std::string  best_match_id       = "";
    unsigned int best_color_distance = 0xffffffffu;
    bool         best_matches_sub    = false;

    for (const auto& p : filaments.get_presets()) {
        // Only a base preset can be loaded: sync_ams_list() resolves a filament_id to the first
        // base preset carrying it, so a derived copy never loads and could outrank its parent.
        if (p.is_visible && p.is_compatible && filaments.get_preset_base(p) == &p && !p.filament_id.empty() &&
            vendor_matches(vendor_filter, p.config.opt_string("filament_vendor", 0u)) &&
            p.config.opt_string("filament_type", 0u) == filament_type) {
            // A profile that specifies no colour, or one that cannot be parsed, is unranked: it
            // is kept as a last resort rather than scored as black, which would otherwise let a
            // colourless profile win every dark spool and beat profiles that do declare a colour.
            std::string  p_color       = p.config.opt_string("default_filament_colour", 0u);
            unsigned int p_color_value = 0;
            unsigned int distance      = UNRANKED_COLOR_DISTANCE;
            if (parse_rgb(p_color, p_color_value)) {
                // Euclidean colour distance in RGB space
                const int dr = ((target_color_value & 0xff) - (p_color_value & 0xff));
                const int dg = (((target_color_value >> 8) & 0xff) - ((p_color_value >> 8) & 0xff));
                const int db = (((target_color_value >> 16) & 0xff) - ((p_color_value >> 16) & 0xff));
                distance      = static_cast<unsigned int>(dr * dr + dg * dg + db * db);
            }

            // Variants (Matte, Silk, Basic, Eco, ...) are not filament_type values -- the
            // profiles carry them in the preset name, so "Snapmaker PLA Matte" and
            // "Snapmaker PLA Basic" are both vendor Snapmaker, type PLA. Prefer a candidate
            // whose name mentions the spool's sub type, and rank by colour within that
            // preference, so a matte spool stops resolving to whichever variant happens to
            // sit nearest in colour.
            // A profile's own filament_subtype counts too. When the tag has no sub type, a
            // legacy tag may still name the variant in its vendor field ("Anycubic PLA Silk"),
            // so look for the profile's subtype, or the whole tag vendor, there instead.
            const std::string p_sub = p.config.has("filament_subtype") ? p.config.opt_string("filament_subtype", 0u) : std::string();
            const bool matches_sub  = !sub_wanted.empty() ?
                                          (contains_sub_type_ci(p.name, sub_wanted) || contains_sub_type_ci(p_sub, sub_wanted)) :
                                          (contains_word_ci(tag_vendor, p_sub) || contains_word_ci(p.name, tag_vendor) ||
                                           contains_word_ci(tag_vendor, p.name));
            if (best_matches_sub && !matches_sub)
                continue;

            if ((matches_sub && !best_matches_sub) || distance < best_color_distance) {
                best_color_distance = distance;
                best_match_id       = p.filament_id;
                best_matches_sub    = matches_sub;
            }
        }
    }
    return best_match_id;
}

} // anonymous namespace

std::string SnapmakerPrinterAgent::find_closest_color_preset_by_vendor_and_type(const PresetCollection& filaments,
                                                                                const std::string&      vendor_name,
                                                                                const std::string&      filament_type,
                                                                                const std::string&      sub_type,
                                                                                const std::string&      color_rgba)
{
    // The printer returns RGBA in the format RRGGBBAA, but profiles store color as #RRGGBB,
    // so we must remove # and ignore alpha channel for distance calculation.
    // An unusable colour cannot rank candidates; leave the match to the type-only lookup.
    unsigned int target_color_value = 0;
    if (!parse_rgb(color_rgba, target_color_value))
        return {};

    const std::string sub_wanted = normalize_fibre_sub_type(trim_and_upper(sub_type));

    // The spool's own vendor first, with the sub type as a preference. A tag that reports no
    // vendor matches no profile's filament_vendor, so such a spool finds nothing in this pass.
    const std::string vendor_match = closest_preset_by_colour(filaments, vendor_name, vendor_name, filament_type,
                                                              sub_wanted, target_color_value);
    if (!vendor_match.empty())
        return vendor_match;

    // Orca ships no preset for this spool's vendor, so fall back to its own Generic presets: they
    // cover the sub types the U1 sees (Generic PLA Silk @U1, Generic PLA High Speed @U1,
    // Generic PETG HF @U1) and carry no other brand's temperatures or flow. That beats another
    // brand's preset of the same finish, and it beats the caller's fallback,
    // filament_id_by_type(), which takes the first visible preset of the type whatever its vendor
    // and finish.
    return closest_preset_by_colour(filaments, GENERIC_FILAMENT_VENDOR, vendor_name, filament_type, sub_wanted,
                                    target_color_value);
}

SnapmakerPrinterAgent::SnapmakerPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir)) {}

AgentInfo SnapmakerPrinterAgent::get_agent_info_static()
{
    return AgentInfo{SNAPMAKER_PRINTER_AGENT_ID, "Snapmaker", SNAPMAKER_AGENT_VERSION, "Snapmaker printer agent"};
}

std::string SnapmakerPrinterAgent::combine_filament_type(const std::string& type, const std::string& sub_type)
{
    const std::string base = trim_and_upper(type);
    const std::string sub  = trim_and_upper(sub_type);

    if (base.empty())
        return "PLA";

    if (sub.empty() || sub == "NONE")
        return base;

    // Only CF and GF are material types, whatever fibre loading the tag reports ("CF10"); skip
    // the suffix when the base already carries it.
    const std::string fibre = normalize_fibre_sub_type(sub);
    if (fibre == "CF" || fibre == "GF") {
        const std::string suffix     = "-" + fibre;
        const bool        has_suffix = base.size() >= suffix.size() &&
                                base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0;
        return has_suffix ? base : base + suffix;
    }

    // Every other sub type is a finish or a brand name, not a material type.
    return base;
}

bool SnapmakerPrinterAgent::build_print_options(const PrintParams& params, nlohmann::json& options, std::string& error)
{
    // Moonraker passes each value on as KEY="str(value)" and Klipper reads these as integers, so no booleans
    options = {{"bed_level", params.task_bed_leveling ? 1 : 0},
               {"flow_calibrate", params.task_flow_cali ? 1 : 0},
               {"time_lapse_camera", params.task_record_timelapse ? 1 : 0}};
    // Jobs without a mapping, such as calibrations, print each tool from the toolhead of the same number:
    // the printer resets its table when it gets none.
    if (params.ams_mapping.empty())
        return true;

    const auto mapping = nlohmann::json::parse(params.ams_mapping, nullptr, false);
    if (!mapping.is_array()) {
        error = _u8L("The toolhead assignment of this print could not be read.");
        return false;
    }
    // The dialog gives a source colour to every filament the plate uses, and -1 both to the unused
    // ones and to those mapped to an external spool, which the U1 does not have.
    const auto info = nlohmann::json::parse(params.ams_mapping_info, nullptr, false);

    auto table = nlohmann::json::array();
    for (size_t i = 0; i < mapping.size(); ++i) {
        const int toolhead = mapping[i].is_number_integer() ? mapping[i].get<int>() : PHYSICAL_EXTRUDER_COUNT;
        if (toolhead == -1) {
            if (info.is_array() && i < info.size() && info[i].contains("sourceColor")) {
                error = GUI::format(_u8L("Filament %1% has no toolhead assigned. Select a toolhead for it and try again."), i + 1);
                return false;
            }
            continue;
        }
        if (i >= static_cast<size_t>(LOGICAL_EXTRUDER_COUNT)) {
            error = GUI::format(_u8L("The printer cannot map filament %1%. It supports filaments 1 to %2%."), i + 1,
                                LOGICAL_EXTRUDER_COUNT);
            return false;
        }
        if (toolhead < 0 || toolhead >= PHYSICAL_EXTRUDER_COUNT) {
            error = GUI::format(_u8L("Filament %1% is assigned to a toolhead the printer does not have."), i + 1);
            return false;
        }
        table.push_back({i, toolhead});
    }
    if (table.empty()) {
        error = _u8L("No filament of this print has a toolhead assigned.");
        return false;
    }
    options["map_table"] = std::move(table);
    return true;
}

bool SnapmakerPrinterAgent::print_uploaded_file(const PrintParams& params, const std::string& filename, std::string& error)
{
    nlohmann::json options;
    if (!build_print_options(params, options, error))
        return false;

    // Plain SDCARD_PRINT_FILE would print with whatever map the previous job left. The method is websocket only.
    nlohmann::json response;
    if (!call_websocket_method("server.files.start_local_print", {{"path", filename}, {"options", options}}, START_PRINT_TIMEOUT,
                               response)) {
        error = _u8L("The printer did not confirm that the print started. Check the printer before sending the job again.");
        return false;
    }

    if (const auto rpc_error = response.find("error"); rpc_error != response.end()) {
        if (rpc_error->is_object() && safe_json_int(*rpc_error, "code") == JSONRPC_METHOD_NOT_FOUND)
            error = _u8L("This printer firmware cannot receive a toolhead assignment. Update it to 1.3.0 or later.");
        else if (rpc_error->is_object())
            error = safe_json_string(*rpc_error, "message");
    } else if (const auto result = response.find("result"); result != response.end() && result->is_object()) {
        // A refused start is still a successful RPC
        if (safe_json_string(*result, "state") == "success")
            return true;
        error = safe_json_string(*result, "message");
    }

    if (error.empty())
        error = _u8L("The printer refused to start the print.");
    BOOST_LOG_TRIVIAL(error) << "SnapmakerPrinterAgent::print_uploaded_file: " << response.dump();
    return false;
}

bool SnapmakerPrinterAgent::fetch_filament_info(std::string dev_id, FilamentSyncMode /*sync_mode*/)
{
    std::string url = join_url(device_info.base_url, "/printer/objects/query?print_task_config&filament_detect&extruder&extruder1&extruder2&extruder3");

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    if (!device_info.api_key.empty()) {
        http.header("X-Api-Key", device_info.api_key);
    }
    http.timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_error = err;
            if (status > 0) {
                http_error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: HTTP request failed: " << http_error;
        return false;
    }

    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: Invalid JSON response";
        return false;
    }

    // Navigate to result.status.print_task_config
    if (!json.contains("result") || !json["result"].contains("status") ||
        !json["result"]["status"].contains("print_task_config")) {
        BOOST_LOG_TRIVIAL(warning) << "SnapmakerPrinterAgent::fetch_filament_info: Missing print_task_config in response";
        return false;
    }

    auto& ptc = json["result"]["status"]["print_task_config"];

    // Read parallel arrays from print_task_config
    auto filament_exist    = ptc.value("filament_exist", std::vector<bool>{});
    auto filament_type     = ptc.value("filament_type", std::vector<std::string>{});
    auto filament_sub_type = ptc.value("filament_sub_type", std::vector<std::string>{});
    auto filament_color    = ptc.value("filament_color_rgba", std::vector<std::string>{});
    auto filament_vendor   = ptc.value("filament_vendor", std::vector<std::string>{});

    const int slot_count = static_cast<int>(filament_exist.size());
    if (slot_count == 0) {
        BOOST_LOG_TRIVIAL(info) << "SnapmakerPrinterAgent::fetch_filament_info: No filament slots reported";
        return false;
    }

    // Read NFC filament_detect data for temperature info (optional)
    nlohmann::json nfc_info;
    if (json["result"]["status"].contains("filament_detect") &&
        json["result"]["status"]["filament_detect"].contains("info")) {
        nfc_info = json["result"]["status"]["filament_detect"]["info"];
    }

    const auto& status      = json["result"]["status"];
    const auto  color_multi = ptc.value("filament_color_multi", nlohmann::json::array());

    static const std::string empty_str;
    static const std::string default_color = "FFFFFFFF";

    std::vector<AmsTrayData> trays;
    trays.reserve(slot_count);

    for (int i = 0; i < slot_count; ++i) {
        AmsTrayData tray;
        tray.slot_index   = i;
        tray.has_filament = filament_exist[i];

        if (tray.has_filament) {
            const std::string raw_sub_type = safe_at(filament_sub_type, i, empty_str);
            tray.tray_type     = combine_filament_type(safe_at(filament_type, i, empty_str), raw_sub_type);
            tray.tray_color    = safe_at(filament_color, i, empty_str);
            if (tray.tray_color.empty())
                tray.tray_color = default_color;

            if (trim_and_upper(raw_sub_type) != "NONE")
                tray.tray_sub_type = raw_sub_type;

            // Slots and toolheads are one to one, and Klipper names them extruder, extruder1, ...
            if (const auto head = status.find(i == 0 ? "extruder" : "extruder" + std::to_string(i));
                head != status.end() && head->is_object()) {
                if (const auto size = head->find("nozzle_diameter"); size != head->end() && size->is_number())
                    tray.nozzle_diameter = size->get<float>();
            }

            if (color_multi.is_array() && i < static_cast<int>(color_multi.size()) && color_multi[i].is_object()) {
                const auto& multi = color_multi[i];
                const int   count = safe_json_int(multi, "nums");
                if (const auto colors = multi.find("colors"); count > 1 && colors != multi.end() && colors->is_array()) {
                    for (int k = 0; k < count && k < static_cast<int>(colors->size()); ++k) {
                        if ((*colors)[k].is_string())
                            tray.tray_colors.push_back((*colors)[k].get<std::string>());
                    }
                }
            }

            auto* bundle = GUI::wxGetApp().preset_bundle;
            // Try to find a matching preset for this filament based on vendor, type and color.
            // If not found, default to traditional search by type only or generic type mapping.
            if (bundle) {
                std::string vendor   = safe_at(filament_vendor, i, empty_str);
                std::string sub_type = raw_sub_type;
                if (trim_and_upper(sub_type) == "NONE")
                    sub_type.clear();
                std::string filament_id = find_closest_color_preset_by_vendor_and_type(bundle->filaments, vendor, tray.tray_type,
                                                                                       sub_type, tray.tray_color);

                if (!filament_id.empty()) {
                    tray.tray_info_idx = filament_id;
                    BOOST_LOG_TRIVIAL(warning) << "Filament sync: Found profile for slot " << i << ": "
                                               << filament_id;
                } else {
                    tray.tray_info_idx = bundle->filaments.filament_id_by_type(tray.tray_type);
                    if (tray.tray_info_idx.empty())
                        tray.tray_info_idx = map_filament_type_to_generic_id(tray.tray_type);
                }
            } else {
                tray.tray_info_idx = map_filament_type_to_generic_id(tray.tray_type);
            }

            // Extract NFC temperature data if available
            if (nfc_info.is_array() && i < static_cast<int>(nfc_info.size()) && nfc_info[i].is_object()) {
                auto& nfc_slot = nfc_info[i];
                tray.bed_temp  = safe_json_int(nfc_slot, "BED_TEMP");
                // build_ams_payload publishes this as `nozzle_temp_max`, and FIRST_LAYER_TEMP is 0 for external tags.
                tray.nozzle_temp = safe_json_int(nfc_slot, "HOTEND_MAX_TEMP");
            }
        }

        trays.emplace_back(std::move(tray));
    }

    build_ams_payload(1, slot_count - 1, trays);
    return true;
}

} // namespace Slic3r
