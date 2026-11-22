#include "libslic3r/ProjectTask.hpp"
#include <cstdlib>
#include <cassert>
#include <cstdio>
#include <boost/log/trivial.hpp>
#include <limits>

#include <map>
#include <vector>
#include <string>
#include <utility>
#include <wx/colour.h>
#include <set>
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "DevMapping.h"
#include "DevFilaSystem.h"
#include "DevUtil.h"

// TODO: remove this include
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GuiColor.hpp"
#include "slic3r/Utils/ColorSpaceConvert.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>

namespace Slic3r
{
    bool DevMappingUtil::is_valid_mapping_result(const MachineObject* obj, std::vector<FilamentInfo>& result, bool check_empty_slot)
    {
        if (result.empty()) return false;

        for (int i = 0; i < result.size(); i++)
        {
            // invalid mapping result
            if (result[i].tray_id < 0)
            {
                if (result[i].ams_id.empty() && result[i].slot_id.empty())
                {
                    return false;
                }
            }
            else
            {

                auto ams_item = obj->GetFilaSystem()->GetAmsById(result[i].ams_id);
                if (ams_item == nullptr)
                {
                    if ((result[i].ams_id != std::to_string(VIRTUAL_TRAY_MAIN_ID)) &&
                        (result[i].ams_id != std::to_string(VIRTUAL_TRAY_DEPUTY_ID)))
                    {
                        result[i].tray_id = -1;
                        return false;
                    }
                }
                else
                {
                    if (check_empty_slot)
                    {
                        auto tray_item = ams_item->GetTrays().find(result[i].slot_id);
                        if (tray_item == ams_item->GetTrays().end())
                        {
                            result[i].tray_id = -1;
                            return false;
                        }
                        else
                        {
                            if (!tray_item->second->is_exists)
                            {
                                result[i].tray_id = -1;
                                return false;
                            }
                        }
                    }
                }
            }
        }
        return true;
    }

    // calc distance map
    struct DisValue {
        int  tray_id;
        float distance;
        bool  is_same_color = true;
        bool  is_type_match = true;
    };

    static void _parse_tray_info(int ams_id, int slot_id, DevAms::AmsType type, DevAmsTray tray, FilamentInfo& result)
    {
        result.color = tray.color;
        result.type = tray.get_filament_type();
        result.filament_id = tray.setting_id;
        result.ctype = tray.ctype;
        result.colors = tray.cols;

        /*for new ams mapping*/
        result.ams_id = std::to_string(ams_id);
        result.slot_id = std::to_string(slot_id);

        if (ams_id == VIRTUAL_TRAY_MAIN_ID || ams_id == VIRTUAL_TRAY_DEPUTY_ID)
        {
            result.tray_id = atoi(tray.id.c_str());
            result.id = atoi(tray.id.c_str());
        }
        else
        {
            if (type == DevAms::N3S)
            {
                result.id = ams_id + slot_id;
            }
            else if (type == DevAms::AMS_LITE_MIXED)
            {
                result.id = AMS_LITE_MIXED_TRAY_INDEX_OFFSET + slot_id;
            }
            else
            {
                result.id = ams_id * 4 + slot_id;
            }
        }
    }

    namespace {

    struct LabColour
    {
        double l, a, b;
        double chroma() const { return std::hypot(a, b); }
        double hue() const { return std::atan2(b, a); }
    };

    // RGB2Lab rounds the sRGB constants differently, which can change the winner between close colours,
    // so this uses the ones the U1's own app uses.
    std::optional<LabColour> parse_lab(std::string hex)
    {
        if (!hex.empty() && hex.front() == '#')
            hex.erase(0, 1);
        if ((hex.size() != 6 && hex.size() != 8) ||
            !std::all_of(hex.begin(), hex.end(), [](unsigned char ch) { return std::isxdigit(ch) != 0; }))
            return std::nullopt;

        std::array<double, 3> rgb;
        for (size_t i = 0; i < rgb.size(); ++i) {
            const double value = std::stoi(hex.substr(2 * i, 2), nullptr, 16) / 255.0;
            rgb[i]             = value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
        }
        const auto f = [](double t) { return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16.0) / 116.0; };
        const double x = f((0.4124564 * rgb[0] + 0.3575761 * rgb[1] + 0.1804375 * rgb[2]) / 0.95047);
        const double y = f(0.2126729 * rgb[0] + 0.7151522 * rgb[1] + 0.072175 * rgb[2]);
        const double z = f((0.0193339 * rgb[0] + 0.119192 * rgb[1] + 0.9503041 * rgb[2]) / 1.08883);
        return LabColour{116.0 * y - 16.0, 500.0 * (x - y), 200.0 * (y - z)};
    }

    // Lower case without separators, so "PLA-CF", "PLA CF" and "pla_cf" are one material
    std::string material_key(const std::string& type)
    {
        std::string key;
        for (const unsigned char ch : type) {
            if (ch != '-' && ch != '_' && ch != ' ')
                key += static_cast<char>(std::tolower(ch));
        }
        return key;
    }

    struct SlotCandidate
    {
        FilamentInfo             info;
        std::optional<LabColour> single_colour; // empty for a multi-colour spool
        double                   distance;
    };

    const SlotCandidate* pick_slot(const std::optional<LabColour>& target, const std::vector<SlotCandidate>& slots)
    {
        const auto closer = [](const SlotCandidate& a, const SlotCandidate* b) {
            return !b || a.distance < b->distance || (a.distance == b->distance && a.info.tray_id < b->info.tray_id);
        };
        const SlotCandidate* best = nullptr;
        for (const SlotCandidate& slot : slots) {
            if (closer(slot, best))
                best = &slot;
        }
        if (!best || best->distance == 0.0)
            return best;

        // CIEDE2000 ranks black closest to many dark colours. When a coloured filament lost to a neutral
        // one, a slot of nearly the same hue that is not far worse wins. These bounds are product policy.
        constexpr double MIN_LIGHTNESS     = 5.0; // hue is noise closer to black
        constexpr double MIN_CHROMA        = 10.0;
        constexpr double NEUTRAL_CHROMA    = 8.0;
        constexpr double PI_VALUE          = 3.14159265358979323846;
        constexpr double MAX_HUE_GAP       = PI_VALUE / 6.0;
        constexpr double MAX_DISTANCE      = 45.0;
        constexpr double MAX_EXTRA_OVER    = 25.0;

        const auto& winner = best->single_colour;
        if (!target || !winner || target->l < MIN_LIGHTNESS || target->chroma() < MIN_CHROMA || winner->chroma() >= NEUTRAL_CHROMA)
            return best;

        const SlotCandidate* preferred = nullptr;
        for (const SlotCandidate& slot : slots) {
            const auto& colour = slot.single_colour;
            if (!colour || colour->l < MIN_LIGHTNESS || colour->chroma() < MIN_CHROMA)
                continue;
            double hue_gap = std::abs(colour->hue() - target->hue());
            hue_gap        = std::min(hue_gap, 2.0 * PI_VALUE - hue_gap);
            if (hue_gap > MAX_HUE_GAP || slot.distance > MAX_DISTANCE || slot.distance > best->distance + MAX_EXTRA_OVER)
                continue;
            if (closer(slot, preferred))
                preferred = &slot;
        }
        return preferred ? preferred : best;
    }

    } // namespace

    int DevMappingUtil::snapmaker_filament_mapping(const MachineObject* obj, const std::vector<FilamentInfo>& filaments, std::vector<FilamentInfo>& result)
    {
        if (filaments.empty())
            return -1;

        result.clear();
        for (const FilamentInfo& filament : filaments) {
            std::string wanted = filament.type;
            // Orca renames support PLA and PA, while the printer reports their base material
            if (wanted == "PLA-S" || wanted == "PA-S")
                wanted.resize(wanted.size() - 2);
            const std::string wanted_key = material_key(wanted);
            const auto        target     = parse_lab(filament.color);

            std::vector<SlotCandidate> slots;
            for (const auto& [ams_key, ams] : obj->GetFilaSystem()->GetAmsList()) {
                const int ams_id = std::atoi(ams_key.c_str());
                for (const auto& [tray_key, tray] : ams->GetTrays()) {
                    if (!tray->is_exists || tray->m_fila_type.empty())
                        continue;
                    if (wanted_key != material_key(tray->m_fila_type) && wanted_key != material_key(tray->m_fila_type + tray->sub_type))
                        continue;
                    if (filament.nozzle_diameter > 0.0 && tray->nozzle_diameter > 0.f &&
                        std::abs(filament.nozzle_diameter - tray->nozzle_diameter) > 0.001)
                        continue;

                    const int     slot_id = std::atoi(tray_key.c_str());
                    SlotCandidate slot{{}, std::nullopt, std::numeric_limits<double>::infinity()};
                    _parse_tray_info(ams_id, slot_id, DevAms::AMS_LITE, *tray, slot.info);
                    slot.info.tray_id = slot.info.id;

                    const bool multi = tray->cols.size() > 1;
                    if (!multi)
                        slot.single_colour = parse_lab(tray->color);
                    if (target) {
                        for (const std::string& hex : multi ? tray->cols : std::vector<std::string>{tray->color}) {
                            if (const auto colour = parse_lab(hex))
                                slot.distance = std::min<double>(slot.distance,
                                    DeltaE00(target->l, target->a, target->b, colour->l, colour->a, colour->b));
                        }
                    }
                    slots.push_back(std::move(slot));
                }
            }

            FilamentInfo mapped;
            if (const SlotCandidate* slot = pick_slot(target, slots)) {
                mapped          = slot->info;
                mapped.distance = static_cast<float>(slot->distance);
            } else {
                mapped.tray_id     = -1;
                mapped.type        = filament.type;
                mapped.filament_id = filament.filament_id;
            }
            mapped.id = filament.id;
            BOOST_LOG_TRIVIAL(info) << "snapmaker_filament_mapping: filament " << mapped.id << " -> toolhead " << mapped.tray_id;
            result.push_back(std::move(mapped));
        }
        return 0;
    }

    int DevMappingUtil::ams_filament_mapping(const MachineObject* obj, const std::vector<FilamentInfo>& filaments, std::vector<FilamentInfo>& result, std::vector<bool> map_opt, std::vector<int> exclude_id, bool nozzle_has_ams_then_ignore_ext)
    {
        if (filaments.empty())
            return -1;

        /////////////////////////
        // Step 1: collect filaments in machine
        std::map<int, FilamentInfo> tray_filaments; // tray_index : tray_color
        bool  left_nozzle_has_ams = false, right_nozzle_has_ams = false;

        const auto& ams_list = obj->GetFilaSystem()->GetAmsList();
        for (auto ams = ams_list.begin(); ams != ams_list.end(); ams++)
        {
            std::string ams_id = ams->second->GetAmsId();
            auto        ams_type = ams->second->GetAmsType();
            // GetAmsType() maps mixed -> AMS_LITE; recover the mixed type so N9 trays index at 24+slot.
            if (ams_type == DevAms::AMS_LITE && ams->second->IsAmsLiteMixed())
            {
                ams_type = DevAms::AMS_LITE_MIXED;
            }
            for (auto tray = ams->second->GetTrays().begin(); tray != ams->second->GetTrays().end(); tray++)
            {
                int ams_id = atoi(ams->first.c_str());
                int tray_id = atoi(tray->first.c_str());
                int tray_index = 0;
                if (ams_type == DevAms::AMS || ams_type == DevAms::AMS_LITE || ams_type == DevAms::N3F)
                {
                    tray_index = ams_id * 4 + tray_id;
                }
                else if (ams_type == DevAms::AMS_LITE_MIXED)
                {
                    tray_index = AMS_LITE_MIXED_TRAY_INDEX_OFFSET + tray_id;
                }
                else if (ams_type == DevAms::N3S)
                {
                    tray_index = ams_id + tray_id;
                }
                else
                {
                    assert(0);
                }

                // skip exclude id
                for (int i = 0; i < exclude_id.size(); i++)
                {
                    if (tray_index == exclude_id[i])
                        continue;
                }
                // push
                FilamentInfo info;
                if (tray->second->is_tray_info_ready())
                {
                    _parse_tray_info(ams_id, tray_id, ams_type, *(tray->second), info);
                }

                //first: left,nozzle=1,map=1   second: right,nozzle=0,map=2
                bool right_ams_valid = (ams->second->GetBindedExtruderSet().count(MAIN_EXTRUDER_ID) != 0) && map_opt[MappingOption::USE_RIGHT_AMS];
                bool left_ams_valid = (ams->second->GetBindedExtruderSet().count(DEPUTY_EXTRUDER_ID) != 0) && map_opt[MappingOption::USE_LEFT_AMS];
                if (right_ams_valid || left_ams_valid)
                {
                    tray_filaments.emplace(std::make_pair(tray_index, info));
                    if (right_ams_valid)
                    {
                        right_nozzle_has_ams = true;
                    }
                    if (left_ams_valid)
                    {
                        left_nozzle_has_ams = true;
                    }
                }
            }
        }

        if (map_opt[MappingOption::USE_RIGHT_EXT] || map_opt[MappingOption::USE_LEFT_EXT])
        {
            for (auto tray : obj->vt_slot)
            {
                bool right_ext_valid = (tray.id == std::to_string(VIRTUAL_TRAY_MAIN_ID) && map_opt[MappingOption::USE_RIGHT_EXT]);
                bool left_ext_valid = (tray.id == std::to_string(VIRTUAL_TRAY_DEPUTY_ID) && map_opt[MappingOption::USE_LEFT_EXT]);
                if (right_ext_valid || left_ext_valid)
                {
                    if (nozzle_has_ams_then_ignore_ext)
                    {
                        if (right_ext_valid && right_nozzle_has_ams)
                        {
                            continue;
                        }
                        if (left_ext_valid && left_nozzle_has_ams)
                        {
                            continue;
                        }
                    }
                    FilamentInfo info;
                    _parse_tray_info(atoi(tray.id.c_str()), 0, DevAms::EXT_SPOOL, tray, info);
                    tray_filaments.emplace(std::make_pair(info.tray_id, info));
                }
            }
        }

        /////////////////////////
        // Step 2: collect the distances of filaments_in_slicing to filaments_in_machine
        char buffer[256];
        std::vector<std::vector<DisValue>> distance_map;

        // print title
        ::sprintf(buffer, "F(id)");
        std::string line = std::string(buffer);
        for (auto tray = tray_filaments.begin(); tray != tray_filaments.end(); tray++)
        {
            ::sprintf(buffer, "   AMS%02d", tray->second.id + 1);
            line += std::string(buffer);
        }
        BOOST_LOG_TRIVIAL(info) << "ams_mapping_distance:" << line;// Print the collected filaments

        for (int i = 0; i < filaments.size(); i++)
        {
            std::vector<DisValue> rol;
            ::sprintf(buffer, "F(%02d)", filaments[i].id + 1);
            line = std::string(buffer);
            for (auto tray = tray_filaments.begin(); tray != tray_filaments.end(); tray++)
            {
                DisValue val;
                val.tray_id = tray->second.id;
                wxColour c = wxColour(filaments[i].color);
                wxColour tray_c = DevAmsTray::decode_color(tray->second.color);
                val.distance = GUI::calc_color_distance(c, tray_c);
                if (filaments[i].type != tray->second.type)
                {
                    val.distance = 999999;
                    val.is_type_match = false;
                }
                else
                {
                    if (c.Alpha() != tray_c.Alpha())
                        val.distance = 999999;
                    val.is_type_match = true;
                }
                ::sprintf(buffer, "  %6.0f", val.distance);
                line += std::string(buffer);
                rol.push_back(val);
            }
            BOOST_LOG_TRIVIAL(info) << "ams_mapping_distance:" << line;
            distance_map.push_back(rol);
        }

        /////////////////////////
        // Step 3: do mapping algorithm

        // setup the mapping result
        for (int i = 0; i < filaments.size(); i++)
        {
            FilamentInfo info;
            info.id = filaments[i].id;
            info.tray_id = -1;
            info.type = filaments[i].type;
            info.filament_id = filaments[i].filament_id;
            result.push_back(info);
        }

        // traverse the mapping
        std::set<int> picked_src;
        std::set<int> picked_tar;
        for (int k = 0; k < distance_map.size(); k++)
        {
            float min_val = std::numeric_limits<float>::max();
            int picked_src_idx = -1;
            int picked_tar_idx = -1;
            for (int i = 0; i < distance_map.size(); i++)
            {
                if (picked_src.find(i) != picked_src.end())
                    continue;

                // try to mapping to different tray
                for (int j = 0; j < distance_map[i].size(); j++)
                {
                    if (picked_tar.find(j) != picked_tar.end())
                    {
                        if (distance_map[i][j].is_same_color
                            && distance_map[i][j].is_type_match
                            && distance_map[i][j].distance < (float)0.0001)
                        {
                            min_val = distance_map[i][j].distance;
                            picked_src_idx = i;
                            picked_tar_idx = j;
                            tray_filaments[picked_tar_idx].distance = min_val;
                        }
                        continue;
                    }

                    if (distance_map[i][j].is_same_color
                        && distance_map[i][j].is_type_match)
                    {
                        if (min_val > distance_map[i][j].distance)
                        {

                            min_val = distance_map[i][j].distance;
                            picked_src_idx = i;
                            picked_tar_idx = j;
                            tray_filaments[picked_tar_idx].distance = min_val;
                        }
                        else if (min_val == distance_map[i][j].distance && filaments[picked_src_idx].filament_id != tray_filaments[picked_tar_idx].filament_id && filaments[i].filament_id == tray_filaments[j].filament_id)
                        {

                            picked_src_idx = i;
                            picked_tar_idx = j;
                        }
                    }
                }

                // take a retry to mapping to used tray
                if (picked_src_idx < 0 || picked_tar_idx < 0)
                {
                    for (int j = 0; j < distance_map[i].size(); j++)
                    {
                        if (distance_map[i][j].is_same_color && distance_map[i][j].is_type_match)
                        {
                            if (min_val > distance_map[i][j].distance)
                            {
                                min_val = distance_map[i][j].distance;
                                picked_src_idx = i;
                                picked_tar_idx = j;
                                tray_filaments[picked_tar_idx].distance = min_val;
                            }
                            else if (min_val == distance_map[i][j].distance && filaments[picked_src_idx].filament_id != tray_filaments[picked_tar_idx].filament_id && filaments[i].filament_id == tray_filaments[j].filament_id)
                            {
                                picked_src_idx = i;
                                picked_tar_idx = j;
                            }
                        }
                    }
                }
            }

            if (picked_src_idx >= 0 && picked_tar_idx >= 0)
            {
                auto tray = tray_filaments.find(distance_map[k][picked_tar_idx].tray_id);

                if (tray != tray_filaments.end())
                {
                    result[picked_src_idx].tray_id = tray->first;

                    result[picked_src_idx].color = tray->second.color;
                    result[picked_src_idx].type = tray->second.type;
                    result[picked_src_idx].distance = tray->second.distance;
                    result[picked_src_idx].filament_id = tray->second.filament_id;
                    result[picked_src_idx].ctype = tray->second.ctype;
                    result[picked_src_idx].colors = tray->second.colors;

                    /*for new ams mapping*/
                    result[picked_src_idx].ams_id = tray->second.ams_id;
                    result[picked_src_idx].slot_id = tray->second.slot_id;
                }

                ::sprintf(buffer, "ams_mapping, picked F(%02d) AMS(%02d), distance=%6.0f", picked_src_idx + 1, picked_tar_idx + 1,
                    distance_map[picked_src_idx][picked_tar_idx].distance);
                BOOST_LOG_TRIVIAL(info) << std::string(buffer);
                picked_src.insert(picked_src_idx);
                picked_tar.insert(picked_tar_idx);
            }
        }

        // Orca: special cases that no AMS available, we select ext slot automatically because we don't have other choice anyway
        if (tray_filaments.size() == 1 && devPrinterUtil::IsVirtualSlot(tray_filaments.begin()->first)) {
            auto ext_tray = tray_filaments.begin();
            for (auto & r : result) {
                if (r.tray_id < 0) {
                    r.tray_id = ext_tray->first;

                    r.color = ext_tray->second.color;
                    r.type = ext_tray->second.type;
                    r.distance = ext_tray->second.distance;
                    r.filament_id = ext_tray->second.filament_id;
                    r.ctype = ext_tray->second.ctype;
                    r.colors = ext_tray->second.colors;

                    /*for new ams mapping*/
                    r.ams_id = ext_tray->second.ams_id;
                    r.slot_id = ext_tray->second.slot_id;
                }
            }
        }

        //check ams mapping result
        if (DevMappingUtil::is_valid_mapping_result(obj, result, true))
        {
            return 0;
        }

        /* for (auto it = result.begin(); it != result.end(); it++) {//This code has never been effective before 2025.03.18
             if (it->distance >= 6000) {
                 it->tray_id = -1;
             }
         }*/
        return 0;
    }

}