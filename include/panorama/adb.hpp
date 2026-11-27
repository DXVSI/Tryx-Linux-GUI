#pragma once

#include <optional>
#include <string>
#include <vector>

namespace panorama {

class Adb {
 public:
  static constexpr const char* MEDIA_PATH = "/sdcard/pcMedia/";

  enum class Status {
    Ready,          // a device row in the "device" state
    NotInstalled,   // the adb program could not be started
    NoDevice,       // adb lists no device
    NoPermissions,  // adb cannot open the device's USB node
    NotReady,       // another state, such as unauthorized or offline
  };

  struct DeviceStatus {
    Status status = Status::NoDevice;
    // The state adb printed for Status::NotReady and Status::NoPermissions.
    std::string state;
  };

  // Runs `adb devices` and classifies its output.
  static DeviceStatus device_status();
  // Classifies the output of `adb devices`; a row in the "device" state wins.
  static DeviceStatus parse_device_list(const std::string& output);

  static bool is_device_connected();
  static bool push(const std::string& local_path,
                   const std::string& remote_name);
  static std::optional<std::vector<std::string>> list_media();
  static bool file_exists(const std::string& filename);
  static bool remove(const std::string& filename);
  static bool reboot();

  // A name directly inside MEDIA_PATH: not empty, not "." or "..", and
  // without '/', NUL or line feed.
  static bool is_plain_media_name(const std::string& name);

 private:
  static std::string find_tryx_serial();
  static std::optional<std::string> run_command(
      const std::vector<std::string>& args);
  static std::optional<std::string> run_device_shell(
      const std::vector<std::string>& remote_args);
};

}  // namespace panorama
