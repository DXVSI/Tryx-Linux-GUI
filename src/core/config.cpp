#include "panorama/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "panorama/picojson.h"

namespace panorama {

namespace {

constexpr const char* kAppName = "tryx-panorama";
constexpr const char* kConfigFilename = "config.json";
constexpr const char* kStateFilename = "display.json";

std::string extract_text(const picojson::object& node, const std::string& field,
                         const std::string& fallback) {
  auto pos = node.find(field);
  if (pos != node.end() && pos->second.is<std::string>()) {
    return pos->second.get<std::string>();
  }
  return fallback;
}

int extract_number(const picojson::object& node, const std::string& field,
                   int fallback) {
  auto pos = node.find(field);
  if (pos != node.end() && pos->second.is<double>()) {
    return static_cast<int>(pos->second.get<double>());
  }
  return fallback;
}

picojson::value to_json_number(int n) {
  return picojson::value(static_cast<double>(n));
}

picojson::value to_json_text(const std::string& s) {
  return picojson::value(s);
}

bool valid_pase_overlay_lease_mode(const std::string& mode) {
  return mode == "ping-and-overlay-lease" || mode == "ping-only";
}

bool parse_root_object(const std::string& raw, picojson::object& out) {
  picojson::value parsed;
  std::string parse_err = picojson::parse(parsed, raw);
  if (!parse_err.empty() || !parsed.is<picojson::object>()) {
    return false;
  }
  out = parsed.get<picojson::object>();
  return true;
}

}  // namespace

std::string ConfigManager::resolve_xdg_path(const char* env_var,
                                             const char* fallback_suffix) {
  const char* xdg_val = std::getenv(env_var);
  if (xdg_val != nullptr && xdg_val[0] != '\0') {
    return std::string(xdg_val) + "/" + kAppName;
  }
  const char* home_dir = std::getenv("HOME");
  if (home_dir != nullptr && home_dir[0] != '\0') {
    return std::string(home_dir) + "/" + fallback_suffix + "/" + kAppName;
  }
  return std::string(fallback_suffix) + "/" + kAppName;
}

std::string ConfigManager::read_file_contents(const std::string& filepath) {
  std::ifstream input(filepath);
  if (!input.is_open()) {
    return {};
  }
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

bool ConfigManager::write_json_file(const std::string& filepath,
                                    const std::string& json_text) {
  std::ofstream output(filepath);
  if (!output.is_open()) {
    return false;
  }
  output << json_text << "\n";
  return output.good();
}

std::string ConfigManager::get_config_dir() {
  return resolve_xdg_path("XDG_CONFIG_HOME", ".config");
}

std::string ConfigManager::get_state_dir() {
  return resolve_xdg_path("XDG_STATE_HOME", ".local/state");
}

std::string ConfigManager::get_config_path() {
  return get_config_dir() + "/" + kConfigFilename;
}

std::string ConfigManager::get_state_path() {
  return get_state_dir() + "/" + kStateFilename;
}

std::optional<Config> ConfigManager::load_config() {
  auto target = get_config_path();

  std::error_code path_error;
  const bool config_exists =
      std::filesystem::exists(target, path_error);
  if (path_error) {
    return std::nullopt;
  }
  if (!config_exists) {
    return Config{};
  }

  std::string raw = read_file_contents(target);
  if (raw.empty()) {
    return std::nullopt;
  }

  picojson::object root;
  if (!parse_root_object(raw, root)) {
    return std::nullopt;
  }

  Config cfg;
  cfg.port = extract_text(root, "port", cfg.port);
  cfg.brightness = extract_number(root, "brightness", cfg.brightness);
  cfg.keepalive_interval =
      extract_number(root, "keepalive_interval", cfg.keepalive_interval);
  cfg.language = extract_text(root, "language", cfg.language);
  const auto overlay_mode = root.find("pase_overlay_lease_mode");
  if (overlay_mode != root.end()) {
    if (!overlay_mode->second.is<std::string>()) {
      return std::nullopt;
    }
    cfg.pase_overlay_lease_mode =
        overlay_mode->second.get<std::string>();
  }
  if (!valid_pase_overlay_lease_mode(cfg.pase_overlay_lease_mode)) {
    return std::nullopt;
  }

  return cfg;
}

bool ConfigManager::save_config(const Config& config) {
  if (!valid_pase_overlay_lease_mode(config.pase_overlay_lease_mode)) {
    return false;
  }
  auto dir_path = get_config_dir();
  std::filesystem::create_directories(dir_path);

  picojson::object root;
  root["port"] = to_json_text(config.port);
  root["brightness"] = to_json_number(config.brightness);
  root["keepalive_interval"] = to_json_number(config.keepalive_interval);
  root["language"] = to_json_text(config.language);
  root["pase_overlay_lease_mode"] =
      to_json_text(config.pase_overlay_lease_mode);

  std::string serialized = picojson::value(root).serialize();
  return write_json_file(get_config_path(), serialized);
}

std::optional<DisplayState> ConfigManager::load_state() {
  auto target = get_state_path();

  if (!std::filesystem::exists(target)) {
    return std::nullopt;
  }

  std::string raw = read_file_contents(target);
  if (raw.empty()) {
    return std::nullopt;
  }

  picojson::object root;
  if (!parse_root_object(raw, root)) {
    return std::nullopt;
  }

  DisplayState ds;

  auto media_it = root.find("media");
  if (media_it != root.end() && media_it->second.is<picojson::array>()) {
    const auto& items = media_it->second.get<picojson::array>();
    ds.media.reserve(items.size());
    for (const auto& entry : items) {
      if (entry.is<std::string>()) {
        ds.media.push_back(entry.get<std::string>());
      }
    }
  }

  ds.ratio = extract_text(root, "ratio", ds.ratio);
  ds.screen_mode = extract_text(root, "screen_mode", ds.screen_mode);
  ds.play_mode = extract_text(root, "play_mode", ds.play_mode);
  ds.brightness = extract_number(root, "brightness", ds.brightness);

  return ds;
}

bool ConfigManager::save_state(const DisplayState& state) {
  auto dir_path = get_state_dir();
  std::filesystem::create_directories(dir_path);

  picojson::array media_entries;
  media_entries.reserve(state.media.size());
  for (const auto& item : state.media) {
    media_entries.push_back(to_json_text(item));
  }

  picojson::object root;
  root["media"] = picojson::value(media_entries);
  root["ratio"] = to_json_text(state.ratio);
  root["screen_mode"] = to_json_text(state.screen_mode);
  root["play_mode"] = to_json_text(state.play_mode);
  root["brightness"] = to_json_number(state.brightness);

  std::string serialized = picojson::value(root).serialize();
  return write_json_file(get_state_path(), serialized);
}

}  // namespace panorama
