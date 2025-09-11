#ifndef BADLINK_SRC_CONFIG_H_
#define BADLINK_SRC_CONFIG_H_

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "toml.hpp"
#include "network_capture.h"
#include "imgui.h"

namespace BadLink {
    namespace Config {

        // Configuration file name
        constexpr const char* CONFIG_FILE = "badlink.toml";

        // Filter preset structure
        struct FilterPreset {
            std::string name;
            std::string filter;
        };

        // Hotkey configuration
        struct HotkeyConfig {
            bool enabled = false;
            ImGuiKey key = ImGuiKey_F9;  // Default to F9
            bool ctrl = false;
            bool shift = false;
            bool alt = false;

            // Convert to string for display, needs debugging, can cause crashes for unknown reasons
            std::string ToString() const {
                if (key == ImGuiKey_None) return "None";

                std::string result;
                if (ctrl) result += "Ctrl+";
                if (shift) result += "Shift+";
                if (alt) result += "Alt+";
                result += ImGui::GetKeyName(key);
                return result;
            }
        };

        // Extended capture parameters with presets and hotkey
        struct Configuration {
            CaptureParameters params;
            std::vector<FilterPreset> filter_presets;
            HotkeyConfig capture_hotkey;
        };

        // Default filter presets
        inline std::vector<FilterPreset> GetDefaultPresets() {
            return {
                {"All traffic", "true"},
                {"TCP only", "tcp"},
                {"UDP only", "udp"},
                {"HTTP (port 80)", "tcp.DstPort == 80 or tcp.SrcPort == 80"},
                {"HTTPS (port 443)", "tcp.DstPort == 443 or tcp.SrcPort == 443"},
                {"DNS (port 53)", "udp.DstPort == 53 or udp.SrcPort == 53"},
                {"Local network", "ip.DstAddr >= 192.168.0.0 and ip.DstAddr <= 192.168.255.255"},
                {"IPv6 only", "ipv6"},
                {"IPv4 only", "ip"},
                {"Outbound only", "outbound"},
                {"Inbound only", "inbound"},
                {"Non-loopback", "!loopback"},
                {"No traffic (test)", "false"}
            };
        }

        // Load configuration from TOML file
        inline bool Load(Configuration& config) {
            if (!std::filesystem::exists(CONFIG_FILE)) {
                return false;
            }

            try {
                auto toml_config = toml::parse_file(CONFIG_FILE);

                // WinDivert parameters
                if (auto section = toml_config["WinDivert"].as_table()) {
                    if (auto val = section->get("QueueLength")->value<int64_t>())
                        config.params.queue_length = static_cast<UINT64>(*val);
                    if (auto val = section->get("QueueTime")->value<int64_t>())
                        config.params.queue_time = static_cast<UINT64>(*val);
                    if (auto val = section->get("QueueSize")->value<int64_t>())
                        config.params.queue_size = static_cast<UINT64>(*val);
                }

                // Performance parameters
                if (auto section = toml_config["Performance"].as_table()) {
                    if (auto val = section->get("BatchSize")->value<int64_t>())
                        config.params.batch_size = static_cast<UINT32>(*val);
                    if (auto val = section->get("WorkerThreads")->value<int64_t>())
                        config.params.worker_threads = static_cast<UINT32>(*val);
                    if (auto val = section->get("PacketBufferSize")->value<int64_t>())
                        config.params.packet_buffer_size = static_cast<UINT32>(*val);
                    if (auto val = section->get("VisualPacketBuffer")->value<int64_t>())
                        config.params.visual_packet_buffer = static_cast<size_t>(*val);
                    if (auto val = section->get("RingPacketBuffer")->value<int64_t>())
                        config.params.ring_packet_buffer = static_cast<size_t>(*val);
                }

                // Network parameters
                if (auto section = toml_config["Network"].as_table()) {
                    if (auto val = section->get("MTUSize")->value<int64_t>())
                        config.params.mtu_size = static_cast<UINT32>(*val);
                    if (auto val = section->get("MaxPacketSize")->value<int64_t>())
                        config.params.max_packet_size = static_cast<UINT32>(*val);
                }

                // Hotkey configuration
                if (auto section = toml_config["Hotkey"].as_table()) {
                    if (auto val = section->get("Enabled")->value<bool>())
                        config.capture_hotkey.enabled = *val;
                    if (auto val = section->get("Key")->value<int64_t>())
                        config.capture_hotkey.key = static_cast<ImGuiKey>(*val);
                    if (auto val = section->get("Ctrl")->value<bool>())
                        config.capture_hotkey.ctrl = *val;
                    if (auto val = section->get("Shift")->value<bool>())
                        config.capture_hotkey.shift = *val;
                    if (auto val = section->get("Alt")->value<bool>())
                        config.capture_hotkey.alt = *val;
                }

                // Filter presets
                config.filter_presets.clear();
                if (auto presets_array = toml_config["FilterPresets"].as_array()) {
                    for (const auto& preset_node : *presets_array) {
                        if (auto preset_table = preset_node.as_table()) {
                            FilterPreset preset;
                            if (auto name = preset_table->get("name")->value<std::string>())
                                preset.name = *name;
                            if (auto filter = preset_table->get("filter")->value<std::string>())
                                preset.filter = *filter;

                            if (!preset.name.empty() && !preset.filter.empty()) {
                                config.filter_presets.push_back(preset);
                            }
                        }
                    }
                }

                // Use defaults if no presets were loaded
                if (config.filter_presets.empty()) {
                    config.filter_presets = GetDefaultPresets();
                }

                return true;
            }
            catch (...) {
                return false;
            }
        }

        // Save configuration to TOML file
        inline bool Save(const Configuration& config) {
            try {
                toml::table toml_config;

                // WinDivert section
                toml_config.insert("WinDivert", toml::table{
                    {"QueueLength", static_cast<int64_t>(config.params.queue_length)},
                    {"QueueTime", static_cast<int64_t>(config.params.queue_time)},
                    {"QueueSize", static_cast<int64_t>(config.params.queue_size)}
                    });

                // Performance section
                toml_config.insert("Performance", toml::table{
                    {"BatchSize", static_cast<int64_t>(config.params.batch_size)},
                    {"WorkerThreads", static_cast<int64_t>(config.params.worker_threads)},
                    {"PacketBufferSize", static_cast<int64_t>(config.params.packet_buffer_size)},
                    {"VisualPacketBuffer", static_cast<int64_t>(config.params.visual_packet_buffer)},
                    {"RingPacketBuffer", static_cast<int64_t>(config.params.ring_packet_buffer)}
                    });

                // Network section
                toml_config.insert("Network", toml::table{
                    {"MTUSize", static_cast<int64_t>(config.params.mtu_size)},
                    {"MaxPacketSize", static_cast<int64_t>(config.params.max_packet_size)}
                    });

                // Hotkey section
                toml_config.insert("Hotkey", toml::table{
                    {"Enabled", config.capture_hotkey.enabled},
                    {"Key", static_cast<int64_t>(config.capture_hotkey.key)},
                    {"Ctrl", config.capture_hotkey.ctrl},
                    {"Shift", config.capture_hotkey.shift},
                    {"Alt", config.capture_hotkey.alt}
                    });

                // Filter presets array
                toml::array presets_array;
                for (const auto& preset : config.filter_presets) {
                    toml::table preset_table;
                    preset_table.insert("name", preset.name);
                    preset_table.insert("filter", preset.filter);
                    presets_array.push_back(preset_table);
                }
                toml_config.insert("FilterPresets", presets_array);

                // Write to file
                std::ofstream file(CONFIG_FILE);
                if (!file.is_open()) {
                    return false;
                }

                file << "# BadLink Configuration File\n";
                file << "# Auto-generated: modifications will be preserved\n";
                file << "#\n";
                file << "# You can add custom filter presets in the FilterPresets section below\n";
                file << "# Example:\n";
                file << "# [[FilterPresets]]\n";
                file << "# name = \"My Custom Filter\"\n";
                file << "# filter = \"tcp.DstPort == 8080\"\n\n";
                file << toml_config;

                return true;
            }
            catch (...) {
                return false;
            }
        }

        // Create default configuration file
        inline void CreateDefault() {
            Configuration config;
            config.filter_presets = GetDefaultPresets();
            Save(config);
        }

        // Backward compatibility overloads
        inline bool Load(CaptureParameters& params) {
            Configuration config;
            if (Load(config)) {
                params = config.params;
                return true;
            }
            return false;
        }

        inline bool Save(const CaptureParameters& params) {
            Configuration config;
            config.params = params;
            config.filter_presets = GetDefaultPresets();
            return Save(config);
        }

    } 
} 
#endif  // BADLINK_SRC_CONFIG_H_