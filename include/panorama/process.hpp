#pragma once

#include <optional>
#include <string>
#include <vector>

namespace panorama {

struct ProcessResult {
  // Exit status, or -1 when the program did not exit normally.
  int exit_code = -1;
  // Standard output and standard error together, when captured.
  std::string output;
};

// Starts argv[0] from PATH with argv as its argument vector. No shell parses
// the arguments. Standard input is /dev/null. Without capture_output, the
// program's output goes to /dev/null. Returns nullopt when the program cannot
// be started.
std::optional<ProcessResult> run_process(const std::vector<std::string>& argv,
                                         bool capture_output);

// Quotes one argument for a POSIX shell, such as the device shell behind
// `adb shell`. Every argument is single-quoted, empty ones included, so no
// character in it keeps a special meaning.
std::string shell_quote(const std::string& arg);

}  // namespace panorama
