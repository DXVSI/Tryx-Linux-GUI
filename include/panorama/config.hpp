#pragma once

#include <optional>
#include <string>
#include <vector>

namespace panorama {

struct Config {
  std::string port;  // Empty = auto-detect
  int brightness = 75;  //default lower than max setting to reduce burn-in risk on display
  int keepalive_interval = 10;
  std::string language = "en";
  std::string pase_overlay_lease_mode = "ping-and-overlay-lease";
};

struct DisplayState {
  std::vector<std::string> media;
  std::string ratio = "2:1";
  std::string screen_mode = "Full Screen";
  std::string play_mode = "Single";
  int brightness = 75;  // default lower than max setting to reduce burn-in risk on the display
};

class ConfigManager {
 public:
  static std::string get_config_dir();
  static std::string get_state_dir();
  static std::string get_config_path();
  static std::string get_state_path();

  static std::optional<Config> load_config();
  static bool save_config(const Config& config);

  static std::optional<DisplayState> load_state();
  static bool save_state(const DisplayState& state);

 private:
  static std::string resolve_xdg_path(const char* env_var,
                                       const char* fallback_suffix);
  static std::string read_file_contents(const std::string& filepath);
  static bool write_json_file(const std::string& filepath,
                              const std::string& json_text);
};

}  // namespace panorama
