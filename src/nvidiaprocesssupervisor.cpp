#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "nvidiaprocesssupervisor.h"

#include <QDir>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

namespace tryx::nvidia {
namespace {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

constexpr auto kReapGrace = std::chrono::milliseconds(250);
constexpr int kActivePollMilliseconds = 25;
constexpr int kQuarantinePollMilliseconds = 100;
constexpr qsizetype kMaximumArgumentBytes = 4096;
constexpr qsizetype kMaximumArguments = 16;

class UniqueFd final {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    UniqueFd(UniqueFd &&other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd &operator=(UniqueFd &&other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.fd_, -1));
        }
        return *this;
    }

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    int release() { return std::exchange(fd_, -1); }

    void reset(int fd = -1) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

int moveFdAtLeastThree(int fd) {
    if (fd < 0 || fd >= 3) {
        return fd;
    }
    const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
    const int savedErrno = errno;
    ::close(fd);
    errno = savedErrno;
    return moved;
}

UniqueFd createEventFd() {
    return UniqueFd(moveFdAtLeastThree(
        ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)));
}

void notifyEventFd(int fd) {
    if (fd < 0) {
        return;
    }
    const eventfd_t value = 1;
    while (::eventfd_write(fd, value) != 0 && errno == EINTR) {
    }
}

void drainEventFd(int fd) {
    if (fd < 0) {
        return;
    }
    eventfd_t value = 0;
    for (;;) {
        if (::eventfd_read(fd, &value) == 0) {
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        return;
    }
}

bool deadlineExpired(Deadline deadline) {
    return Clock::now() >= deadline;
}

bool childReapingIsOwned() {
    struct sigaction action {};
    if (::sigaction(SIGCHLD, nullptr, &action) != 0) {
        return false;
    }
    return action.sa_handler == SIG_DFL &&
           (action.sa_flags & SA_NOCLDWAIT) == 0;
}

bool validSpec(const ProcessSpec &spec) {
    if (spec.executable.isEmpty() ||
        !QDir::isAbsolutePath(spec.executable) ||
        spec.executable.contains(QChar::Null) ||
        spec.arguments.size() > kMaximumArguments ||
        spec.stdoutLimit <= 0 || spec.stderrLimit <= 0 ||
        spec.stdoutLimit > 64 * 1024 || spec.stderrLimit > 16 * 1024) {
        return false;
    }
    const QByteArray executable = spec.executable.toLocal8Bit();
    if (executable.isEmpty() || executable.contains('\0') ||
        executable.size() > kMaximumArgumentBytes) {
        return false;
    }
    for (const QString &argument : spec.arguments) {
        const QByteArray encoded = argument.toLocal8Bit();
        if (argument.contains(QChar::Null) || encoded.contains('\0') ||
            encoded.size() > kMaximumArgumentBytes) {
            return false;
        }
    }
    return true;
}

class SpawnFileActions final {
public:
    SpawnFileActions() : error_(::posix_spawn_file_actions_init(&actions_)) {}
    ~SpawnFileActions() {
        if (error_ == 0) {
            ::posix_spawn_file_actions_destroy(&actions_);
        }
    }

    bool configure(int stdoutWriteFd, int stderrWriteFd) {
        if (error_ != 0) {
            return false;
        }
        // Actions execute in insertion order. Child fd 1/2 stay blocking and
        // closefrom removes unrelated serial/recovery descriptors.
        // https://man7.org/linux/man-pages/man3/posix_spawn.3.html
        // https://sourceware.org/pipermail/glibc-cvs/2021q3/073654.html
        return ::posix_spawn_file_actions_addopen(
                   &actions_, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
               ::posix_spawn_file_actions_adddup2(
                   &actions_, stdoutWriteFd, STDOUT_FILENO) == 0 &&
               ::posix_spawn_file_actions_adddup2(
                   &actions_, stderrWriteFd, STDERR_FILENO) == 0 &&
               ::posix_spawn_file_actions_addclosefrom_np(&actions_, 3) == 0;
    }

    posix_spawn_file_actions_t *get() { return &actions_; }

private:
    posix_spawn_file_actions_t actions_ {};
    int error_ = 0;
};

class SpawnAttributes final {
public:
    SpawnAttributes() : error_(::posix_spawnattr_init(&attributes_)) {}
    ~SpawnAttributes() {
        if (error_ == 0) {
            ::posix_spawnattr_destroy(&attributes_);
        }
    }

    bool configure() {
        if (error_ != 0) {
            return false;
        }
        sigset_t emptyMask;
        sigset_t defaultSignals;
        ::sigemptyset(&emptyMask);
        ::sigemptyset(&defaultSignals);
        ::sigaddset(&defaultSignals, SIGPIPE);
        const short flags = POSIX_SPAWN_SETSIGMASK |
                            POSIX_SPAWN_SETSIGDEF;
        return ::posix_spawnattr_setsigmask(
                   &attributes_, &emptyMask) == 0 &&
               ::posix_spawnattr_setsigdefault(
                   &attributes_, &defaultSignals) == 0 &&
               ::posix_spawnattr_setflags(&attributes_, flags) == 0;
    }

    posix_spawnattr_t *get() { return &attributes_; }

private:
    posix_spawnattr_t attributes_ {};
    int error_ = 0;
};

enum class DrainResult {
    Open,
    Eof,
    Overflow,
    Error,
};

DrainResult drainPipe(UniqueFd *fd, QByteArray *retained,
                      qsizetype *byteCount, qsizetype limit) {
    if (!fd || !fd->valid()) {
        return DrainResult::Eof;
    }
    char buffer[4096];
    for (;;) {
        const ssize_t count = ::read(fd->get(), buffer, sizeof(buffer));
        if (count > 0) {
            if (*byteCount > limit - count) {
                fd->reset();
                return DrainResult::Overflow;
            }
            *byteCount += count;
            if (retained) {
                retained->append(buffer, count);
            }
            continue;
        }
        if (count == 0) {
            fd->reset();
            return DrainResult::Eof;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return DrainResult::Open;
        }
        fd->reset();
        return DrainResult::Error;
    }
}

bool setReadEndNonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool createPipe(UniqueFd *readEnd, UniqueFd *writeEnd) {
    int descriptors[2] = {-1, -1};
    if (::pipe2(descriptors, O_CLOEXEC) != 0) {
        return false;
    }
    UniqueFd read(moveFdAtLeastThree(descriptors[0]));
    UniqueFd write(moveFdAtLeastThree(descriptors[1]));
    if (!read.valid() || !write.valid() ||
        !setReadEndNonblocking(read.get())) {
        return false;
    }
    *readEnd = std::move(read);
    *writeEnd = std::move(write);
    return true;
}

int pollTimeoutMilliseconds(
    Deadline deadline,
    const std::optional<Clock::time_point> &killStarted,
    bool reaped) {
    const Clock::time_point now = Clock::now();
    if (killStarted.has_value() && !reaped) {
        const auto elapsed = now - *killStarted;
        if (elapsed >= kReapGrace) {
            return kQuarantinePollMilliseconds;
        }
        const auto remainingGrace =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                kReapGrace - elapsed);
        return static_cast<int>(std::max<qint64>(
            1, std::min<qint64>(remainingGrace.count(),
                                kActivePollMilliseconds)));
    }
    if (now >= deadline) {
        return 0;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);
    return static_cast<int>(std::min<qint64>(
        std::max<qint64>(remaining.count(), 1),
        kActivePollMilliseconds));
}

}  // namespace

struct ProcessAttempt::State final {
    State(ProcessSpec requestedSpec, Deadline requestedDeadline,
          quint64 requestedGeneration,
          PosixSpawnFunction requestedSpawnFunction,
          KillFunction requestedKillFunction,
          WaitPidFunction requestedWaitPidFunction,
          UniqueFd result, UniqueFd control)
        : spec(std::move(requestedSpec)), deadline(requestedDeadline),
          generation(requestedGeneration),
          spawnFunction(std::move(requestedSpawnFunction)),
          killFunction(std::move(requestedKillFunction)),
          waitPidFunction(std::move(requestedWaitPidFunction)),
          resultEventFd(result.release()),
          controlEventFd(control.release()) {}

    ~State() {
        if (resultEventFd >= 0) {
            ::close(resultEventFd);
        }
        if (controlEventFd >= 0) {
            ::close(controlEventFd);
        }
    }

    bool publishFirst(ProcessOutcome outcome, bool terminal,
                      const QByteArray &stdoutData = {},
                      int exitCode = -1) {
        const std::lock_guard<std::mutex> publicationLock(
            publicationMutex);
        bool expected = false;
        if (!outcomePublished.compare_exchange_strong(expected, true)) {
            return false;
        }
        queue(ProcessEvent{outcome, generation, stdoutData,
                           exitCode, terminal});
        return true;
    }

    void queue(ProcessEvent event) {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            events.append(std::move(event));
        }
        notifyEventFd(resultEventFd);
    }

    void requestStop(ProcessOutcome reason, bool publish) {
        int expected = -1;
        stopReason.compare_exchange_strong(
            expected, static_cast<int>(reason));
        const bool firstStop = !cancelRequested.exchange(
            true, std::memory_order_acq_rel);
        if (publish) {
            publishFirst(requestedStopReason(), false);
        }
        if (firstStop) {
            notifyEventFd(controlEventFd);
        }
    }

    void requestShutdown() {
        const bool firstShutdown = !shutdownRequested.exchange(
            true, std::memory_order_acq_rel);
        const bool wasStopping = cancelRequested.load(
            std::memory_order_acquire);
        requestStop(ProcessOutcome::Cancelled, false);
        if (firstShutdown && wasStopping) {
            notifyEventFd(controlEventFd);
        }
    }

    ProcessOutcome requestedStopReason() const {
        const int value = stopReason.load(std::memory_order_acquire);
        return value < 0 ? ProcessOutcome::Cancelled
                         : static_cast<ProcessOutcome>(value);
    }

    void finish(ProcessOutcome outcome,
                const QByteArray &stdoutData = {}, int exitCode = -1) {
        if (!publishFirst(outcome, true, stdoutData, exitCode)) {
            queue(ProcessEvent{ProcessOutcome::Cleanup, generation, {},
                               -1, true});
        }
        workerDone.store(true, std::memory_order_release);
        deadlineCondition.notify_all();
    }

    void finishOwnershipLost() {
        {
            const std::lock_guard<std::mutex> publicationLock(
                publicationMutex);
            outcomePublished.store(true, std::memory_order_release);
            queue(ProcessEvent{ProcessOutcome::OwnershipLost, generation, {},
                               -1, true});
        }
        workerDone.store(true, std::memory_order_release);
        deadlineCondition.notify_all();
    }

    ProcessSpec spec;
    const Deadline deadline;
    const quint64 generation;
    PosixSpawnFunction spawnFunction;
    KillFunction killFunction;
    WaitPidFunction waitPidFunction;
    const int resultEventFd;
    const int controlEventFd;
    std::mutex publicationMutex;
    std::mutex mutex;
    std::mutex deadlineMutex;
    std::condition_variable deadlineCondition;
    QVector<ProcessEvent> events;
    std::atomic<bool> cancelRequested{false};
    std::atomic<bool> outcomePublished{false};
    std::atomic<bool> workerDone{false};
    std::atomic<bool> shutdownRequested{false};
    std::atomic<int> stopReason{-1};
};

namespace {

void finishStopped(const std::shared_ptr<ProcessAttempt::State> &state) {
    state->finish(state->requestedStopReason());
}

bool stopForDeadline(const std::shared_ptr<ProcessAttempt::State> &state) {
    if (!deadlineExpired(state->deadline)) {
        return false;
    }
    state->requestStop(ProcessOutcome::Timeout, true);
    return true;
}

void runAttempt(const std::shared_ptr<ProcessAttempt::State> &state) {
    if (stopForDeadline(state) || state->cancelRequested.load()) {
        finishStopped(state);
        return;
    }

    UniqueFd stdoutRead;
    UniqueFd stdoutWrite;
    UniqueFd stderrRead;
    UniqueFd stderrWrite;
    if (!createPipe(&stdoutRead, &stdoutWrite) ||
        !createPipe(&stderrRead, &stderrWrite)) {
        state->finish(ProcessOutcome::StartFailed);
        return;
    }

    SpawnFileActions fileActions;
    SpawnAttributes attributes;
    if (!fileActions.configure(stdoutWrite.get(), stderrWrite.get()) ||
        !attributes.configure()) {
        state->finish(ProcessOutcome::StartFailed);
        return;
    }

    std::vector<QByteArray> argumentStorage;
    argumentStorage.reserve(
        static_cast<size_t>(state->spec.arguments.size() + 1));
    argumentStorage.push_back(state->spec.executable.toLocal8Bit());
    for (const QString &argument : state->spec.arguments) {
        argumentStorage.push_back(argument.toLocal8Bit());
    }
    std::vector<char *> arguments;
    arguments.reserve(argumentStorage.size() + 1);
    for (QByteArray &argument : argumentStorage) {
        arguments.push_back(argument.data());
    }
    arguments.push_back(nullptr);

    QByteArray localeAll("LC_ALL=C");
    QByteArray language("LANG=C");
    char *environment[] = {localeAll.data(), language.data(), nullptr};

    if (stopForDeadline(state) || state->cancelRequested.load()) {
        finishStopped(state);
        return;
    }

    pid_t childPid = -1;
    const QByteArray executable = state->spec.executable.toLocal8Bit();
    const int spawnResult = state->spawnFunction(
        &childPid, executable.constData(), fileActions.get(), attributes.get(),
        arguments.data(), environment);

    stdoutWrite.reset();
    stderrWrite.reset();

    if (spawnResult != 0 || childPid <= 0) {
        if (stopForDeadline(state) || state->cancelRequested.load()) {
            finishStopped(state);
        } else {
            state->finish(ProcessOutcome::StartFailed, {}, spawnResult);
        }
        return;
    }

    bool reaped = false;
    bool killSent = false;
    bool shutdownKillSent = false;
    bool ownershipLost = false;
    int childStatus = 0;
    QByteArray stdoutData;
    qsizetype stdoutBytes = 0;
    qsizetype stderrBytes = 0;
    std::optional<Clock::time_point> killStarted;

    for (;;) {
        stopForDeadline(state);
        bool stopping = state->cancelRequested.load(std::memory_order_acquire);
        const bool shuttingDown =
            state->shutdownRequested.load(std::memory_order_acquire);
        if (shuttingDown) {
            stdoutRead.reset();
            stderrRead.reset();
        }

        pollfd descriptors[3];
        nfds_t descriptorCount = 0;
        if (stdoutRead.valid()) {
            descriptors[descriptorCount++] =
                pollfd{stdoutRead.get(), POLLIN | POLLHUP | POLLERR, 0};
        }
        if (stderrRead.valid()) {
            descriptors[descriptorCount++] =
                pollfd{stderrRead.get(), POLLIN | POLLHUP | POLLERR, 0};
        }
        descriptors[descriptorCount++] =
            pollfd{state->controlEventFd, POLLIN | POLLERR, 0};

        const int pollResult = ::poll(
            descriptors, descriptorCount,
            pollTimeoutMilliseconds(state->deadline, killStarted,
                                    reaped));
        if (pollResult < 0 && errno != EINTR) {
            state->requestStop(ProcessOutcome::IoFailure, true);
            stopping = true;
        }
        drainEventFd(state->controlEventFd);

        if (state->shutdownRequested.load(std::memory_order_acquire)) {
            stdoutRead.reset();
            stderrRead.reset();
        }

        const DrainResult stdoutState = drainPipe(
            &stdoutRead, &stdoutData, &stdoutBytes,
            state->spec.stdoutLimit);
        const DrainResult stderrState = drainPipe(
            &stderrRead, nullptr, &stderrBytes,
            state->spec.stderrLimit);
        if (stdoutState == DrainResult::Overflow ||
            stderrState == DrainResult::Overflow) {
            state->requestStop(ProcessOutcome::OutputLimit, true);
            stopping = true;
        } else if (stdoutState == DrainResult::Error ||
                   stderrState == DrainResult::Error) {
            state->requestStop(ProcessOutcome::IoFailure, true);
            stopping = true;
        }

        if (!reaped) {
            const pid_t waitResult = state->waitPidFunction(
                childPid, &childStatus, WNOHANG);
            if (waitResult == childPid) {
                reaped = true;
            } else if (waitResult < 0 && errno != EINTR) {
                if (errno == ECHILD) {
                    ownershipLost = true;
                    stdoutRead.reset();
                    stderrRead.reset();
                } else {
                    state->requestStop(ProcessOutcome::IoFailure, true);
                    stopping = true;
                }
            }
        }

        if (ownershipLost) {
            state->finishOwnershipLost();
            return;
        }

        stopping = stopping ||
            state->cancelRequested.load(std::memory_order_acquire);
        const bool shutdownNeedsKill =
            state->shutdownRequested.load(std::memory_order_acquire) &&
            !shutdownKillSent;
        if (stopping && !reaped && (!killSent || shutdownNeedsKill)) {
            if (!killStarted.has_value()) {
                killStarted = Clock::now();
            }
            bool killCompleted = false;
            for (;;) {
                if (state->killFunction(childPid, SIGKILL) == 0 ||
                    errno == ESRCH) {
                    killCompleted = true;
                    break;
                }
                if (errno != EINTR) {
                    state->publishFirst(ProcessOutcome::IoFailure, false);
                    break;
                }
            }
            killSent = killSent || killCompleted;
            if (shutdownNeedsKill && killCompleted) {
                shutdownKillSent = true;
            }
        }

        if (stopping && reaped) {
            finishStopped(state);
            return;
        }

        if (!stopping && reaped && !stdoutRead.valid() &&
            !stderrRead.valid()) {
            if (deadlineExpired(state->deadline)) {
                state->requestStop(ProcessOutcome::Timeout, true);
                finishStopped(state);
                return;
            }
            if (WIFEXITED(childStatus) && WEXITSTATUS(childStatus) == 0) {
                state->finish(ProcessOutcome::Success, stdoutData, 0);
            } else {
                const int exitCode = WIFEXITED(childStatus)
                    ? WEXITSTATUS(childStatus)
                    : -1;
                state->finish(ProcessOutcome::ExitFailure, {}, exitCode);
            }
            return;
        }
    }
}

struct ThreadContext final {
    std::shared_ptr<ProcessAttempt::State> state;
};

void *attemptThreadMain(void *opaque) {
    std::unique_ptr<ThreadContext> context(
        static_cast<ThreadContext *>(opaque));
    runAttempt(context->state);
    return nullptr;
}

void *deadlineThreadMain(void *opaque) {
    std::unique_ptr<ThreadContext> context(
        static_cast<ThreadContext *>(opaque));
    const auto &state = context->state;
    std::unique_lock<std::mutex> lock(state->deadlineMutex);
    const bool completed = state->deadlineCondition.wait_until(
        lock, state->deadline, [state]() {
            return state->workerDone.load(std::memory_order_acquire);
        });
    lock.unlock();
    if (!completed) {
        state->requestStop(ProcessOutcome::Timeout, true);
    }
    return nullptr;
}

int startDetachedThread(void *(*entry)(void *), ThreadContext *context) {
    pthread_attr_t attributes {};
    const int attributeInitError = ::pthread_attr_init(&attributes);
    int error = attributeInitError;
    if (error == 0) {
        error = ::pthread_attr_setdetachstate(
            &attributes, PTHREAD_CREATE_DETACHED);
    }
    pthread_t thread;
    if (error == 0) {
        error = ::pthread_create(&thread, &attributes, entry, context);
    }
    if (attributeInitError == 0) {
        ::pthread_attr_destroy(&attributes);
    }
    return error;
}

}  // namespace

ProcessSupervisor::ProcessSupervisor()
    : spawnFunction_([](
          pid_t *pid, const char *path,
          const posix_spawn_file_actions_t *fileActions,
          const posix_spawnattr_t *attributes, char *const arguments[],
          char *const environment[]) {
          return ::posix_spawn(pid, path, fileActions, attributes,
                               arguments, environment);
      }),
      killFunction_([](pid_t pid, int signal) {
          return ::kill(pid, signal);
      }),
      waitPidFunction_([](pid_t pid, int *status, int options) {
          return ::waitpid(pid, status, options);
      }) {}

#ifdef TRYX_NVIDIA_TESTING
ProcessSupervisor::ProcessSupervisor(PosixSpawnFunction spawnFunction)
    : ProcessSupervisor(std::move(spawnFunction), {}, {}) {}

ProcessSupervisor::ProcessSupervisor(
    PosixSpawnFunction spawnFunction, KillFunction killFunction,
    WaitPidFunction waitPidFunction)
    : ProcessSupervisor() {
    if (spawnFunction) {
        spawnFunction_ = std::move(spawnFunction);
    }
    if (killFunction) {
        killFunction_ = std::move(killFunction);
    }
    if (waitPidFunction) {
        waitPidFunction_ = std::move(waitPidFunction);
    }
}
#endif

ProcessAttempt::ProcessAttempt(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

ProcessAttempt::~ProcessAttempt() {
    shutdown();
}

int ProcessAttempt::notificationFd() const {
    return state_ ? state_->resultEventFd : -1;
}

QVector<ProcessEvent> ProcessAttempt::takeEvents() {
    if (!state_) {
        return {};
    }
    drainEventFd(state_->resultEventFd);
    QVector<ProcessEvent> result;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        result.swap(state_->events);
    }
    return result;
}

void ProcessAttempt::requestKill(ProcessOutcome reason) {
    if (!state_ || state_->workerDone.load(std::memory_order_acquire)) {
        return;
    }
    if (reason != ProcessOutcome::Timeout &&
        reason != ProcessOutcome::OutputLimit &&
        reason != ProcessOutcome::IoFailure &&
        reason != ProcessOutcome::Cancelled) {
        reason = ProcessOutcome::Cancelled;
    }
    state_->requestStop(reason, true);
}

void ProcessAttempt::shutdown() {
    if (state_ && !state_->workerDone.load(std::memory_order_acquire)) {
        state_->requestShutdown();
    }
}

bool ProcessAttempt::active() const {
    return state_ &&
        !state_->workerDone.load(std::memory_order_acquire);
}

std::shared_ptr<ProcessAttempt> ProcessSupervisor::start(
    const ProcessSpec &spec, Deadline absoluteDeadline,
    quint64 generation) const {
    UniqueFd resultEvent = createEventFd();
    UniqueFd controlEvent = createEventFd();
    if (!resultEvent.valid() || !controlEvent.valid()) {
        return {};
    }

    auto state = std::make_shared<ProcessAttempt::State>(
        spec, absoluteDeadline, generation, spawnFunction_,
        killFunction_, waitPidFunction_, std::move(resultEvent),
        std::move(controlEvent));
    auto attempt = std::shared_ptr<ProcessAttempt>(
        new ProcessAttempt(state));

    if (!validSpec(spec) || !spawnFunction_ || !killFunction_ ||
        !waitPidFunction_) {
        state->finish(ProcessOutcome::StartFailed);
        return attempt;
    }
    if (!childReapingIsOwned()) {
        state->finishOwnershipLost();
        return attempt;
    }
    if (deadlineExpired(absoluteDeadline)) {
        state->finish(ProcessOutcome::Timeout);
        return attempt;
    }

    auto *deadlineContext = new (std::nothrow) ThreadContext{state};
    if (!deadlineContext) {
        state->finish(ProcessOutcome::StartFailed);
        return attempt;
    }
    int error = startDetachedThread(deadlineThreadMain, deadlineContext);
    if (error != 0) {
        delete deadlineContext;
        state->finish(ProcessOutcome::StartFailed, {}, error);
        return attempt;
    }

    auto *attemptContext = new (std::nothrow) ThreadContext{state};
    if (!attemptContext) {
        state->finish(ProcessOutcome::StartFailed);
        return attempt;
    }
    error = startDetachedThread(attemptThreadMain, attemptContext);
    if (error != 0) {
        delete attemptContext;
        state->finish(ProcessOutcome::StartFailed, {}, error);
    }
    return attempt;
}

}  // namespace tryx::nvidia
