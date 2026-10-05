#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

#include <chrono>
#include <functional>
#include <memory>
#include <spawn.h>

namespace tryx::nvidia {

enum class ProcessOutcome {
    Success,
    StartFailed,
    ExitFailure,
    Timeout,
    OutputLimit,
    IoFailure,
    OwnershipLost,
    Cancelled,
    Cleanup,
};

struct ProcessSpec {
    QString executable;
    QStringList arguments;
    qsizetype stdoutLimit = 64 * 1024;
    qsizetype stderrLimit = 16 * 1024;
};

struct ProcessEvent {
    ProcessOutcome outcome = ProcessOutcome::StartFailed;
    quint64 generation = 0;
    QByteArray stdoutData;
    int exitCode = -1;
    bool terminal = false;
};

using PosixSpawnFunction = std::function<int(
    pid_t *, const char *, const posix_spawn_file_actions_t *,
    const posix_spawnattr_t *, char *const[], char *const[])>;
using KillFunction = std::function<int(pid_t, int)>;
using WaitPidFunction = std::function<pid_t(pid_t, int *, int)>;

class ProcessAttemptHandle {
public:
    virtual ~ProcessAttemptHandle() = default;

    virtual int notificationFd() const = 0;
    virtual QVector<ProcessEvent> takeEvents() = 0;
    virtual void requestKill(
        ProcessOutcome reason = ProcessOutcome::Cancelled) = 0;
    virtual void shutdown() = 0;
    virtual bool active() const = 0;
};

class ProcessAttempt final : public ProcessAttemptHandle {
public:
    struct State;

    ~ProcessAttempt() override;

    int notificationFd() const override;
    QVector<ProcessEvent> takeEvents() override;
    void requestKill(
        ProcessOutcome reason = ProcessOutcome::Cancelled) override;
    void shutdown() override;
    bool active() const override;

private:
    explicit ProcessAttempt(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;

    friend class ProcessSupervisor;
};

class ProcessSupervisor final {
public:
    ProcessSupervisor();
#ifdef TRYX_NVIDIA_TESTING
    explicit ProcessSupervisor(PosixSpawnFunction spawnFunction);
    ProcessSupervisor(PosixSpawnFunction spawnFunction,
                      KillFunction killFunction,
                      WaitPidFunction waitPidFunction);
#endif

    std::shared_ptr<ProcessAttempt> start(
        const ProcessSpec &spec,
        std::chrono::steady_clock::time_point absoluteDeadline,
        quint64 generation) const;

private:
    PosixSpawnFunction spawnFunction_;
    KillFunction killFunction_;
    WaitPidFunction waitPidFunction_;
};

}  // namespace tryx::nvidia
