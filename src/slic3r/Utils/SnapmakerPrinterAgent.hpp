#pragma once

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "bambu_networking.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace Slic3r {

class PresetCollection;

class SnapmakerPrinterAgent : public MoonrakerPrinterAgent
{
public:
    explicit SnapmakerPrinterAgent(std::string log_dir);
    ~SnapmakerPrinterAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

    // Pick the filament_id of the closest visible base preset for a spool's vendor, type, sub type
    // and colour. Static so the matcher can be exercised without a printer.
    static std::string find_closest_color_preset_by_vendor_and_type(const PresetCollection& filaments,
                                                                    const std::string&      vendor_name,
                                                                    const std::string&      filament_type,
                                                                    const std::string&      sub_type,
                                                                    const std::string&      color_rgba);

    // Combine filament_type + filament_sub_type into a unified type string
    static std::string combine_filament_type(const std::string& type, const std::string& sub_type);

    // The options of the U1's server.files.start_local_print. Fails, saying why in `error`, rather than
    // let a filament print from a toolhead the user did not choose.
    static bool build_print_options(const PrintParams& params, nlohmann::json& options, std::string& error);

protected:
    bool print_uploaded_file(const PrintParams& params, const std::string& filename, std::string& error) override;
    bool supports_flow_calibration() const override { return true; }
};

} // namespace Slic3r
