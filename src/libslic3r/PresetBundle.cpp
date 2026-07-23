Exit code: 0
Wall time: 2.6 seconds
Total output lines: 6089
Output:
#include <cassert>
#include <ctime>

#include "PresetBundle.hpp"
#include "PrintConfig.hpp"
#include "libslic3r.h"
#include "I18N.hpp"
#include "Utils.hpp"
#include "LocalesUtils.hpp"
#include "Model.hpp"
#include "libslic3r_version.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <fstream>
#include <unordered_set>
#include <boost/filesystem.hpp>
#include <boost/algorithm/clamp.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/locale.hpp>
#include <boost/log/trivial.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <miniz/miniz.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

// Mark string for localization and translate.
#define L(s) Slic3r::I18N::translate(s)

// Store the print/filament/printer presets into a "presets" subdirectory of the Slic3rPE config dir.
// This breaks compatibility with the upstream Slic3r if the --datadir is used to switch between the two versions.
//#define SLIC3R_PROFILE_USE_PRESETS_SUBDIR

namespace Slic3r {

static std::vector<std::string> s_project_options {
    "flush_volumes_vector",
    "flush_volumes_matrix",
    // BBS
    "filament_colour",
    "filament_colour_type",
    "filament_multi_colour",
    "wipe_tower_x",
    "wipe_tower_y",
    "wipe_tower_rotation_angle",
    "curr_bed_type",
    "flush_multiplier",
    // Fast-purge mode: project-level purge control, inert at Default.
    "flush_multiplier_fast",
    "prime_volume_mode",
    "nozzle_volume_type",
    "filament_map_mode",
    "filament_map",
    // Per-filament nozzle-volume choice; project-level like filament_map so the per-filament
    // slot resolution survives preset switches.
    "filament_volume_map",
    // Per-filament physical-nozzle choice the grouping engine writes back; project-level so a
    // saved project round-trips the assignment alongside filament_map/filament_volume_map.
    "filament_nozzle_map",
    // Filament Track Switch device state: whether the switch is installed and ready, and
    // whether dynamic per-nozzle filament mapping is active. Persisted with the project and
    // restored from a saved 3mf; reset to false on load and set true only by live device sync.
    "has_filament_switcher",
    "enable_filament_dynamic_map"
};

//Orca: add custom as default
const char *PresetBundle::ORCA_DEFAULT_BUNDLE = "Custom";
const char *PresetBundle::ORCA_DEFAULT_PRINTER_MODEL = "MyKlipper 0.4 nozzle";
const char *PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT = "0.4";
const char *PresetBundle::ORCA_DEFAULT_FILAMENT = "Generic PLA @System";
const char *PresetBundle::ORCA_FILAMENT_LIBRARY = "OrcaFilamentLibrary";
const char *PresetBundle::ORCA_DEFAULT_FILAMENT_PLACEHOLDER = "Default Filament";

DynamicPrintConfig PresetBundle::construct_full_config(
    Preset& in_printer_preset,
    Preset& in_print_preset,
    const DynamicPrintConfig& project_config,
    std::vector<Preset>& in_filament_presets,
    bool apply_extruder,
    std::optional<std::vector<int>> filament_maps_new,
    std::optional<std::vector<int>> filament_volume_maps_new)
{
    DynamicPrintConfig &printer_config = in_printer_preset.config;
    DynamicPrintConfig &print_config   = in_print_preset.config;

    DynamicPrintConfig out;
    out.apply(FullPrintConfig::defaults());
    out.apply(printer_config);
    out.apply(print_config);
    out.apply(project_config);
    out.apply(in_filament_presets[0].config);

    size_t num_filaments = in_filament_presets.size();

    std::vector<int> filament_maps = out.option<ConfigOptionInts>("filament_map")->values;
    std::vector<int> filament_volume_maps(num_filaments, (int)nvtStandard);

    ConfigOptionInts* filament_volume_map_opt = out.option<ConfigOptionInts>("filament_volume_map");
    if (filament_maps_new.has_value())
        filament_maps = *filament_maps_new;
    if (filament_volume_maps_new.has_value())
        filament_volume_maps = *filament_volume_maps_new;
    else if (filament_volume_map_opt && filament_volume_map_opt->values.size() == num_filaments)
        filament_volume_maps = filament_volume_map_opt->values;

    // in some middle state, they may be different
    if (filament_maps.size() != num_filaments) {
        filament_maps.resize(num_filaments, 1);
    }
    if (filament_volume_maps.size() != num_filaments) {
        filament_volume_maps.resize(num_filaments, nvtStandard);
    }

    auto *extruder_diameter = dynamic_cast<const ConfigOptionFloats *>(out.option("nozzle_diameter"));
    // Collect the "compatible_printers_condition" and "inherits" values over all presets (print, filaments, printers) into a single vector.
    std::vector<std::string> compatible_printers_condition;
    std::vector<std::string> compatible_prints_condition;
    std::vector<std::string> inherits;
    std::vector<std::string> filament_ids;
    std::vector<std::string> print_compatible_printers;
    // BBS: add logic for settings check between different system presets
    std::vector<std::string> different_settings;
    std::string              different_print_settings, different_printer_settings;
    compatible_printers_condition.emplace_back(in_print_preset.compatible_printers_condition());

    const ConfigOptionStrings *compatible_printers = print_config.option<ConfigOptionStrings>("compatible_printers", false);
    if (compatible_printers) print_compatible_printers = compatible_printers->values;
    // BBS: add logic for settings check between different system presets
    std::string print_inherits = in_print_preset.inherits();
    inherits.emplace_back(print_inherits);

    // BBS: update printer config related with variants
    std::vector<std::vector<NozzleVolumeType>> nozzle_volume_types;
    int extruder_count = 1, extruder_volume_type_count = 1;
    bool different_extruder = false;
    if (apply_extruder) {
        different_extruder = out.support_different_extruders(extruder_count);
        extruder_volume_type_count = out.get_extruder_nozzle_volume_count(extruder_count, nozzle_volume_types);

        if ((extruder_count > 1) || different_extruder) {
            // Orca: keep processing variant_1 before variant_2 here; variant_2 slots are resolved
            // against the printer id/variant lists as rewritten by the variant_1 pass, and the
            // composed values depend on that order. Note the order is load-bearing, not correct
            // in general: the variant_2 pass reads the original full-width arrays through indices
            // resolved on the shrunk lists, which mis-reads presets whose variant_2 columns differ
            // per variant (e.g. X2D machine_max_speed_e/machine_max_acceleration_e). The slicing
            // path composes variant_2 first and is unaffected; changing the order here would alter
            // long-standing composed values, so any fix must re-baseline them.
            out.update_values_to_printer_extruders(out, extruder_count, extruder_volume_type_count, nozzle_volume_types, printer_options_with_variant_1, "printer_extruder_id", "printer_extruder_variant");
            out.update_values_to_printer_extruders(out, extruder_count, extruder_volume_type_count, nozzle_volume_types, printer_options_with_variant_2, "printer_extruder_id", "printer_extruder_variant", 2);
            // update print config related with variants
            out.update_values_to_printer_extruders(out, extruder_count, extruder_volume_type_count, nozzle_volume_types, print_options_with_variant, "print_extruder_id", "print_extruder_variant");
        }
    }

    if (num_filaments <= 1) {
        // BBS: update filament config related with variants
        DynamicPrintConfig filament_config = in_filament_presets[0].config;
        if (apply_extruder && ((extruder_count > 1) || different_extruder))
            filament_config.update_values_to_printer_extruders(out, extruder_count, extruder_volume_type_count, nozzle_volume_types, filament_options_with_variant, "", "filament_extruder_variant", 1, filament_maps[0], (NozzleVolumeType)filament_volume_maps[0]);
        out.apply(filament_config);
        compatible_printers_condition.emplace_back(in_filament_presets[0].compatible_printers_condition());
        compatible_prints_condition.emplace_back(in_filament_presets[0].compatible_prints_condition());
        std::string filament_inherits = in_filament_presets[0].inherits();
        inherits.emplace_back(filament_inherits);
        filament_ids.emplace_back(in_filament_presets[0].filament_id);

        std::vector<int> &filament_self_indice = out.option<ConfigOptionInts>("filament_self_index", true)->values;
        int               index_size           = out.option<ConfigOptionStrings>("filament_extruder_variant")->size();
        filament_self_indice.resize(index_size, 1);
    } else {
        std::vector<const DynamicPrintConfig *> filament_configs;
        std::vector<const Preset *>             filament_presets;
        for (const Preset & preset : in_filament_presets) {
            filament_presets.emplace_back(&preset);
            filament_configs.emplace_back(&(preset.config));
        }

        std::vector<DynamicPrintConfig> filament_temp_configs;
        filament_temp_configs.resize(num_filaments);
        for (size_t i = 0; i < num_filaments; ++i) {
            filament_temp_configs[i] = *(filament_configs[i]);
            if (apply_extruder && ((extruder_count > 1) || different_extruder))
                filament_temp_configs[i].update_values_to_printer_extruders(out, extruder_count, extruder_volume_type_count, nozzle_volume_types, filament_options_with_variant, "", "filament_extruder_variant", 1, filament_maps[i], (NozzleVolumeType)filament_volume_maps[i]);
        }

        // loop through options and apply them to the resulting config.
        std::vector<int> filament_variant_count(num_filaments, 1);
        for (const t_config_option_key &key : in_filament_presets[0].config.keys()) {
            if (key == "compatible_prints" || key == "compatible_printers") continue;
            // Get a destination option.
            ConfigOption *opt_dst = out.option(key, false);
            if (opt_dst->is_scalar()) {
                // Get an option, do not create if it does not exist.
                const ConfigOption *opt_src = filament_temp_configs.front().option(key);
                if (opt_src != nullptr) opt_dst->set(opt_src);
            } else {
                // BBS
                ConfigOptionVectorBase *opt_vec_dst = static_cast<ConfigOptionVectorBase *>(opt_dst);
                {
                    if (apply_extruder) {
                        std::vector<const ConfigOption *> filament_opts(num_filaments, nullptr);
                        // Setting a vector value from all filament_configs.
                        for (size_t i = 0; i < filament_opts.size(); ++i) filament_opts[i] = filament_temp_configs[i].option(key);
                        opt_vec_dst->set(filament_opts);
                    } else {
                        for (size_t i = 0; i < num_filaments; ++i) {
                            const ConfigOptionVectorBase *filament_option = static_cast<const ConfigOptionVectorBase *>(filament_temp_configs[i].option(key));
                            if (i == 0)
                                opt_vec_dst->set(filament_option);
                            else
                                opt_vec_dst->append(filament_option);

                            if (key == "filament_extruder_variant") filament_variant_count[i] = filament_option->size();
                        }
                    }
                }
            }
        }

        if (!apply_extruder) {
            // append filament_self_index
            std::vector<int> &filament_self_indice = out.option<ConfigOptionInts>("filament_self_index", true)->values;
            int               index_size           = out.option<ConfigOptionStrings>("filament_extruder_variant")->size();
            filament_self_indice.resize(index_size, 1);
            int k = 0;
            for (size_t i = 0; i < num_filaments; i++) {
                for (size_t j = 0; j < filament_variant_count[i]; j++) { filament_self_indice[k++] = i + 1; }
            }
        }
    }

    // These value types clash between the print and filament profiles. They should be renamed.
    out.erase("compatible_prints");
    out.erase("compatible_prints_condition");
    out.erase("compatible_printers");
    out.erase("compatible_printers_condition");
    out.erase("inherits");
    // BBS: add logic for settings check between different system presets
    out.erase("different_settings_to_system");

    static const char *keys[] = {"support_filament", "support_interface_filament"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        std::string key = std::string(keys[i]);
        auto       *opt = dynamic_cast<ConfigOptionInt *>(out.option(key, false));
        assert(opt != nullptr);
        opt->value = boost::algorithm::clamp<int>(opt->value, 0, int(num_filaments));
    }

    std::vector<std::string> filamnet_preset_names;
    for (auto preset : in_filament_presets) {
        filamnet_preset_names.emplace_back(preset.name);
    }
    out.option<ConfigOptionString>("print_settings_id", true)->value      = in_print_preset.name;
    out.option<ConfigOptionStrings>("filament_settings_id", true)->values = filamnet_preset_names;
    out.option<ConfigOptionString>("printer_settings_id", true)->value    = in_printer_preset.name;
    out.option<ConfigOptionStrings>("filament_ids", true)->values         = filament_ids;
    out.option<ConfigOptionInts>("filament_map", true)->values            = filament_maps;
    out.option<ConfigOptionInts>("filament_volume_map", true)->values     = filament_volume_maps;

    auto add_if_some_non_empty = [&out](std::vector<std::string> &&values, const std::string &key) {
        bool nonempty = false;
        for (const std::string &v : values)
            if (!v.empty()) {
                nonempty = true;
                break;
            }
        if (nonempty) out.set_key_value(key, new ConfigOptionStrings(std::move(values)));
    };
    add_if_some_non_empty(std::move(compatible_printers_condition), "compatible_machine_expression_group");
    add_if_some_non_empty(std::move(compatible_prints_condition), "compatible_process_expression_group");
    add_if_some_non_empty(std::move(inherits), "inherits_group");
    // BBS: add logic for settings check between different system presets
    //add_if_some_non_empty(std::move(different_settings), "different_settings_to_system");
    add_if_some_non_empty(std::move(print_compatible_printers), "print_compatible_printers");

    out.option<ConfigOptionEnumGeneric>("printer_technology", true)->value = ptFFF;
    return out;
}

std::string PresetBundle::find_preset_vendor(const std::string &preset_name, Preset::Type type)
{
    // Get the resources preset directory (contains all bundled vendor profiles)
    fs::path system_dir = fs::path(Slic3r::resources_dir()) / PRESET_PROFILES_DIR;
    if (!fs::exists(system_dir) || !fs::is_directory(system_dir)) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << " Resources profiles directory does not exist: " << system_dir.string();
        return "";
    }

    // Determine which preset list key to search for based on type
    const char* preset_list_key = nullptr;
    if (type == Preset::Type::TYPE_PRINT)
        preset_list_key = BBL_JSON_KEY_PROCESS_LIST;
    else if (type == Preset::Type::TYPE_FILAMENT)
        preset_list_key = BBL_JSON_KEY_FILAMENT_LIST;
    else if (type == Preset::Type::TYPE_PRINTER)
        preset_list_key = BBL_JSON_KEY_MACHINE_LIST;
    else {
        // Not supported for other types
        return "";
    }

    // Iterate through vendor JSON files in the system directory
    for (auto& dir_entry : fs::directory_iterator(system_dir)) {
        std::string vendor_file = dir_entry.path().string();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Checking vendor: " << vendor_file;
        if (!Slic3r::is_json_file(vendor_file))
            continue;

        // Get vendor name (filename without .json extension)
        std::string vendor_name = dir_entry.path().filename().string();
        vendor_name.erase(vendor_name.size() - 5); // Remove ".json"

        try {
            // Load and parse the vendor JSON file
            boost::nowide::ifstream ifs(vendor_file);
            json j;
            ifs >> j;

            // Check if the preset list key exists
            if (!j.contains(preset_list_key))
                continue;

            auto& preset_list = j[preset_list_key];
            if (!preset_list.is_array())
                continue;

            // Search for the preset in the list
            for (auto& preset_entry : preset_list) {
                if (!preset_entry.is_object())
                    continue;

                // Get the preset name
                std::string p_name;
                if (preset_entry.contains(BBL_JSON_KEY_NAME) && preset_entry[BBL_JSON_KEY_NAME].is_string())
                    p_name = preset_entry[BBL_JSON_KEY_NAME].get<std::string>();

                if (p_name != preset_name)
                    continue;

                // Found the preset! Get the vendor name and install the entire bundle
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Found preset " << p_name
                                            << " in vendor bundle " << vendor_name;
                
                return vendor_name;
            }
        }
        catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to find vendor name for " << preset_name << ": " << e.what();
            return "";
        }
    }

    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Could not find vendor for preset " << preset_name;
    return "";
}

PresetBundle::PresetBundle()
    : prints(Preset::TYPE_PRINT, Preset::print_options(), static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()))
    , filaments(Preset::TYPE_FILAMENT, Preset::filament_options(), static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()), ORCA_DEFAULT_FILAMENT_PLACEHOLDER)
    , sla_materials(Preset::TYPE_SLA_MATERIAL, Preset::sla_material_options(), static_cast<const SLAMaterialConfig &>(SLAFullPrintConfig::defaults()))
    , sla_prints(Preset::TYPE_SLA_PRINT, Preset::sla_print_options(), static_cast<const SLAPrintObjectConfig &>(SLAFullPrintConfig::defaults()))
    , printers(Preset::TYPE_PRINTER, Preset::printer_options(), static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()), "Default Printer")
    , physical_printers(PhysicalPrinter::printer_options())
{
    // The following keys are handled by the UI, they do not have a counterpart in any StaticPrintConfig derived classes,
    // therefore they need to be handled differently. As they have no counterpart in StaticPrintConfig, they are not being
    // initialized based on PrintConfigDef(), but to empty values (zeros, empty vectors, empty strings).
    //
    // "compatible_printers", "compatible_printers_condition", "inherits",
    // "print_settings_id", "filament_settings_id", "printer_settings_id", "printer_settings_id"
    // "printer_model", "printer_variant", "default_print_profile…72901 tokens truncated…ct_other_filament_if_incompatible != PresetSelectCompatibleType::Never) {
            // Verify validity of the current filament presets.
            const std::string prefered_filament_profile = prefered_filament_profiles.empty() ? std::string() : prefered_filament_profiles.front();
            if (this->filament_presets.size() == 1) {
                // The compatible profile should have been already selected for the preset editor. Just use it.
            	if (select_other_filament_if_incompatible == PresetSelectCompatibleType::Always || filament_preset_was_compatible.front())
                	this->filament_presets.front() = this->filaments.get_edited_preset().name;
            } else {
                for (size_t idx = 0; idx < this->filament_presets.size(); ++ idx) {
                    std::string &filament_name = this->filament_presets[idx];
                    Preset      *preset = this->filaments.find_preset(filament_name, false);
                    if (preset == nullptr || (! preset->is_compatible && (select_other_filament_if_incompatible == PresetSelectCompatibleType::Always || filament_preset_was_compatible[idx])))
                        // Pick a compatible profile. If there are prefered_filament_profiles, use them.
                        filament_name = this->filaments.first_compatible(
                            PreferedFilamentProfileMatch(preset,
                                (idx < prefered_filament_profiles.size()) ? prefered_filament_profiles[idx] : prefered_filament_profile)).name;
                }
            }
        }
		break;
    }
    case ptSLA:
    {
		assert(printer_preset.config.has("default_sla_print_profile"));
		assert(printer_preset.config.has("default_sla_material_profile"));
		this->sla_prints.update_compatible(printer_preset_with_vendor_profile, nullptr, select_other_print_if_incompatible,
            PreferedPrintProfileMatch(this->sla_prints.get_selected_idx() == size_t(-1) ? nullptr : &this->sla_prints.get_edited_preset(), printer_preset.config.opt_string("default_sla_print_profile")));
        const PresetWithVendorProfile sla_print_preset_with_vendor_profile = this->sla_prints.get_edited_preset_with_vendor_profile();
		this->sla_materials.update_compatible(printer_preset_with_vendor_profile, &sla_print_preset_with_vendor_profile, select_other_filament_if_incompatible,
            PreferedProfileMatch(this->sla_materials.get_selected_idx() == size_t(-1) ? std::string() : this->sla_materials.get_edited_preset().alias, printer_preset.config.opt_string("default_sla_material_profile")));
		break;
	}
    default: break;
    }

    BOOST_LOG_TRIVIAL(info) << boost::format("update_compatibility for all presets exit");
}


std::vector<std::string> PresetBundle::export_current_configs(const std::string &                     path,
                                                              std::function<int(std::string const &)> override_confirm,
                                                              bool                                    include_modify,
                                                              bool                                    export_system_settings)
{
    const Preset &print_preset    = include_modify ? prints.get_edited_preset() : prints.get_selected_preset();
    const Preset &printer_preset  = include_modify ? printers.get_edited_preset() : printers.get_selected_preset();
    std::set<Preset const *> presets { &print_preset, &printer_preset };
    for (auto &f : filament_presets) {
        auto filament_preset = filaments.find_preset(f, include_modify);
        if (filament_preset) presets.insert(filament_preset);
    }

    int overwrite = 0;
    std::vector<std::string> result;
    for (auto preset : presets) {
        if ((preset->is_system  && !export_system_settings) || preset->is_default)
            continue;
        std::string file = path + "/" + preset->name + ".json";
        if (overwrite == 0) overwrite = 1;
        if (boost::filesystem::exists(file) && overwrite < 2) {
            overwrite = override_confirm(preset->name);
            if (overwrite == 0 || overwrite == 2)
                continue;
        }
        preset->config.save_to_json(file, preset->name, "", preset->version.to_string());
        result.push_back(file);
    }
    return result;
}

// Set the filament preset name. As the name could come from the UI selection box,
// an optional "(modified)" suffix will be removed from the filament name.
void PresetBundle::set_filament_preset(size_t idx, const std::string &name)
{
    if (idx >= filament_presets.size()) {
        BOOST_LOG_TRIVIAL(warning) << boost::format("Warning: set_filament_preset out of range %1% - %2%") % idx % filament_presets.size();
        return;
    }
    filament_presets[idx] = Preset::remove_suffix_modified(name);
}

void PresetBundle::set_default_suppressed(bool default_suppressed)
{
    prints.set_default_suppressed(default_suppressed);
    filaments.set_default_suppressed(default_suppressed);
    sla_prints.set_default_suppressed(default_suppressed);
    sla_materials.set_default_suppressed(default_suppressed);
    printers.set_default_suppressed(default_suppressed);
}

bool PresetBundle::has_errors(bool check_duplicate_filament_subtypes) const
{
    if (m_errors != 0 || printers.m_errors != 0 || filaments.m_errors != 0 || prints.m_errors != 0)
        return true;

    bool has_errors = false;
    // Orca: check if all filament presets have compatible_printers setting
    for (auto& preset : filaments) {
        if (!preset.is_system)
            continue;
        // It's per design that the Orca Filament Library can have the empty compatible_printers.
        if(preset.vendor->name == PresetBundle::ORCA_FILAMENT_LIBRARY)
            continue;
        auto* compatible_printers = dynamic_cast<const ConfigOptionStrings*>(preset.config.option("compatible_printers"));
        if (compatible_printers == nullptr || compatible_printers->values.empty()) {
            has_errors = true;
            BOOST_LOG_TRIVIAL(error) << "Filament preset \"" << preset.file << "\" is missing compatible_printers setting";
        }
    }

    if (check_duplicate_filament_subtypes && this->check_duplicate_filament_subtypes())
        has_errors = true;

    if (this->check_preset_references())
        has_errors = true;

    return has_errors;
}

// Orca: turn a preset file path into an absolute file:// URI for log messages.
// Printed unquoted on its own line, this is the one format clickable in both the
// VS Code integrated terminal (Cmd/Ctrl+click) and macOS Terminal.app
// (Cmd+double-click); quotes or literal spaces break link detection in both, so
// the characters that would terminate the URI token are percent-encoded.
static std::string preset_file_uri(const std::string &file)
{
    std::string path;
    try {
        path = boost::filesystem::canonical(file).generic_string();
    } catch (...) {
        path = file;
    }
    std::string uri = "file://";
    if (path.empty() || path.front() != '/')
        uri += '/'; // Windows drive paths (e.g. C:/...) need the extra leading slash
    for (char c : path) {
        switch (c) {
        case ' ': uri += "%20"; break;
        case '#': uri += "%23"; break;
        case '%': uri += "%25"; break;
        default:  uri += c;
        }
    }
    return uri;
}

// Orca: validator-only. Flag any system preset whose inherits / compatible_printers /
// compatible_prints references a name that no longer resolves. Uses find_preset (exact match, then
// the renamed_from map - no fuzzy find_preset2, no alias resolution), so:
//   nullptr           -> the referenced preset was deleted/renamed away (name is dangling),
//   resolved != name  -> the reference uses an old name that renamed_from maps to a current one.
// Both should be fixed at the source rather than relying on load-time normalization - which is why
// normalize_compatible_presets() is skipped in validation mode (see load_presets), so this sees the
// raw vendor-JSON references. Safe under a single-vendor run (-v) too: inherits / compatible_printers
// / compatible_prints only name same-vendor or OrcaFilamentLibrary presets (both loaded), so a
// reference that does not resolve is genuinely dangling rather than an unloaded cross-vendor preset.
bool PresetBundle::check_preset_references() const
{
    bool found = false;

    // Resolve one reference (an inherits parent or a compatible_* entry) against its target
    // collection and log if it is dangling (unknown) or uses a renamed preset's old name.
    auto report_ref = [&](const Preset &p, const std::string &name, const PresetCollection &target,
                          const char *verb, const char *noun) {
        const Preset *resolved = target.find_preset(name, false);
        if (resolved == nullptr) {
            found = true;
            BOOST_LOG_TRIVIAL(error) << "Preset \"" << p.name << "\" " << verb << " unknown " << noun << " \"" << name << "\":\n"
                                     << preset_file_uri(p.file);
        } else if (resolved->name != name) {
            found = true;
            BOOST_LOG_TRIVIAL(error) << "Preset \"" << p.name << "\" " << verb << " renamed " << noun << " \"" << name
                                     << "\" (now \"" << resolved->name << "\"):\n" << preset_file_uri(p.file);
        }
    };

    auto check_list = [&](const Preset &p, const char *key, const PresetCollection &target) {
        const auto *opt = p.config.option<ConfigOptionStrings>(key);
        if (opt == nullptr)
            return;
        for (const std::string &name : opt->values)
            if (!name.empty())
                report_ref(p, name, target, "references", key);
    };

    auto check_collection = [&](const PresetCollection &holders, const PresetCollection *processes) {
        for (const Preset &p : holders) {
            if (!p.is_system)
                continue;
            if (const std::string &inh = p.inherits(); !inh.empty())
                report_ref(p, inh, holders, "inherits", "parent");
            check_list(p, "compatible_printers", this->printers);
            if (processes != nullptr)
                check_list(p, "compatible_prints", *processes);
        }
    };

    // Printers carry no compatible_printers/compatible_prints (those name a printer, so a printer
    // holding them makes no sense); check_list is a no-op for them, so only their inherits is checked.
    check_collection(this->printers,      nullptr);
    check_collection(this->prints,        nullptr);
    check_collection(this->filaments,     &this->prints);
    check_collection(this->sla_prints,    nullptr);
    check_collection(this->sla_materials, &this->sla_prints);

    return found;
}

// Orca: a filament is matched from the AMS by (filament_id + printer compatibility).
// For any one printer, at most one instantiated filament preset with a given
// filament_id may be compatible - otherwise the AMS match is ambiguous and the
// runtime silently picks whichever loads first. This validator-only check flags
// any printer that has two or more compatible filament presets sharing a filament_id.
bool PresetBundle::check_duplicate_filament_subtypes() const
{
    // Pre-collect system filament presets (each carries its effective filament_id,
    // inherited from its @base at load time), grouped by vendor so we only test a
    // printer against its own vendor's filaments. A vendor's compatible_printers
    // only names that vendor's printers, so same-vendor scoping is correctness
    // preserving and avoids an O(all printers x all filaments) sweep.
    std::map<std::string, std::vector<const Preset *>> filaments_by_vendor;
    for (const auto &preset : filaments) {
        if (!preset.is_system || preset.filament_id.empty() || preset.vendor == nullptr)
            continue;
        filaments_by_vendor[preset.vendor->name].push_back(&preset);
    }

    bool found_duplicates = false;
    for (const auto &printer : printers) {
        if (!printer.is_system || printer.vendor == nullptr)
            continue;
        auto vendor_it = filaments_by_vendor.find(printer.vendor->name);
        if (vendor_it == filaments_by_vendor.end())
            continue;

        const PresetWithVendorProfile active_printer = printers.get_preset_with_vendor_profile(printer);
        // std::map keeps the reported errors in a deterministic (sorted) order.
        std::map<std::string, std::vector<const Preset *>> by_filament_id;
        for (const Preset *fil : vendor_it->second)
            if (is_compatible_with_printer(filaments.get_preset_with_vendor_profile(*fil), active_printer))
                by_filament_id[fil->filament_id].push_back(fil);

        for (const auto &entry : by_filament_id) {
            if (entry.second.size() < 2)
                continue;
            found_duplicates = true;
            // List each conflicting preset with a clickable file:// URI on its own
            // line, so the profile author can jump straight to the files to fix.
            std::string presets;
            for (const Preset *p : entry.second)
                presets += "\n    - " + p->name + "\n      " + preset_file_uri(p->file);
            BOOST_LOG_TRIVIAL(error)
                << "Ambiguous AMS filament match: " << entry.second.size()
                << " filament presets share filament_id \"" << entry.first
                << "\" and are all compatible with printer \"" << printer.name
                << "\". When matching an AMS spool the slicer cannot tell them apart and"
                   " silently picks whichever loads first." << presets;
        }
    }

    // Print the troubleshooting guidance once, not per error, to keep the log readable.
    if (found_duplicates)
        BOOST_LOG_TRIVIAL(error) << "\n========================================\n"
            << "How to fix \"Ambiguous AMS filament match\" errors: make sure only ONE filament"
               " preset with a given filament_id is compatible with each printer. Either"
               "\n    (a) remove the overlapping printer from a preset's \"compatible_printers\""
               " list (e.g. a '@printer' preset over-claiming a nozzle that already has its own"
               " '@printer 0.x nozzle' preset), or"
               "\n    (b) if these are genuinely different materials, give each its own"
               " \"filament_id\" - a common cause is a wrong \"inherits\" pointing at another"
               " material's @base preset.";

    return found_duplicates;
}

// Orca: BundleMetadata method implementations
bool BundleMetadata::load_from_json(const std::string& path)
{
    try {
        boost::nowide::ifstream ifs(path);
        if (!ifs.good())
            return false;

        json j;
        ifs >> j;

        if (j.contains("id")) this->id = j["id"].get<std::string>();

        if (j.contains("name")) this->name = j["name"].get<std::string>();
        else if (j.contains("bundle_id")) this->name = j["bundle_id"].get<std::string>();                 // backwards compat w bundle_structure.json

        if (j.contains("version")) this->version = j["version"].get<std::string>();

        if (j.contains("description")) this->description = j["description"].get<std::string>();
        else if (j.contains("bundle_type")) this->description = j["bundle_type"].get<std::string>();    // backwards compat w bundle_structure.json

        if (j.contains("author")) this->author = j["author"].get<std::string>();

        if (j.contains("imported_time")) this->imported_time = j["imported_time"].get<long long>();

        if (j.contains("updated_time")) this->updated_time = j["updated_time"].get<long long>();

        if (j.contains("print_presets"))                                                                                                                                                                                                                                      
            this->print_presets = j["print_presets"].get<std::vector<std::string>>();                                                                                                                                                                                         
                                                                                                                                                                                                                                                                                
        if (j.contains("filament_presets"))                                                                                                                                                                                                                                   
            this->filament_presets = j["filament_presets"].get<std::vector<std::string>>();                                                                                                                                                                                   
                                                                                                                                                                                                                                                                                
        if (j.contains("printer_presets"))                                                                                                                                                                                                                                    
            this->printer_presets = j["printer_presets"].get<std::vector<std::string>>();  

        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to load bundle metadata from " << path << ": " << e.what();
        return false;
    }
}

bool BundleMetadata::save_to_json(const std::string& path) const
{
    auto strip_prefix = [](const std::vector<std::string>& names) {
                json arr = json::array();
                for (const auto& name : names)
                {
                    arr.push_back(boost::filesystem::path(name).filename().string());
                    std::string test = boost::filesystem::path(name).filename().string();
                }
                return arr;
            };
    try {
        json j;
        j["id"] = this->id;
        j["name"] = this->name;
        j["version"] = this->version;
        j["description"] = this->description;
        j["author"] = this->author;
        j["imported_time"] = this->imported_time;
        j["updated_time"] = this->updated_time;

        j["print_presets"] = strip_prefix(this->print_presets);                                                                                                                                                                                                                             
        j["filament_presets"] = strip_prefix(this->filament_presets);                                                                                                                                                                                                                       
        j["printer_presets"] = strip_prefix(this->printer_presets);

        boost::nowide::ofstream ofs(path);
        ofs << j.dump(4);
        return ofs.good();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Failed to save bundle metadata to " << path << ": " << e.what();
        return false;
    }
}
} // namespace Slic3r

