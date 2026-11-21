#include "util.h"
#include "file_access.h"

#include "settings.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QIODevice>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <thread>
#ifndef Q_OS_WIN
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <pthread.h>
#include <sys/qos.h>
#else
#include <dlfcn.h>
#include <sys/resource.h>
#endif

namespace core {

bool qaRun() {
#if defined(ROOM237_QA)
  return qEnvironmentVariableIsSet("ROOM237_QA_PORT");
#else
  return false;
#endif
}

bool qaPathAllowed(const QString &path) {
  static const std::optional<QString> fence = []() -> std::optional<QString> {
    if (!qaRun()) return std::nullopt;
    const QString value = qEnvironmentVariable("ROOM237_QA_FENCE");
    if (value.isEmpty()) return std::nullopt;
    return tryCanonical(value);
  }();
  if (!fence) return true;
  const auto real = tryCanonical(path);
  return !real || path::startsWith(*real, *fence);
}

std::mutex mediaMoveLock;
std::mutex storeWriteLock;

namespace {
std::mutex decodeMutex;
// Never destroyed, like every wait point of the detached workers.
std::condition_variable &decodeAvailable = *new std::condition_variable;
int decoders = 0;
}  // namespace

DecodePermit::DecodePermit() {
  std::unique_lock lock(decodeMutex);
  decodeAvailable.wait(lock, [] { return decoders < 4; });
  ++decoders;
}
DecodePermit::~DecodePermit() {
  {
    std::lock_guard lock(decodeMutex);
    --decoders;
  }
  decodeAvailable.notify_one();
}

void Budget::acquire() {
  std::unique_lock lock(mutex_);
  available_.wait(lock, [this] { return used_ < limit_; });
  ++used_;
}
void Budget::release() {
  {
    std::lock_guard lock(mutex_);
    --used_;
  }
  available_.notify_one();
}

QString ffmpegPath() {
  static const QString found = [] {
    const QString dir = QCoreApplication::instance() ? QCoreApplication::applicationDirPath() : QString();
#ifdef Q_OS_WIN
    const QString candidate = path::join(dir, QStringLiteral("ffmpeg.exe"));
#else
    const QString candidate = path::join(dir, QStringLiteral("ffmpeg"));
#endif
    if (!dir.isEmpty() && isFile(candidate)) return candidate;
    return QStringLiteral("ffmpeg");
  }();
  return found;
}

namespace {
constexpr qint64 kDiagnosticBytes = 4 * 1024 * 1024;
ProcessResult collectProcess(const QString &program, const QStringList &arguments, QIODevice *input, int timeoutMs,
                             qint64 stdoutLimit, bool binaryOutput, int startTimeoutMs = 30000) {
  if (stdoutLimit < 0) fail(QStringLiteral("Missing process output budget"));
  QProcess process;
#ifdef Q_OS_WIN
  process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
#else
  const auto descriptors = FileAccess::inheritedDecoderDescriptors();
  if (!descriptors.empty()) process.setChildProcessModifier([descriptors] {
    // QProcess invokes this after fork and before exec. Only async-signal-safe fcntl calls are used here.
    for (int fd : descriptors) if (fcntl(fd, F_SETFD, 0) < 0) _exit(127);
  });
#endif
  QByteArray out, err;
  bool overflow = false;
  auto drain = [&](QProcess::ProcessChannel channel, QByteArray &bytes, qint64 limit, bool binary) {
    process.setReadChannel(channel);
    while (process.bytesAvailable()) {
      const QByteArray chunk = process.read(65536);
      if (chunk.isEmpty()) break;
      const qint64 remaining = std::max<qint64>(0, limit - bytes.size());
      if (binary && chunk.size() > remaining) {
        overflow = true;
        process.kill();
      }
      if (!overflow || !binary) bytes.append(chunk.constData(), qsizetype(std::min<qint64>(remaining, chunk.size())));
    }
  };
  auto stdoutReady = [&] { drain(QProcess::StandardOutput, out, stdoutLimit, binaryOutput); };
  auto stderrReady = [&] { drain(QProcess::StandardError, err, kDiagnosticBytes, false); };
  // Direct slots also run inside waitForStarted/Finished/BytesWritten, without a GUI event loop. Never retain a
  // whole readAll() result before slicing it. Diagnostics keep their existing first-4-MiB contract.
  QObject::connect(&process, &QProcess::readyReadStandardOutput, &process, stdoutReady, Qt::DirectConnection);
  QObject::connect(&process, &QProcess::readyReadStandardError, &process, stderrReady, Qt::DirectConnection);
  auto disconnectReaders = qScopeGuard([&process] { process.disconnect(); });
  process.setProgram(program);
  process.setArguments(arguments);
  if (!input) process.setStandardInputFile(QProcess::nullDevice());
  process.start();
  if (!process.waitForStarted(qMax(1, startTimeoutMs))) fail(process.errorString());
  QElapsedTimer clock;
  clock.start();
  bool closed = !input;
  while (process.state() != QProcess::NotRunning && !overflow) {
    const qint64 remaining = qint64(qMax(1, timeoutMs)) - clock.elapsed();
    if (remaining <= 0) {
      process.kill();
      process.waitForFinished(5000);
      fail(QStringLiteral("process timed out"));
    }
    if (input && !closed && process.bytesToWrite() < (1 << 20)) {
      const QByteArray chunk = input->read(256 * 1024);
      if (!chunk.isEmpty() && process.write(chunk) != chunk.size()) {
        process.kill(); process.waitForFinished(5000); fail(QStringLiteral("Cannot write process input"));
      }
      if (chunk.isEmpty() || input->atEnd()) { process.closeWriteChannel(); closed = true; }
    }
    process.waitForFinished(int(std::min<qint64>(remaining, input && !closed ? 10 : 50)));
    stdoutReady();
    stderrReady();
  }
  if (overflow) { process.kill(); process.waitForFinished(5000); }
  stdoutReady();
  stderrReady();
  if (overflow) fail(QStringLiteral("Process binary output exceeds its consumer budget"));
  ProcessResult result;
  result.exitCode = process.exitCode();
  result.success = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
  result.stdoutBytes = std::move(out);
  result.stderrBytes = std::move(err);
  return result;
}
}  // namespace

ProcessResult runProcess(const QString &program, const QStringList &arguments, int timeoutMs) {
  return collectProcess(program, arguments, nullptr, timeoutMs, kDiagnosticBytes, false);
}
ProcessResult runBinaryProcess(const QString &program, const QStringList &arguments, int timeoutMs,
                               qint64 stdoutLimit, QIODevice *input, int startTimeoutMs) {
  return collectProcess(program, arguments, input, timeoutMs, stdoutLimit, true, startTimeoutMs);
}
ProcessResult runWithInput(const QString &program, const QStringList &arguments, QIODevice &input, int timeoutMs, qint64 stdoutLimit) {
  return runBinaryProcess(program, arguments, timeoutMs, stdoutLimit, &input);
}

ProcessResult runChecked(const QString &program, const QStringList &arguments, int timeoutMs) {
  ProcessResult result = runProcess(program, arguments, timeoutMs);
  if (!result.success) fail(QString::fromUtf8(result.stderrBytes));
  return result;
}

static bool hardwareDecodingUsable() {
#if defined(Q_OS_LINUX)
  // Static Linux FFmpeg builds load VAAPI lazily and abort when libva is missing.
  static const bool usable = [] {
    for (const char *name : {"libva.so.2", "libva-drm.so.2"}) {
      void *handle = dlopen(name, RTLD_LAZY);
      if (!handle) return false;
      dlclose(handle);
    }
    return true;
  }();
  return usable;
#else
  return true;
#endif
}

QStringList ffmpegTuning(bool isVideo) { return ffmpegTuning(isVideo, readSettings().ffmpeg); }
QStringList ffmpegTuning(bool isVideo, const FfmpegSettings &ffmpeg) {
  QStringList args;
  QString hwaccel = ffmpeg.hwaccel.toLower();
  if (!hardwareDecodingUsable()) hwaccel = QStringLiteral("none");
  if (isVideo && hwaccel != QLatin1String("none")) args << QStringLiteral("-hwaccel") << hwaccel;
  args << QStringLiteral("-threads") << QString::number(ffmpeg.resolvedThreads());
  return args;
}

int ffmpegTimeoutMs() { return int(readSettings().ffmpeg.timeoutSecs * 1000); }

void setLowPriorityCurrentThread() {
#if defined(Q_OS_MACOS)
  pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(Q_OS_WIN)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
  // Linux applies a nice value to the calling thread only.
  setpriority(PRIO_PROCESS, 0, 19);
#endif
}

QString fileManagerName() {
#if defined(Q_OS_MACOS)
  return QStringLiteral("Finder");
#elif defined(Q_OS_WIN)
  return QStringLiteral("File Explorer");
#else
  QString hint = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
  if (hint.isEmpty()) hint = qEnvironmentVariable("DESKTOP_SESSION");
  hint = hint.toLower();
  static const std::pair<const char *, const char *> hints[] = {{"kde", "Dolphin"}, {"plasma", "Dolphin"}, {"gnome", "GNOME Files"}, {"xfce", "Thunar"}, {"lxqt", "PCManFM-Qt"}, {"lxde", "PCManFM"}, {"cinnamon", "Nemo"}, {"pantheon", "Pantheon Files"}, {"mate", "Caja"}};
  for (const auto &[needle, name] : hints)
    if (hint.contains(QLatin1String(needle))) return QString::fromLatin1(name);
  static const std::pair<const char *, const char *> processes[] = {{"dolphin", "Dolphin"}, {"nautilus", "GNOME Files"}, {"nemo", "Nemo"}, {"thunar", "Thunar"}, {"pcmanfm-qt", "PCManFM-Qt"}, {"pcmanfm", "PCManFM"}, {"konqueror", "Konqueror"}, {"caja", "Caja"}};
  try {
    const QString out = QString::fromUtf8(runProcess(QStringLiteral("ps"), {QStringLiteral("-e"), QStringLiteral("-o"), QStringLiteral("comm")}, 5000).stdoutBytes).toLower();
    for (const auto &[process, name] : processes)
      if (out.contains(QLatin1String(process))) return QString::fromLatin1(name);
  } catch (const Error &) {
  }
  return QStringLiteral("File Manager");
#endif
}

QString canonicalizeDirectory(const QString &directory) {
  const QString real = canonical(directory);
  if (!isDir(real)) fail(QStringLiteral("Expected a directory"));
  return real;
}

}  // namespace core
