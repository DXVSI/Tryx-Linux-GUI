#include "panorama/adb.hpp"

#include "panorama/process.hpp"

#include <utility>

namespace panorama {

bool Adb::is_plain_media_name(const std::string& name) {
  return !name.empty() && name != "." && name != ".." &&
         name.find('/') == std::string::npos &&
         name.find('\0') == std::string::npos &&
         name.find('\n') == std::string::npos;
}

std::string Adb::find_tryx_serial() {
  // Find TRYX device serial among connected ADB devices
  auto result = run_process({"adb", "devices"}, true);
  if (!result) return {};
  const std::string& output = result->output;

  std::string::size_type pos = 0;
  while (pos < output.size()) {
    auto eol = output.find('\n', pos);
    if (eol == std::string::npos) eol = output.size();
    std::string row = output.substr(pos, eol - pos);
    auto tab = row.find('\t');
    if (tab != std::string::npos) {
      std::string serial = row.substr(0, tab);
      // TRYX devices have "TRYX" in serial number
      if (serial.find("TRYX") != std::string::npos) {
        return serial;
      }
    }
    pos = eol + 1;
  }
  return {};
}

std::optional<std::string> Adb::run_command(
    const std::vector<std::string>& args) {
  std::vector<std::string> argv{"adb"};

  // Auto-select TRYX device if multiple devices connected
  static std::string cached_serial;
  if (cached_serial.empty()) {
    cached_serial = find_tryx_serial();
  }
  if (!cached_serial.empty() && (args.empty() || args[0] != "devices")) {
    argv.push_back("-s");
    argv.push_back(cached_serial);
  }
  argv.insert(argv.end(), args.begin(), args.end());

  // adb runs without a local shell, so no argument here is parsed by one.
  auto result = run_process(argv, true);
  if (!result) {
    return std::nullopt;
  }
  return std::move(result->output);
}

std::optional<std::string> Adb::run_device_shell(
    const std::vector<std::string>& remote_args) {
  // adb joins the words after "shell" and the device shell parses them, so
  // quote every word for that shell and pass the command as one argument.
  std::string remote_command;
  for (const auto& arg : remote_args) {
    if (!remote_command.empty()) {
      remote_command.push_back(' ');
    }
    remote_command += shell_quote(arg);
  }
  return run_command({"shell", remote_command});
}

bool Adb::is_device_connected() {
  auto output = run_command({"devices"});
  if (!output.has_value()) {
    return false;
  }

  const std::string& text = output.value();
  std::string::size_type pos = 0;
  while (pos < text.size()) {
    auto eol = text.find('\n', pos);
    if (eol == std::string::npos) {
      eol = text.size();
    }
    std::string row = text.substr(pos, eol - pos);
    auto tab_idx = row.find('\t');
    if (tab_idx != std::string::npos) {
      std::string status = row.substr(tab_idx + 1);
      while (!status.empty() && (status.back() == '\r' ||
                                  status.back() == '\n' ||
                                  status.back() == ' ')) {
        status.pop_back();
      }
      if (status == "device") {
        return true;
      }
    }
    pos = eol + 1;
  }
  return false;
}

bool Adb::push(const std::string& local_path,
               const std::string& remote_name) {
  if (!is_plain_media_name(remote_name)) {
    return false;
  }
  std::string dest = std::string(MEDIA_PATH) + remote_name;
  auto output = run_command({"push", local_path, dest});
  if (!output.has_value()) {
    return false;
  }
  const auto& msg = output.value();
  return msg.find("pushed") != std::string::npos ||
         msg.find("1 file") != std::string::npos;
}

std::optional<std::vector<std::string>> Adb::list_media() {
  auto output = run_device_shell({"ls", "-1", MEDIA_PATH});
  if (!output.has_value()) {
    return std::nullopt;
  }

  const auto& text = output.value();
  if (text.find("No such file") != std::string::npos ||
      text.find("error:") != std::string::npos) {
    return std::vector<std::string>{};
  }

  std::vector<std::string> entries;
  std::string::size_type start = 0;
  while (start < text.size()) {
    auto nl = text.find('\n', start);
    std::string::size_type end = (nl == std::string::npos) ? text.size() : nl;
    std::string entry = text.substr(start, end - start);

    // strip trailing whitespace and carriage returns
    auto last_valid = entry.find_last_not_of(" \t\r\n");
    if (last_valid != std::string::npos) {
      entry.erase(last_valid + 1);
    } else {
      entry.clear();
    }

    if (!entry.empty()) {
      entries.push_back(std::move(entry));
    }
    start = end + 1;
  }

  return entries;
}

bool Adb::file_exists(const std::string& filename) {
  if (!is_plain_media_name(filename)) {
    return false;
  }
  std::string target = std::string(MEDIA_PATH) + filename;
  auto output = run_device_shell({"ls", target});
  return output.has_value() &&
         output.value().find("No such file") == std::string::npos;
}

bool Adb::remove(const std::string& filename) {
  if (!is_plain_media_name(filename)) {
    return false;
  }
  std::string target = std::string(MEDIA_PATH) + filename;
  auto output = run_device_shell({"rm", target});
  return output.has_value() &&
         output.value().find("No such file") == std::string::npos;
}

bool Adb::reboot() {
  // Never reboot whatever other Android device adb would pick by default.
  if (find_tryx_serial().empty()) {
    return false;
  }
  return run_command({"reboot"}).has_value();
}

}  // namespace panorama
