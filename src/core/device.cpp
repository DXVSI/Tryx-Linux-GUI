#include "panorama/device.hpp"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <thread>

namespace panorama {

namespace {

std::string get_string(const picojson::value& v, const std::string& key,
                       const std::string& def = "") {
  if (!v.is<picojson::object>()) return def;
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end() || !it->second.is<std::string>()) return def;
  return it->second.get<std::string>();
}

bool has_key(const picojson::value& v, const std::string& key) {
  if (!v.is<picojson::object>()) return false;
  return v.get<picojson::object>().count(key) > 0;
}

const picojson::value& get_value(const picojson::value& v,
                                 const std::string& key) {
  static picojson::value null_val;
  if (!v.is<picojson::object>()) return null_val;
  const auto& obj = v.get<picojson::object>();
  auto it = obj.find(key);
  if (it == obj.end()) return null_val;
  return it->second;
}

}  // namespace

std::optional<std::string> Device::find_device(bool verbose) {
  namespace fs = std::filesystem;

  std::vector<std::string> candidates;

  // Scan /dev for ttyACM* devices
  for (const auto& entry : fs::directory_iterator("/dev")) {
    std::string name = entry.path().filename().string();
    if (name.rfind("ttyACM", 0) == 0) {
      candidates.push_back(entry.path().string());
    }
  }

  if (candidates.empty()) {
    if (verbose) {
      std::cerr << "No /dev/ttyACM* devices found\n";
    }
    return std::nullopt;
  }

  // Sort for consistent ordering (ttyACM0, ttyACM1, ...)
  std::sort(candidates.begin(), candidates.end());

  if (verbose) {
    std::cout << "Scanning " << candidates.size() << " device(s)...\n";
  }

  // Try each device
  for (const auto& port : candidates) {
    if (verbose) {
      std::cout << "  Trying " << port << "... ";
    }

    Device dev(port, false);
    if (!dev.connect()) {
      if (verbose) {
        std::cout << "failed to open\n";
      }
      continue;
    }

    auto info = dev.handshake();
    if (info && !info->product_id.empty() && info->product_id != "unknown") {
      if (verbose) {
        std::cout << "found " << info->product_id << "\n";
      }
      return port;
    }

    if (verbose) {
      std::cout << "no response\n";
    }
  }

  return std::nullopt;
}

Device::Device(const std::string& port, bool verbose)
    : port_(port), verbose_(verbose) {}

Device::~Device() {
  disconnect();
}

bool Device::connect() {
  fd_ = open(port_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd_ < 0) {
    if (verbose_) {
      std::cerr << "Failed to open " << port_ << ": " << strerror(errno)
                << "\n";
    }
    return false;
  }

  struct termios tty;
  memset(&tty, 0, sizeof(tty));

  if (tcgetattr(fd_, &tty) != 0) {
    if (verbose_) {
      std::cerr << "tcgetattr failed: " << strerror(errno) << "\n";
    }
    close(fd_);
    fd_ = -1;
    return false;
  }

  // 115200 baud
  cfsetospeed(&tty, B115200);
  cfsetispeed(&tty, B115200);

  // 8N1, no flow control
  tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
  tty.c_cflag &= ~(PARENB | PARODD | CSTOPB);
  tty.c_cflag &= ~CRTSCTS;
  tty.c_cflag |= CLOCAL | CREAD;

  // Raw mode
  tty.c_iflag &=
      ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  tty.c_oflag &= ~OPOST;
  tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

  // Non-blocking reads
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 0;

  if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
    if (verbose_) {
      std::cerr << "tcsetattr failed: " << strerror(errno) << "\n";
    }
    close(fd_);
    fd_ = -1;
    return false;
  }

  tcflush(fd_, TCIOFLUSH);

  if (verbose_) {
    std::cout << "Connected to " << port_ << "\n";
  }

  return true;
}

void Device::disconnect() {
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
}

std::vector<uint8_t> Device::read_response(int timeout_ms) {
  std::vector<uint8_t> response;

  struct pollfd pfd;
  pfd.fd = fd_;
  pfd.events = POLLIN;

  auto start = std::chrono::steady_clock::now();

  while (true) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();

    if (elapsed >= timeout_ms) {
      break;
    }

    int remaining = timeout_ms - static_cast<int>(elapsed);
    int ret = poll(&pfd, 1, remaining);

    if (ret > 0 && (pfd.revents & POLLIN)) {
      uint8_t buf[256];
      ssize_t n = read(fd_, buf, sizeof(buf));
      if (n > 0) {
        response.insert(response.end(), buf, buf + n);

        // Check if we have a complete frame
        if (response.size() >= 2 && response.front() == FRAME_MARKER &&
            response.back() == FRAME_MARKER) {
          break;
        }
      }
    } else if (ret < 0 && errno != EINTR) {
      break;
    }
  }

  return response;
}

std::optional<Response> Device::send_command(const std::string& request_state,
                                             const std::string& cmd_type,
                                             const std::string& content,
                                             bool wait_response) {
  if (fd_ < 0) {
    return std::nullopt;
  }

  ++seq_number_;
  auto frame = build_frame(request_state, cmd_type, content, "1", seq_number_);

  if (verbose_) {
    std::cout << "Sending: " << cmd_type << "\n";
    std::cout << "Frame hex: ";
    for (uint8_t b : frame) {
      std::cout << std::hex << std::uppercase << std::setfill('0')
                << std::setw(2) << static_cast<int>(b);
    }
    std::cout << std::dec << "\n";
  }

  ssize_t written = write(fd_, frame.data(), frame.size());
  if (written != static_cast<ssize_t>(frame.size())) {
    if (verbose_) {
      std::cerr << "Write failed\n";
    }
    return std::nullopt;
  }

  tcdrain(fd_);

  if (!wait_response) {
    return std::nullopt;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  auto response = read_response(500);

  if (response.empty()) {
    if (verbose_) {
      std::cout << "No response received\n";
    }
    return std::nullopt;
  }

  if (verbose_) {
    std::cout << "Response hex: ";
    for (uint8_t b : response) {
      std::cout << std::hex << std::uppercase << std::setfill('0')
                << std::setw(2) << static_cast<int>(b);
    }
    std::cout << std::dec << "\n";
  }

  auto parsed = parse_response(response);
  if (verbose_ && parsed) {
    std::cout << "Parsed: " << parsed->raw << "\n";
  }

  return parsed;
}

std::optional<DeviceInfo> Device::handshake() {
  auto response = send_command("POST", "conn", "");

  if (!response || !response->json) {
    return std::nullopt;
  }

  DeviceInfo info;
  const auto& j = *response->json;

  info.product_id = get_string(j, "productId", "unknown");
  info.os = get_string(j, "OS", "unknown");
  info.serial = get_string(j, "sn", "unknown");

  if (has_key(j, "version")) {
    const auto& v = get_value(j, "version");
    info.app_version = get_string(v, "app", "unknown");
    info.firmware = get_string(v, "firmware", "unknown");
    info.hardware = get_string(v, "hardware", "unknown");
  }

  if (has_key(j, "attribute")) {
    const auto& attr_val = get_value(j, "attribute");
    if (attr_val.is<picojson::array>()) {
      for (const auto& attr : attr_val.get<picojson::array>()) {
        if (attr.is<std::string>()) {
          info.attributes.push_back(attr.get<std::string>());
        }
      }
    }
  }

  return info;
}

std::optional<Response> Device::set_screen_config(const ScreenConfig& config) {
  std::string content;

  // Helper: build a single DisplaySettings into a picojson object
  auto build_settings = [](const DisplaySettings& ds) -> picojson::object {
    picojson::object filter;
    filter["value"] = picojson::value("");
    filter["opacity"] = picojson::value(static_cast<double>(ds.filter_opacity));

    picojson::array badges_arr;
    for (const auto& b : ds.badges) {
      badges_arr.push_back(picojson::value(b));
    }

    picojson::object settings;
    settings["position"] = picojson::value(ds.position);
    settings["color"] = picojson::value(ds.color);
    settings["align"] = picojson::value(ds.align);
    settings["filter"] = picojson::value(filter);
    settings["badges"] = picojson::value(badges_arr);
    return settings;
  };

  // Build media array
  picojson::array media_arr;
  for (const auto& m : config.media) {
    media_arr.push_back(picojson::value(m));
  }

  // Build config object
  picojson::object cfg;
  if (!config.preset_id.empty()) {
    cfg["Type"] = picojson::value("Pre-set");
    cfg["id"] = picojson::value(config.preset_id);
  } else {
    cfg["Type"] = picojson::value("Custom");
    cfg["id"] = picojson::value("Customization");
    cfg["media"] = picojson::value(media_arr);
  }
  cfg["screenMode"] = picojson::value(config.screen_mode);
  cfg["ratio"] = picojson::value(config.ratio);
  cfg["playMode"] = picojson::value(config.play_mode);

  if (config.screen_mode == "Screen Splitting") {
    // Settings as JSON array of 2 objects
    picojson::array settings_arr;
    settings_arr.push_back(picojson::value(build_settings(config.settings)));
    settings_arr.push_back(picojson::value(build_settings(config.settings2)));
    cfg["settings"] = picojson::value(settings_arr);

    // sysinfoDisplay as JSON array of 2 arrays
    picojson::array sysinfo_outer;

    picojson::array sysinfo_left;
    for (const auto& label : config.sysinfo_display) {
      sysinfo_left.push_back(picojson::value(label));
    }
    sysinfo_outer.push_back(picojson::value(sysinfo_left));

    picojson::array sysinfo_right;
    for (const auto& label : config.sysinfo_display2) {
      sysinfo_right.push_back(picojson::value(label));
    }
    sysinfo_outer.push_back(picojson::value(sysinfo_right));

    cfg["sysinfoDisplay"] = picojson::value(sysinfo_outer);
  } else {
    // Full Screen: single settings object, single sysinfo array
    cfg["settings"] = picojson::value(build_settings(config.settings));

    picojson::array sysinfo_arr;
    for (const auto& label : config.sysinfo_display) {
      sysinfo_arr.push_back(picojson::value(label));
    }
    cfg["sysinfoDisplay"] = picojson::value(sysinfo_arr);
  }

  content = picojson::value(cfg).serialize();

  std::cerr << "[screen_config] " << content << "\n";

  // Send twice - device requires this to apply config reliably
  send_command("POST", "waterBlockScreenId", content);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  auto result = send_command("POST", "waterBlockScreenId", content);

  // Send waterfall mode command based on config
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  set_waterfall_mode(config.waterfall_mode);

  return result;
}

std::optional<Response> Device::set_sysinfo_display(const ScreenConfig& config) {
  picojson::object obj;

  // Device expects "items" key, not "sysinfoDisplay"
  picojson::array items_arr;
  for (const auto& label : config.sysinfo_display) {
    items_arr.push_back(picojson::value(label));
  }
  obj["items"] = picojson::value(items_arr);

  std::string content = picojson::value(obj).serialize();
  std::cerr << "[sysinfo_display] " << content << "\n";

  return send_command("POST", "sysinfoDisplay", content, false);
}

std::optional<Response> Device::set_temperature_unit(const std::string& unit) {
  picojson::object obj;
  obj["value"] = picojson::value(unit);  // "Celsius" or "Fahrenheit"
  std::string content = picojson::value(obj).serialize();
  return send_command("POST", "temperature", content);
}

std::optional<Response> Device::send_config(const std::string& cpu_name, const std::string& gpu_name, const std::string& temp_unit) {
  // Send spec via "POST spec"
  {
    picojson::object spec;
    spec["cpu"] = picojson::value(cpu_name);
    spec["gpu"] = picojson::value(gpu_name);
    std::string specContent = picojson::value(spec).serialize();
    std::cerr << "[spec] " << specContent << "\n";
    send_command("POST", "spec", specContent, true);
  }

  // Send temperature unit
  set_temperature_unit(temp_unit);

  return {};
}

std::optional<Response> Device::send_full_config(
    const ScreenConfig& config,
    const std::string& cpu_name,
    const std::string& gpu_name,
    int brightness,
    const std::string& temp_unit) {

  // Build full config JSON matching KANALI format
  picojson::object root;
  root["temperature"] = picojson::value(temp_unit);

  // waterBlockScreen object
  picojson::object wbs;
  wbs["enable"] = picojson::value(true);
  wbs["displayInSleep"] = picojson::value(false);
  wbs["brightness"] = picojson::value(static_cast<double>(brightness));
  wbs["waterfallMode"] = picojson::value(config.waterfall_mode);

  // Embed screen config as "id"
  auto build_settings = [](const DisplaySettings& ds) -> picojson::object {
    picojson::object filter;
    filter["value"] = picojson::value("");
    filter["opacity"] = picojson::value(static_cast<double>(ds.filter_opacity));
    picojson::array badges_arr;
    for (const auto& b : ds.badges)
      badges_arr.push_back(picojson::value(b));
    picojson::object s;
    s["position"] = picojson::value(ds.position);
    s["color"] = picojson::value(ds.color);
    s["align"] = picojson::value(ds.align);
    s["filter"] = picojson::value(filter);
    s["badges"] = picojson::value(badges_arr);
    return s;
  };

  picojson::object screenCfg;
  screenCfg["Type"] = picojson::value(config.preset_id.empty() ? "Custom" : "Pre-set");
  screenCfg["id"] = picojson::value(config.preset_id.empty() ? "Customization" : config.preset_id);
  screenCfg["screenMode"] = picojson::value(config.screen_mode);
  screenCfg["ratio"] = picojson::value(config.ratio);
  screenCfg["playMode"] = picojson::value(config.play_mode);

  picojson::array media_arr;
  for (const auto& m : config.media)
    media_arr.push_back(picojson::value(m));
  screenCfg["media"] = picojson::value(media_arr);

  if (config.screen_mode == "Screen Splitting") {
    picojson::array settings_arr;
    settings_arr.push_back(picojson::value(build_settings(config.settings)));
    settings_arr.push_back(picojson::value(build_settings(config.settings2)));
    screenCfg["settings"] = picojson::value(settings_arr);

    picojson::array sysinfo_outer;
    picojson::array left, right;
    for (const auto& l : config.sysinfo_display)
      left.push_back(picojson::value(l));
    for (const auto& r : config.sysinfo_display2)
      right.push_back(picojson::value(r));
    sysinfo_outer.push_back(picojson::value(left));
    sysinfo_outer.push_back(picojson::value(right));
    screenCfg["sysinfoDisplay"] = picojson::value(sysinfo_outer);
  } else {
    screenCfg["settings"] = picojson::value(build_settings(config.settings));
    picojson::array sysinfo_arr;
    for (const auto& l : config.sysinfo_display)
      sysinfo_arr.push_back(picojson::value(l));
    screenCfg["sysinfoDisplay"] = picojson::value(sysinfo_arr);
  }

  wbs["id"] = picojson::value(screenCfg);
  root["waterBlockScreen"] = picojson::value(wbs);

  // spec
  picojson::object spec;
  spec["cpu"] = picojson::value(cpu_name);
  spec["gpu"] = picojson::value(gpu_name);
  root["spec"] = picojson::value(spec);

  std::string content = picojson::value(root).serialize();
  std::cerr << "[full_config] " << content.substr(0, 200) << "...\n";

  return send_command("POST", "config", content);
}

std::optional<Response> Device::send_sysinfo(
    const std::vector<SysinfoData>& data) {
  // Build PcInfo JSON object matching device firmware expectations
  picojson::object cpu, gpu, memory, motherboard, disk, network;
  picojson::array fans;

  // Defaults
  cpu["load"] = picojson::value(0.0);
  cpu["temperature"] = picojson::value(0.0);
  cpu["speedAverage"] = picojson::value(0.0);
  cpu["voltage"] = picojson::value(0.0);
  cpu["power"] = picojson::value(0.0);
  cpu["fanAverage"] = picojson::value(0.0);

  gpu["load"] = picojson::value(0.0);
  gpu["temperature"] = picojson::value(std::string("0")); // GPU temp is STRING!
  gpu["speed"] = picojson::value(0.0);
  gpu["voltage"] = picojson::value(0.0);
  gpu["power"] = picojson::value(0.0);
  gpu["fan"] = picojson::value(0.0);

  memory["load"] = picojson::value(0.0);
  memory["speed"] = picojson::value(0.0);
  memory["temperature"] = picojson::value(0.0);
  memory["total"] = picojson::value(0.0);
  memory["used"] = picojson::value(0.0);

  motherboard["temperature"] = picojson::value(0.0);

  disk["load"] = picojson::value(0.0);
  disk["used"] = picojson::value(0.0);
  disk["total"] = picojson::value(0.0);
  disk["temperature"] = picojson::value(0.0);
  disk["activity"] = picojson::value(0.0);
  disk["readSpeed"] = picojson::value(0.0);
  disk["writeSpeed"] = picojson::value(0.0);

  network["download"] = picojson::value(0.0);
  network["upload"] = picojson::value(0.0);

  // Fill from provided data
  for (const auto& item : data) {
    double val = 0;
    try { val = std::stod(item.value); } catch (...) {}

    if (item.label == "CPU Temperature") {
      cpu["temperature"] = picojson::value(val);
    } else if (item.label == "CPU Frequency") {
      cpu["speedAverage"] = picojson::value(val);
    } else if (item.label == "CPU Usage") {
      cpu["load"] = picojson::value(val);
    } else if (item.label == "CPU Voltage") {
      cpu["voltage"] = picojson::value(val);
    } else if (item.label == "GPU Temperature") {
      gpu["temperature"] = picojson::value(item.value); // String!
    } else if (item.label == "GPU Frequency") {
      gpu["speed"] = picojson::value(val);
    } else if (item.label == "GPU Usage") {
      gpu["load"] = picojson::value(val);
    } else if (item.label == "GPU Voltage") {
      gpu["voltage"] = picojson::value(val);
    } else if (item.label == "Hard Disk Temperature") {
      disk["temperature"] = picojson::value(val);
    } else if (item.label == "Motherboard Temperature") {
      motherboard["temperature"] = picojson::value(val);
    } else if (item.label == "Memory Frequency") {
      memory["speed"] = picojson::value(val);
    } else if (item.label == "Memory Utilization") {
      memory["load"] = picojson::value(val);
    }
  }

  picojson::object pcInfo;
  pcInfo["cpu"] = picojson::value(cpu);
  pcInfo["gpu"] = picojson::value(gpu);
  pcInfo["memory"] = picojson::value(memory);
  pcInfo["motherboard"] = picojson::value(motherboard);
  pcInfo["disk"] = picojson::value(disk);
  pcInfo["network"] = picojson::value(network);
  pcInfo["fans"] = picojson::value(fans);
  pcInfo["timestamp"] = picojson::value(static_cast<double>(
      std::chrono::system_clock::now().time_since_epoch().count() / 1000000));

  std::string content = picojson::value(pcInfo).serialize();

  // Device expects "POST all" command, not "POST sysinfo"
  return send_command("POST", "all", content, false);
}

std::optional<Response> Device::set_waterfall_mode(bool enable) {
  picojson::object obj;
  obj["enable"] = picojson::value(enable);
  std::string content = picojson::value(obj).serialize();
  std::cerr << "[waterfallMode] " << content << "\n";
  return send_command("POST", "waterfallMode", content);
}

std::optional<Response> Device::set_rotation(int degrees) {
  picojson::object obj;
  obj["degree"] = picojson::value(static_cast<double>(degrees));
  std::string content = picojson::value(obj).serialize();
  return send_command("POST", "rotate", content);
}

std::optional<Response> Device::reboot() {
  return send_command("POST", "reboot", "");
}

std::optional<Response> Device::set_brightness(int value) {
  picojson::object obj;
  obj["value"] = picojson::value(static_cast<double>(value));
  std::string content = picojson::value(obj).serialize();
  return send_command("POST", "brightness", content);
}

std::optional<Response> Device::delete_media(
    const std::vector<std::string>& files) {
  picojson::array file_arr;
  for (const auto& f : files) {
    file_arr.push_back(picojson::value(f));
  }
  picojson::object obj;
  obj["include"] = picojson::value(file_arr);
  std::string content = picojson::value(obj).serialize();
  return send_command("POST", "mediaDelete", content);
}

}  // namespace panorama
