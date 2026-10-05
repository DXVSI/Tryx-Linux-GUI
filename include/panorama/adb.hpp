#pragma once

#include <optional>
#include <string>
#include <vector>

namespace panorama {

class Adb {
 public:
  static constexpr const char* MEDIA_PATH = "/sdcard/pcMedia/";

  static bool is_device_connected();
  static bool push(const std::string& local_path,
                   const std::string& remote_name);
  static std::optional<std::vector<std::string>> list_media();
  static bool file_exists(const std::string& filename);
  static bool remove(const std::string& filename);

 private:
  static std::string find_tryx_serial();
  static std::string escape_shell_arg(const std::string& input);
  static std::optional<std::string> run_command(
      const std::vector<std::string>& args);
};

}  // namespace panorama
