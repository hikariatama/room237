#pragma once
#include "fs.h"

#include <QIODevice>
#include <QStringList>
#include <condition_variable>
#include <mutex>

namespace core {

// QA runs of qt/tests. Only builds with the QA driver honour ROOM237_QA_PORT.
bool qaRun();
bool qaPathAllowed(const QString &path);

extern std::mutex mediaMoveLock;
extern std::mutex storeWriteLock;

// Shared budget across imports, background workers and foreground requests.
class DecodePermit {
public:
  DecodePermit();
  ~DecodePermit();
  DecodePermit(const DecodePermit &) = delete;
  DecodePermit &operator=(const DecodePermit &) = delete;
};

// Bounded counting semaphore; held for the lifetime of a Permit.
class Budget {
public:
  explicit Budget(int limit) : limit_(limit) {}
  void acquire();
  void release();
  class Permit {
  public:
    explicit Permit(Budget &budget) : budget_(&budget) { budget.acquire(); }
    ~Permit() { if (budget_) budget_->release(); }
    Permit(Permit &&other) noexcept : budget_(other.budget_) { other.budget_ = nullptr; }
    Permit(const Permit &) = delete;
    Permit &operator=(const Permit &) = delete;
  private:
    Budget *budget_;
  };
private:
  std::mutex mutex_;
  std::condition_variable available_;
  int used_ = 0, limit_;
};

struct ProcessResult {
  int exitCode = -1;
  bool success = false;
  QByteArray stdoutBytes, stderrBytes;
};
QString ffmpegPath();
// Runs to completion or kills the process at the timeout ("process timed out").
ProcessResult runProcess(const QString &program, const QStringList &arguments, int timeoutMs);
// As above, and fails with the process's standard error when it does not exit successfully.
ProcessResult runChecked(const QString &program, const QStringList &arguments, int timeoutMs);
// Streams `input` to the process's standard input (FFmpeg reads "pipe:0"), so plaintext never touches the disk.
ProcessResult runWithInput(const QString &program, const QStringList &arguments, QIODevice &input, int timeoutMs);
struct FfmpegSettings;
QStringList ffmpegTuning(bool isVideo);  // -hwaccel and -threads from the current settings.
QStringList ffmpegTuning(bool isVideo, const FfmpegSettings &settings);
// ffmpeg-sidecar's FfmpegCommand defaults, which every earlier preview command carried.
inline QStringList ffmpegCommandPrefix() { return {QStringLiteral("-loglevel"), QStringLiteral("level+info")}; }
int ffmpegTimeoutMs();

void setLowPriorityCurrentThread();
QString fileManagerName();
QString canonicalizeDirectory(const QString &directory);

}  // namespace core
