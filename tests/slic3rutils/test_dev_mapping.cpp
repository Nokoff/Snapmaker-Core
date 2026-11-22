// Match the include environment that libslic3r_gui TUs get from pchheader.hpp: Windows.h with
// WIN32_LEAN_AND_MEAN/NOMINMAX must come first so rpcndr.h's `byte` is processed before <cstddef>
// makes std::byte a competing candidate (otherwise the Windows COM headers pulled in via
// DeviceManager.hpp error with an ambiguous `byte`). wx/timer.h must precede DeviceManager.hpp,
// which includes DeviceErrorDialog.hpp (uses wxTimerEvent) before its own wx/timer.h include.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "libslic3r/ProjectTask.hpp"
#include <vector>
#include <catch2/catch_message.hpp>

#include <catch2/catch_all.hpp>

#include <cstddef>
#include <string>

#include <wx/timer.h>

#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevMapping.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"

#include <nlohmann/json.hpp>
#include "slic3r/GUI/DeviceCore/DevFilaSwitch.h"
#include "slic3r/GUI/DeviceCore/DevUtil.h"

using json = nlohmann::json;
using namespace Slic3r;

TEST_CASE("AMS tray placeholder state follows the latest status", "[DevFilaSystem]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");

    const json empty_slot = json::parse(R"({
        "ams": {
            "tray_exist_bits": "0",
            "ams": [ { "id": "0", "info": "00000001", "tray": [
                { "id": "0", "tray_slot_placeholder": "1", "tray_color": "00000000" }
            ] } ]
        }
    })");
    DevFilaSystemParser::ParseV1_0(empty_slot, &obj, obj.GetFilaSystem().get(), false);

    DevAmsTray* tray = obj.GetFilaSystem()->GetAmsTray("0", "0");
    REQUIRE(tray != nullptr);
    REQUIRE(tray->is_slot_placeholder);

    const json loaded_slot = json::parse(R"({
        "ams": {
            "tray_exist_bits": "1",
            "ams": [ { "id": "0", "info": "00000001", "tray": [
                { "id": "0", "tray_color": "FF0000FF" }
            ] } ]
        }
    })");
    DevFilaSystemParser::ParseV1_0(loaded_slot, &obj, obj.GetFilaSystem().get(), false);

    CHECK_FALSE(tray->is_slot_placeholder);
}

TEST_CASE("An AMS unit is shown under the name its status gives it", "[DevFilaSystem]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");

    const json named = json::parse(R"({
        "ams": { "ams": [ { "id": "0", "info": "00000001", "display_name": "Qidi Box 1", "tray": [] } ] }
    })");
    DevFilaSystemParser::ParseV1_0(named, &obj, obj.GetFilaSystem().get(), false);

    DevAms* ams = obj.GetFilaSystem()->GetAmsById("0");
    REQUIRE(ams != nullptr);
    CHECK(ams->GetDisplayName().ToStdString() == "Qidi Box 1");

    // Without a name the unit falls back to its type: an AMS (info type 1) with id 0 is AMS-1
    const json unnamed = json::parse(R"({
        "ams": { "ams": [ { "id": "0", "info": "00000001", "tray": [] } ] }
    })");
    DevFilaSystemParser::ParseV1_0(unnamed, &obj, obj.GetFilaSystem().get(), false);

    CHECK(ams->GetDisplayName().ToStdString() == "AMS-1");
}

TEST_CASE("Switch-bound AMS trays map to the left extruder", "[DevMapping]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");

    // aux bit 29 = Filament Track Switch installed (DevFilaSwitch.cpp:69-77)
    obj.GetFilaSwitch()->ParseFilaSwitchInfo(json::parse(R"({"aux":"20000000"})"));
    REQUIRE(obj.GetFilaSwitch()->IsInstalled());

    // info bits: 0-3 type(1=AMS), 8-11 extruder(0xE=switch-bound), 24-27 bind_switch_in(0)
    // tray_exist_bits bit 0 marks AMS 0 / tray 0 present so the mapping result survives the
    // is_exists check in is_valid_mapping_result (DevFilaSystem.cpp:769).
    // tray_info_idx/tray_type are intentionally omitted: the tray parse resolves the display
    // filament type via MachineObject::setting_id_to_type(), which reads the GUI preset bundle
    // (wxGetApp().preset_bundle) — unavailable in this headless unit test. The tray filament
    // type (only needed for the type-match below) is set directly after the parse instead.
    json print_push = json::parse(R"({
        "ams": {
            "tray_exist_bits": "1",
            "ams": [ {
                "id": "0",
                "info": "00000E01",
                "tray": [ { "id": "0", "tray_color": "FF0000FF" } ]
            } ]
        }
    })");
    DevFilaSystemParser::ParseV1_0(print_push, &obj, obj.GetFilaSystem().get(), false);

    const auto& ams_list = obj.GetFilaSystem()->GetAmsList();
    REQUIRE(ams_list.count("0") == 1);
    REQUIRE(ams_list.at("0")->GetBindedExtruderSet().count(MAIN_EXTRUDER_ID) == 1);
    REQUIRE(ams_list.at("0")->GetBindedExtruderSet().count(DEPUTY_EXTRUDER_ID) == 1);

    DevAmsTray* tray = obj.GetFilaSystem()->GetAmsTray("0", "0");
    REQUIRE(tray != nullptr);
    tray->m_fila_type = "PLA";

    FilamentInfo fila;
    fila.id    = 0;
    fila.type  = "PLA";
    fila.color = "FF0000FF";

    std::vector<FilamentInfo> result;
    std::vector<bool> map_opt(4, false);       // MappingOption: LEFT_AMS,RIGHT_AMS,LEFT_EXT,RIGHT_EXT (DevMapping.h:13-19)
    map_opt[MappingOption::USE_LEFT_AMS] = true;

    DevMappingUtil::ams_filament_mapping(&obj, {fila}, result, map_opt, {}, false);

    // A switch-bound AMS feeds BOTH extruders, so a left-only mapping request
    // must still land the filament on the AMS tray.
    REQUIRE(result.size() == 1);
    CHECK(result[0].tray_id == 0);
    CHECK(result[0].ams_id == "0");
}

TEST_CASE("Without a switch the binding set equals the single bound extruder", "[DevMapping]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    REQUIRE_FALSE(obj.GetFilaSwitch()->IsInstalled());

    // AMS 0: info extruder nibble = MAIN (right). AMS 1: nibble = DEPUTY (left).
    // AMS 2: no "info" key at all (old X1/P1 firmware) -> must default to MAIN.
    // tray_exist_bits: bit ams_id*4+tray_id -> 0x111 marks tray 0 of each AMS.
    json print_push = json::parse(R"({
        "ams": {
            "tray_exist_bits": "111",
            "ams": [
                { "id": "0", "info": "00000001", "tray": [ { "id": "0", "tray_color": "FF0000FF" } ] },
                { "id": "1", "info": "00000101", "tray": [ { "id": "0", "tray_color": "00FF00FF" } ] },
                { "id": "2",                     "tray": [ { "id": "0", "tray_color": "0000FFFF" } ] }
            ]
        }
    })");
    DevFilaSystemParser::ParseV1_0(print_push, &obj, obj.GetFilaSystem().get(), false);

    // The invariant that keeps the binding-set mapping filter behavior-preserving for
    // ordinary printers: GetBindedExtruderSet() == { GetExtruderId() }, info key or not.
    const auto& ams_list = obj.GetFilaSystem()->GetAmsList();
    REQUIRE(ams_list.count("0") == 1);
    REQUIRE(ams_list.count("1") == 1);
    REQUIRE(ams_list.count("2") == 1);
    for (const char* id : {"0", "1", "2"}) {
        const auto& ams = ams_list.at(id);
        INFO("ams " << id);
        REQUIRE(ams->GetBindedExtruderSet().size() == 1);
        REQUIRE(ams->GetBindedExtruderSet().count(ams->GetExtruderId()) == 1);
    }
    REQUIRE(ams_list.at("0")->GetExtruderId() == MAIN_EXTRUDER_ID);
    REQUIRE(ams_list.at("1")->GetExtruderId() == DEPUTY_EXTRUDER_ID);
    REQUIRE(ams_list.at("2")->GetExtruderId() == MAIN_EXTRUDER_ID);

    for (const char* id : {"0", "1", "2"}) {
        DevAmsTray* tray = obj.GetFilaSystem()->GetAmsTray(id, "0");
        REQUIRE(tray != nullptr);
        tray->m_fila_type = "PLA";
    }

    // A left-only request must exclude the MAIN-bound AMSes: the red filament exactly
    // matches AMS 0's red tray, so landing anywhere but AMS 1 (or unmapped) means the
    // exclusion is broken.
    FilamentInfo fila;
    fila.id    = 0;
    fila.type  = "PLA";
    fila.color = "FF0000FF";

    std::vector<FilamentInfo> result;
    std::vector<bool> map_opt(4, false);
    map_opt[MappingOption::USE_LEFT_AMS] = true;
    DevMappingUtil::ams_filament_mapping(&obj, {fila}, result, map_opt, {}, false);
    REQUIRE(result.size() == 1);
    CHECK(result[0].ams_id != "0");
    CHECK(result[0].ams_id != "2");

    // The mirrored right-only request maps to the exact-match MAIN-bound AMS.
    result.clear();
    map_opt[MappingOption::USE_LEFT_AMS]  = false;
    map_opt[MappingOption::USE_RIGHT_AMS] = true;
    DevMappingUtil::ams_filament_mapping(&obj, {fila}, result, map_opt, {}, false);
    REQUIRE(result.size() == 1);
    CHECK(result[0].ams_id == "0");
}

TEST_CASE("Switch-bound AMS with an invalid track is excluded from mapping", "[DevMapping]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    obj.GetFilaSwitch()->ParseFilaSwitchInfo(json::parse(R"({"aux":"20000000"})"));
    REQUIRE(obj.GetFilaSwitch()->IsInstalled());

    // info bits 24-27 = 0xF: switch-bound (0xE) but the input track is not yet valid -
    // the transient while the device is still homing the switch. The AMS must survive
    // (display keeps working) with an EMPTY binding set that excludes it from mapping.
    json print_push = json::parse(R"({
        "ams": {
            "tray_exist_bits": "1",
            "ams": [ { "id": "0", "info": "0F000E01", "tray": [ { "id": "0", "tray_color": "FF0000FF" } ] } ]
        }
    })");
    DevFilaSystemParser::ParseV1_0(print_push, &obj, obj.GetFilaSystem().get(), false);

    const auto& ams_list = obj.GetFilaSystem()->GetAmsList();
    REQUIRE(ams_list.count("0") == 1);
    REQUIRE(ams_list.at("0")->GetBindedExtruderSet().empty());
    REQUIRE_FALSE(ams_list.at("0")->GetSwitcherPos().has_value());
    REQUIRE_FALSE(obj.GetFilaSwitch()->IsReady());

    DevAmsTray* tray = obj.GetFilaSystem()->GetAmsTray("0", "0");
    REQUIRE(tray != nullptr);
    tray->m_fila_type = "PLA";

    FilamentInfo fila;
    fila.id    = 0;
    fila.type  = "PLA";
    fila.color = "FF0000FF";

    std::vector<FilamentInfo> result;
    std::vector<bool> map_opt(4, false);
    map_opt[MappingOption::USE_LEFT_AMS]  = true;
    map_opt[MappingOption::USE_RIGHT_AMS] = true;
    DevMappingUtil::ams_filament_mapping(&obj, {fila}, result, map_opt, {}, false);
    REQUIRE(result.size() == 1);
    CHECK(result[0].tray_id == -1);

    // Without the switch, the same 0xE AMS is dropped from the list entirely.
    MachineObject obj_no_switch(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    REQUIRE_FALSE(obj_no_switch.GetFilaSwitch()->IsInstalled());
    DevFilaSystemParser::ParseV1_0(print_push, &obj_no_switch, obj_no_switch.GetFilaSystem().get(), false);
    REQUIRE(obj_no_switch.GetFilaSystem()->GetAmsList().count("0") == 0);
}

namespace {

struct U1Slot
{
    std::string color; // RRGGBBAA, or empty for an empty slot
    std::string type     = "PLA";
    std::string sub_type = "";
    float       diameter = 0.4f;
    std::vector<std::string> colors = {};
};

// The toolhead each wanted filament gets from a U1 loaded with `slots`.
std::vector<int> map_on_u1(const std::vector<U1Slot>& slots, const std::vector<FilamentInfo>& wanted)
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");

    json     trays = json::array();
    unsigned exist = 0;
    for (size_t i = 0; i < slots.size(); ++i) {
        json tray{{"id", std::to_string(i)}, {"tray_color", slots[i].color.empty() ? "00000000" : slots[i].color}};
        tray["cols"]                 = slots[i].colors.empty() ? json::array({tray["tray_color"]}) : json(slots[i].colors);
        tray["tray_sub_type"]        = slots[i].sub_type;
        tray["tray_nozzle_diameter"] = slots[i].diameter;
        trays.push_back(tray);
        if (!slots[i].color.empty())
            exist |= 1u << i;
    }
    const char digit[] = {"0123456789ABCDEF"[exist], 0};
    const json push{{"ams", {{"tray_exist_bits", digit}, {"ams", json::array({{{"id", "0"}, {"info", "0002"}, {"tray", trays}}})}}}};
    DevFilaSystemParser::ParseV1_0(push, &obj, obj.GetFilaSystem().get(), false);
    // Set after the parse, which resolves types through the GUI's preset bundle
    for (size_t i = 0; i < slots.size(); ++i) {
        if (DevAmsTray* tray = obj.GetFilaSystem()->GetAmsTray("0", std::to_string(i)))
            tray->m_fila_type = slots[i].type;
    }

    std::vector<FilamentInfo> result;
    DevMappingUtil::snapmaker_filament_mapping(&obj, wanted, result);
    std::vector<int> toolheads;
    for (const FilamentInfo& info : result)
        toolheads.push_back(info.tray_id);
    return toolheads;
}

FilamentInfo wanted_filament(const std::string& color, const std::string& type = "PLA", double diameter = 0.4)
{
    FilamentInfo info;
    info.color           = color;
    info.type            = type;
    info.nozzle_diameter = diameter;
    return info;
}

} // namespace

TEST_CASE("A dark colour maps to a U1 slot of its hue instead of black", "[DevMapping][Snapmaker]")
{
    const auto [wanted, coloured] = GENERATE(table<std::string, std::string>({
        {"#003000FF", "409040FF"}, // green
        {"#500808FF", "C04040FF"}, // red
        {"#080840FF", "8080FFFF"}, // blue
    }));
    CAPTURE(wanted, coloured);

    CHECK(map_on_u1({{"000000FF"}, {coloured}}, {wanted_filament(wanted)}) == std::vector<int>{1});
}

TEST_CASE("A neutral colour or a different hue leaves black as the U1 match", "[DevMapping][Snapmaker]")
{
    const auto [wanted, coloured] = GENERATE(table<std::string, std::string>({
        {"#000000FF", "409040FF"}, // black
        {"#202020FF", "409040FF"}, // charcoal
        {"#020301FF", "409040FF"}, // hue is noise this dark
        {"#003000FF", "E03030FF"}, // another hue
    }));
    CAPTURE(wanted, coloured);

    CHECK(map_on_u1({{"000000FF"}, {coloured}}, {wanted_filament(wanted)}) == std::vector<int>{0});
}

TEST_CASE("The nearest coloured U1 slot wins over a brighter one of the same hue", "[DevMapping][Snapmaker]")
{
    CHECK(map_on_u1({{"000000FF"}, {"003000FF"}, {"409040FF"}}, {wanted_filament("#003000FF")}) == std::vector<int>{1});
    CHECK(map_on_u1({{"407040FF"}, {"80A080FF"}}, {wanted_filament("#408040FF")}) == std::vector<int>{0});
}

TEST_CASE("Each U1 filament takes its closest slot even when another filament took it", "[DevMapping][Snapmaker]")
{
    const std::vector<FilamentInfo> wanted{wanted_filament("#FF0000FF"), wanted_filament("#F01010FF"), wanted_filament("#FFFFFFFF")};

    CHECK(map_on_u1({{"FFFFFFFF"}, {"FF0000FF"}, {"0000FFFF"}}, wanted) == std::vector<int>{1, 1, 0});
}

TEST_CASE("Equally close U1 slots resolve to the lowest toolhead", "[DevMapping][Snapmaker]")
{
    CHECK(map_on_u1({{"0000FFFF"}, {"FF0000FF"}, {"FF0000FF"}}, {wanted_filament("#FF0000FF")}) == std::vector<int>{1});
}

TEST_CASE("A U1 filament only maps to a slot of its material", "[DevMapping][Snapmaker]")
{
    const std::vector<U1Slot> slots{{"FF0000FF", "PETG"}, {"0000FFFF", "PLA"}, {"00FF00FF", "BREAKAWAY", "Support"}};

    CHECK(map_on_u1(slots, {wanted_filament("#FF0000FF")}) == std::vector<int>{1});
    CHECK(map_on_u1(slots, {wanted_filament("#FF0000FF", "PLA-S")}) == std::vector<int>{1});
    CHECK(map_on_u1(slots, {wanted_filament("#FF0000FF", "Breakaway Support")}) == std::vector<int>{2});
    CHECK(map_on_u1(slots, {wanted_filament("#FF0000FF", "ABS")}) == std::vector<int>{-1});
}

TEST_CASE("A U1 filament only maps to a toolhead with the nozzle it was sliced for", "[DevMapping][Snapmaker]")
{
    const std::vector<U1Slot> slots{{"FF0000FF", "PLA", "", 0.4f}, {"0000FFFF", "PLA", "", 0.6f}};

    CHECK(map_on_u1(slots, {wanted_filament("#FF0000FF", "PLA", 0.6)}) == std::vector<int>{1});
    CHECK(map_on_u1(slots, {wanted_filament("#FF0000FF", "PLA", 0.8)}) == std::vector<int>{-1});
}

TEST_CASE("An empty U1 slot is never mapped", "[DevMapping][Snapmaker]")
{
    CHECK(map_on_u1({{""}, {"FFFFFFFF"}}, {wanted_filament("#000000FF")}) == std::vector<int>{1});
}

TEST_CASE("A multi-colour U1 spool matches by its closest colour", "[DevMapping][Snapmaker]")
{
    const std::vector<U1Slot> slots{{"F0F000FF"}, {"0000FFFF", "PLA", "", 0.4f, {"0000FFFF", "FFFF00FF"}}};

    CHECK(map_on_u1(slots, {wanted_filament("#FFFF00FF")}) == std::vector<int>{1});
}
