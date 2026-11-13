#include "panorama/process.hpp"

#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace panorama {

namespace {

class SpawnFileActions {
 public:
  SpawnFileActions() { valid_ = posix_spawn_file_actions_init(&actions_) == 0; }
  ~SpawnFileActions() {
    if (valid_) {
      posix_spawn_file_actions_destroy(&actions_);
    }
  }
  SpawnFileActions(const SpawnFileActions&) = delete;
  SpawnFileActions& operator=(const SpawnFileActions&) = delete;

  bool valid() const { return valid_; }
  posix_spawn_file_actions_t* get() { return &actions_; }

 private:
  posix_spawn_file_actions_t actions_{};
  bool valid_ = false;
};

void close_quietly(int& fd) {
  if (fd >= 0) {
    close(fd);
    fd = -1;
  }
}

}  // namespace

std::optional<ProcessResult> run_process(const std::vector<std::string>& argv,
                                         bool capture_output) {
  if (argv.empty() || argv.front().empty()) {
    return std::nullopt;
  }

  SpawnFileActions actions;
  if (!actions.valid()) {
    return std::nullopt;
  }

  int pipe_fds[2] = {-1, -1};
  if (capture_output) {
    if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
      return std::nullopt;
    }
  }

  bool actions_ok =
      posix_spawn_file_actions_addopen(actions.get(), STDIN_FILENO,
                                       "/dev/null", O_RDONLY, 0) == 0;
  if (capture_output) {
    actions_ok = actions_ok &&
        posix_spawn_file_actions_adddup2(actions.get(), pipe_fds[1],
                                         STDOUT_FILENO) == 0 &&
        posix_spawn_file_actions_adddup2(actions.get(), pipe_fds[1],
                                         STDERR_FILENO) == 0;
  } else {
    actions_ok = actions_ok &&
        posix_spawn_file_actions_addopen(actions.get(), STDOUT_FILENO,
                                         "/dev/null", O_WRONLY, 0) == 0 &&
        posix_spawn_file_actions_adddup2(actions.get(), STDOUT_FILENO,
                                         STDERR_FILENO) == 0;
  }
  if (!actions_ok) {
    close_quietly(pipe_fds[0]);
    close_quietly(pipe_fds[1]);
    return std::nullopt;
  }

  std::vector<char*> child_argv;
  child_argv.reserve(argv.size() + 1);
  for (const auto& arg : argv) {
    child_argv.push_back(const_cast<char*>(arg.c_str()));
  }
  child_argv.push_back(nullptr);

  pid_t pid = -1;
  const int spawn_error = posix_spawnp(&pid, argv.front().c_str(),
                                       actions.get(), nullptr,
                                       child_argv.data(), environ);
  close_quietly(pipe_fds[1]);
  if (spawn_error != 0) {
    close_quietly(pipe_fds[0]);
    return std::nullopt;
  }

  ProcessResult result;
  if (capture_output) {
    char chunk[4096];
    while (true) {
      const ssize_t n = read(pipe_fds[0], chunk, sizeof(chunk));
      if (n > 0) {
        result.output.append(chunk, static_cast<size_t>(n));
        continue;
      }
      if (n < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
    close_quietly(pipe_fds[0]);
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return std::nullopt;
    }
  }
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  }
  return result;
}

std::string shell_quote(const std::string& arg) {
  std::string quoted;
  quoted.reserve(arg.size() + 2);
  quoted.push_back('\'');
  for (const char ch : arg) {
    if (ch == '\'') {
      quoted.append("'\\''");
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

}  // namespace panorama
