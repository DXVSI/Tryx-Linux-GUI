#include "panorama/adb.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace panorama {

std::string Adb::escape_shell_arg(const std::string& input) {
  bool needs_quoting = false;
  const char* unsafe_chars = " \t'\"\\$`!#&|;(){}[]<>?*~";
  for (char ch : input) {
    if (std::strchr(unsafe_chars, ch) != nullptr) {
      needs_quoting = true;
      break;
    }
  }
  if (!needs_quoting) {
    return input;
  }

  std::string escaped = "\"";
  for (char ch : input) {
    if (ch == '"' || ch == '\\' || ch == '$' || ch == '`') {
      escaped += '\\';
    }
    escaped += ch;
  }
  escaped += '"';
  return escaped;
}

std::string Adb::find_tryx_serial() {
  // Find TRYX device serial among connected ADB devices
  FILE* proc = popen("adb devices 2>&1", "r");
  if (!proc) return {};
  char chunk[2048];
  std::string output;
  while (fgets(chunk, sizeof(chunk), proc))
    output += chunk;
  pclose(proc);

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
  std::string commandline = "adb";

  // Auto-select TRYX device if multiple devices connected
  static std::string cached_serial;
  if (cached_serial.empty()) {
    cached_serial = find_tryx_serial();
  }
  if (!cached_serial.empty() && (args.empty() || args[0] != "devices")) {
    commandline += " -s " + escape_shell_arg(cached_serial);
  }

  for (const auto& token : args) {
    commandline += ' ';
    commandline += escape_shell_arg(token);
  }
  commandline += " 2>&1";

  FILE* proc = popen(commandline.c_str(), "r");
  if (proc == nullptr) {
    return std::nullopt;
  }

  std::string collected;
  char chunk[2048];
  while (std::fgets(chunk, sizeof(chunk), proc) != nullptr) {
    collected.append(chunk);
  }
  pclose(proc);

  return collected;
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
  auto output = run_command({"shell", "ls", "-1", MEDIA_PATH});
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
  std::string target = std::string(MEDIA_PATH) + filename;
  auto output = run_command({"shell", "ls", target});
  return output.has_value() &&
         output.value().find("No such file") == std::string::npos;
}

bool Adb::remove(const std::string& filename) {
  std::string target = std::string(MEDIA_PATH) + filename;
  auto output = run_command({"shell", "rm", target});
  return output.has_value() &&
         output.value().find("No such file") == std::string::npos;
}

}  // namespace panorama
