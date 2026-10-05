#pragma once

#include <optional>
#include <string>
#include <vector>

#include "protocol.hpp"

namespace panorama {

struct DeviceInfo {
  std::string product_id;
  std::string os;
  std::string serial;
  std::string app_version;
  std::string firmware;
  std::string hardware;
  std::vector<std::string> attributes;
};

struct DisplaySettings {
  std::string position = "Top";    // "Top", "Center", "Bottom"
  std::string color = "#FFFFFF";   // hex color
  std::string align = "Left";     // "Left", "Center", "Right"
  std::vector<std::string> badges; // e.g. "CPU Badge", "GPU Badge"
  int filter_opacity = 0;          // 0-100
};

struct ScreenConfig {
  std::string preset_id;  // e.g. "Pre-set 1: Cooling delivery" (empty = custom mode)
  std::vector<std::string> media;
  std::string screen_mode = "Full Screen";
  std::string ratio = "2:1";
  std::string play_mode = "Single";
  std::vector<std::string> sysinfo_display; // max 3 labels
  DisplaySettings settings;

  // For Screen Splitting mode
  DisplaySettings settings2;
  std::vector<std::string> sysinfo_display2;
  bool waterfall_mode = false;
};

struct SysinfoData {
  std::string label;  // e.g. "CPU Temperature"
  std::string value;  // e.g. "65"
  std::string unit;   // e.g. "°C"
};

class Device {
 public:
  explicit Device(const std::string& port, bool verbose = false);
  ~Device();

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  // Auto-detect device by scanning /dev/ttyACM* and attempting handshake
  static std::optional<std::string> find_device(bool verbose = false);

  bool connect();
  void disconnect();
  bool is_connected() const { return fd_ >= 0; }
  const std::string& port() const { return port_; }

  std::optional<Response> send_command(const std::string& request_state,
                                       const std::string& cmd_type,
                                       const std::string& content = "",
                                       bool wait_response = true);

  std::optional<DeviceInfo> handshake();
  std::optional<Response> set_screen_config(const ScreenConfig& config);
  std::optional<Response> set_sysinfo_display(const ScreenConfig& config);
  std::optional<Response> set_temperature_unit(const std::string& unit = "Celsius");
  std::optional<Response> send_config(const std::string& cpu_name, const std::string& gpu_name, const std::string& temp_unit = "Celsius");
  std::optional<Response> send_full_config(const ScreenConfig& config, const std::string& cpu_name, const std::string& gpu_name, int brightness = 75, const std::string& temp_unit = "Celsius");
  std::optional<Response> set_brightness(int value);
  std::optional<Response> set_rotation(int degrees);
  std::optional<Response> reboot();
  std::optional<Response> set_waterfall_mode(bool enable);
  std::optional<Response> delete_media(const std::vector<std::string>& files);
  std::optional<Response> send_sysinfo(const std::vector<SysinfoData>& data);

 private:
  std::string port_;
  bool verbose_;
  int fd_ = -1;
  int seq_number_ = 0;

  std::vector<uint8_t> read_response(int timeout_ms = 1000);
};

}  // namespace panorama
