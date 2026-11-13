#include "updater.h"
#include "qa_fence.h"
#include "i18n.h"
#include "platform.h"
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QFileInfo>
#include <QUrl>
static const char *kFeed = "https://github.com/hikariatama/room237/releases/latest/download/qt-update.json";
namespace {
#ifdef ROOM237_QA
QJsonObject fixture() { return QJsonDocument::fromJson(qgetenv("ROOM237_QA_UPDATE_FIXTURE")).object(); }
bool canonical(const QString &path, bool missing = false) {
  if (!QaFence::allows(path, missing) || QDir::cleanPath(path) != QDir::fromNativeSeparators(path)) return false;
  QString ancestor = path, tail;
  while (!QFileInfo::exists(ancestor)) {
    tail = QLatin1Char('/') + QFileInfo(ancestor).fileName() + tail;
    ancestor = QFileInfo(ancestor).absolutePath();
  }
  return QFileInfo(ancestor).canonicalFilePath() + tail == QDir::fromNativeSeparators(path);
}
bool fixtureUrl(const QString &raw) {
  const QUrl url(raw), feed(fixture().value("feed").toString());
  if (!url.isValid() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) return false;
  if (url.scheme() == QLatin1String("file")) return url.host().isEmpty() && canonical(url.toLocalFile(), true);
  return url.scheme() == QLatin1String("http") && url.host() == QLatin1String("127.0.0.1") && url.port() > 0 &&
         url.port() == feed.port() && !url.path().split('/').contains(QStringLiteral(".."));
}
QString siblingPath(const QString &install, const QString &suffix) {
  return QDir(QFileInfo(install).absolutePath()).filePath(QLatin1Char('.') + QFileInfo(install).fileName() + suffix);
}
QStringList fixtureLaunch() {
  const auto f = fixture();
  return {f.value("executable").toString(), QStringLiteral("--root"), f.value("root").toString(),
          QStringLiteral("--profile"), f.value("profile").toString(), QStringLiteral("--legacy-profile"), f.value("legacy").toString()};
}
#else
QJsonObject fixture() { return {}; }
bool canonical(const QString &, bool = false) { return true; }
bool fixtureUrl(const QString &) { return true; }
QStringList fixtureLaunch() { return {}; }
#endif
}
#ifdef ROOM237_QA
bool Updater::qaFixtureAllowed() {
  if (!QaFence::enabled() || !qEnvironmentVariableIsSet("ROOM237_QA_UPDATE_FIXTURE") ||
      !canonical(qEnvironmentVariable("ROOM237_QA_FENCE"))) return false;
  const auto f = fixture();
  for (const auto &key : {"root", "profile", "legacy", "home", "install", "executable", "staging"})
    if (!canonical(f.value(key).toString(), QByteArray(key) == "staging")) return false;
  // An AppImage runs from its own extraction; the installed file it replaces and relaunches is $APPIMAGE itself.
  const auto install = Platform::installInfo();
  const bool image = install.kind == QLatin1String("appimage");
  const QString running = image ? qEnvironmentVariable("APPIMAGE") : QCoreApplication::applicationFilePath();
  if (f.value("home").toString() != qEnvironmentVariable("HOME") ||
      f.value("executable").toString() != running ||
      f.value("install").toString() != install.install ||
      !(image ? f.value("executable").toString() == f.value("install").toString()
              : QaFence::inside(f.value("executable").toString(), f.value("install").toString())) ||
      f.value("staging").toString() != siblingPath(f.value("install").toString(), QStringLiteral(".update-new")) ||
      !canonical(siblingPath(f.value("install").toString(), QStringLiteral(".update-old")), true) ||
      !canonical(siblingPath(f.value("install").toString(), QStringLiteral(".update-failed")), true) ||
      f.value("publicKey").toString().isEmpty() || !fixtureUrl(f.value("feed").toString())) return false;
  const auto args = QCoreApplication::arguments();
  for (const auto &pair : {qMakePair(QStringLiteral("--root"), QStringLiteral("root")), qMakePair(QStringLiteral("--profile"), QStringLiteral("profile")), qMakePair(QStringLiteral("--legacy-profile"), QStringLiteral("legacy"))}) {
    const int i = args.indexOf(pair.first);
    if (i < 1 || i + 1 >= args.size() || args[i + 1] != f.value(pair.second).toString() || args.count(pair.first) != 1) return false;
  }
  for (const auto &leaf : {"updates", "update-plan.json", "update-health", "update-result.json", "room237-update-helper", "room237-update-helper.exe"})
    if (!canonical(QDir(f.value("profile").toString()).filePath(QString::fromLatin1(leaf)), true)) return false;
  return true;
}
bool Updater::qaCoreAllowed(const QString &method, const QJsonObject &params) {
  if (!qaFixtureAllowed()) return false;
  const auto f = fixture();
  if (method == QLatin1String("update_cleanup")) return params.value("install") == f.value("install");
  if (method == QLatin1String("update_check")) return true; // The core verifies the signature and fixture URL.
  const auto p = params.value("plan").toObject();
  if (p.value("install") != f.value("install")) return false;
  if (method == QLatin1String("update_stage")) {
    const QString payload = p.value("payload").toString(), probe = p.value("probe").toString();
    return canonical(payload) && QaFence::inside(payload, QDir(f.value("profile").toString()).filePath("updates")) &&
           probe == Platform::installInfo().probe;
  }
  if (method != QLatin1String("update_launch_helper")) return false;
  QJsonArray launch;
  for (const auto &arg : fixtureLaunch()) launch.append(arg);
  return p.value("staged") == f.value("staging") &&
         p.value("backup").toString() == siblingPath(f.value("install").toString(), QStringLiteral(".update-old")) &&
         p.value("launch").toArray() == launch &&
         p.value("healthFile").toString() == QDir(f.value("profile").toString()).filePath("update-health") &&
         p.value("resultFile").toString() == QDir(f.value("profile").toString()).filePath("update-result.json") &&
         p.value("waitPid").toDouble() == QCoreApplication::applicationPid();
}
#endif
Updater::Updater(CoreClient *core, const QString &profile, QObject *parent) : QObject(parent), core_(core), profile_(profile) {
  periodic_.setInterval(15 * 60 * 1000);
  periodic_.setTimerType(Qt::VeryCoarseTimer);
  connect(&periodic_, &QTimer::timeout, this, [this] { checkNow(false); });
}
QString Updater::feedUrl() const {
  if (QaFence::enabled()) return qaFixtureAllowed() ? fixture().value("feed").toString() : QString();
  const QString override = qEnvironmentVariable("ROOM237_UPDATE_FEED");
  return override.isEmpty() ? QString::fromLatin1(kFeed) : override;
}
QNetworkAccessManager *Updater::network() {
  if (!network_) {
    network_ = new QNetworkAccessManager(this);
    network_->setRedirectPolicy(QaFence::enabled() ? QNetworkRequest::ManualRedirectPolicy : QNetworkRequest::NoLessSafeRedirectPolicy);
    network_->setTransferTimeout(30000);
  }
  return network_;
}
void Updater::start() {
  if (QaFence::enabled() && !qaFixtureAllowed()) return;
  if (!QaFence::enabled() || !QFile::exists(QDir(profile_).filePath(QStringLiteral("update-plan.json"))))
  core_->call(QStringLiteral("update_cleanup"), {{QStringLiteral("install"), Platform::installInfo().install}});
  // The helper records its result after it has seen this process become healthy.
  for (const int delay : {0, 1000, 3000, 6000}) QTimer::singleShot(delay, this, &Updater::reportPreviousResult);
  if (automatic_) {
    QTimer::singleShot(5000, this, [this] { if (automatic_) checkNow(false); });
    periodic_.start();
  }
}
void Updater::setAutomatic(bool automatic) {
  automatic_ = automatic;
  if (automatic) periodic_.start();
  else periodic_.stop();
}
void Updater::reportPreviousResult() {
  if (QaFence::enabled() && !qaFixtureAllowed()) return;
  QFile result(QDir(profile_).filePath(QStringLiteral("update-result.json")));
  if (!result.open(QIODevice::ReadOnly)) return;
  const QString status = QJsonDocument::fromJson(result.readAll()).object().value(QLatin1String("status")).toString();
  previousResult_ = status;
  result.close();
  result.remove();
  if (status == QLatin1String("updated")) emit message(0, t("update.done", {{"version", QStringLiteral(ROOM237_VERSION)}}));
  else if (status == QLatin1String("rolled-back") || status == QLatin1String("failed")) emit message(1, t("update.rolledBack"));
}
void Updater::markHealthyIfUpdating() {
  if (QaFence::enabled() && !qaFixtureAllowed()) return;
  if (QaFence::enabled() && fixture().value("failHealthVersion").toString() == QStringLiteral(ROOM237_VERSION)) return;
  const QDir profile(profile_);
  if (!QFile::exists(profile.filePath(QStringLiteral("update-plan.json")))) return;
  QFile health(profile.filePath(QStringLiteral("update-health")));
  if (health.open(QIODevice::WriteOnly)) health.close();
  QFile::remove(profile.filePath(QStringLiteral("update-plan.json")));
  // The helper records "updated" only after it sees this mark, which can be later than the start-time reads
  // (an AppImage unpacks itself on every launch), so the result is read again relative to the mark.
  for (const int delay : {500, 1500, 3000, 6000}) QTimer::singleShot(delay, this, &Updater::reportPreviousResult);
}
void Updater::fail(const QString &text) {
  state_ = QStringLiteral("failed");
  if (reply_) reply_->abort();
  if (file_) {
    file_->close();
    file_->remove();
    delete file_;
    file_ = nullptr;
  }
  emit failed(text);
}
void Updater::checkNow(bool interactive) {
  if (QaFence::enabled() && !qaFixtureAllowed()) return;
  if (state_ == QLatin1String("checking") || state_ == QLatin1String("downloading") || state_ == QLatin1String("staging") || state_ == QLatin1String("restarting")) return;
  interactive_ = interactive;
  state_ = QStringLiteral("checking");
  if (interactive) emit message(2, t("update.checking"));
  QNetworkRequest request{QUrl(feedUrl())};
  reply_ = network()->get(request);
  connect(reply_, &QNetworkReply::finished, this, [this, reply = reply_] {
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
      state_ = QStringLiteral("idle");
      if (interactive_) emit message(1, t("update.offline"));
      return;
    }
    fetchSignature(reply->readAll());
  });
}
void Updater::fetchSignature(const QByteArray &manifest) {
  if (QaFence::enabled() && (!qaFixtureAllowed() || !fixtureUrl(feedUrl() + QStringLiteral(".minisig")))) return fail(t("update.invalid"));
  QNetworkRequest request{QUrl(feedUrl() + QStringLiteral(".minisig"))};
  reply_ = network()->get(request);
  connect(reply_, &QNetworkReply::finished, this, [this, reply = reply_, manifest] {
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
      state_ = QStringLiteral("idle");
      if (interactive_) emit message(1, t("update.offline"));
      return;
    }
    verify(manifest, reply->readAll());
  });
}
void Updater::verify(const QByteArray &manifest, const QByteArray &signature) {
  core_->call(QStringLiteral("update_check"), {{QStringLiteral("manifest"), QString::fromUtf8(manifest)}, {QStringLiteral("signature"), QString::fromUtf8(signature)}, {QStringLiteral("currentVersion"), QStringLiteral(ROOM237_VERSION)}, {QStringLiteral("platform"), Platform::platformKey()}}, [this](const QJsonValue &value, const QString &error) {
    state_ = QStringLiteral("idle");
    if (!error.isEmpty()) {
      qWarning("update check rejected: %s", qPrintable(error));
      if (interactive_) emit message(1, t("update.invalid"));
      return;
    }
    const QJsonObject result = value.toObject();
    if (!result.value(QLatin1String("available")).toBool()) {
      if (interactive_) emit message(0, t("update.none"));
      emit none();
      return;
    }
    if (QaFence::enabled() && (!qaFixtureAllowed() || !fixtureUrl(result.value("url").toString()))) return fail(t("update.invalid"));
    version_ = result.value(QLatin1String("version")).toString();
    url_ = result.value(QLatin1String("url")).toString();
    sha256_ = result.value(QLatin1String("sha256")).toString();
    kind_ = result.value(QLatin1String("kind")).toString();
    size_ = qint64(result.value(QLatin1String("size")).toDouble());
    state_ = QStringLiteral("available");
    const Platform::InstallInfo install = Platform::installInfo();
    if (!install.unavailableReason.isEmpty() || install.kind != kind_) emit manualOnly(install.unavailableReason, version_);
    else emit available(version_);
  });
}
void Updater::install() {
  if (QaFence::enabled() && !qaFixtureAllowed()) return;
  if (state_ != QLatin1String("available") && state_ != QLatin1String("failed")) return;
  downloadPayload();
}
void Updater::cancel() {
  if (QaFence::enabled() && !qaFixtureAllowed()) return;
  if (state_ != QLatin1String("downloading")) return;
  state_ = QStringLiteral("available");
  if (reply_) {
    reply_->disconnect(this);
    reply_->abort();
    reply_->deleteLater();
  }
  if (file_) {
    file_->close();
    file_->remove();
    delete file_;
    file_ = nullptr;
  }
  emit available(version_);
}
void Updater::downloadPayload() {
  if (QaFence::enabled() && (!qaFixtureAllowed() || !fixtureUrl(url_) || !canonical(QDir(profile_).filePath("updates/payload-" + version_), true))) return fail(t("update.invalid"));
  state_ = QStringLiteral("downloading");
  QDir(profile_).mkpath(QStringLiteral("updates"));
  payloadPath_ = QDir(profile_).filePath(QStringLiteral("updates/payload-") + version_);
  file_ = new QFile(payloadPath_);
  if (!file_->open(QIODevice::WriteOnly)) return fail(t("update.failed"));
  emit downloading();
  QNetworkRequest request{QUrl(url_)};
  reply_ = network()->get(request);
  connect(reply_, &QNetworkReply::readyRead, this, [this] {
    const QByteArray chunk = reply_->readAll();
    if (file_->pos() + chunk.size() > size_ || file_->write(chunk) != chunk.size()) return fail(t("update.invalid"));
  });
  connect(reply_, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64) {
    if (size_ > 0) emit progress(double(received) / size_);
  });
  connect(reply_, &QNetworkReply::finished, this, [this, reply = reply_] {
    reply->deleteLater();
    if (state_ != QLatin1String("downloading")) return;
    if (reply->error() != QNetworkReply::NoError) return fail(t("update.offline"));
    file_->write(reply->readAll());
    file_->close();
    delete file_;
    file_ = nullptr;
    stagePayload();
  });
}
void Updater::stagePayload() {
  state_ = QStringLiteral("staging");
  const Platform::InstallInfo install = Platform::installInfo();
  const QJsonObject plan{{QStringLiteral("payload"), payloadPath_}, {QStringLiteral("sha256"), sha256_}, {QStringLiteral("size"), double(size_)}, {QStringLiteral("kind"), kind_}, {QStringLiteral("install"), install.install}, {QStringLiteral("probe"), install.probe}, {QStringLiteral("version"), version_}};
  core_->call(QStringLiteral("update_stage"), {{QStringLiteral("plan"), plan}}, [this, install](const QJsonValue &value, const QString &error) {
    QFile::remove(payloadPath_);
    if (!error.isEmpty()) {
      qWarning("update stage rejected: %s", qPrintable(error));
      return fail(t("update.invalid"));
    }
    state_ = QStringLiteral("restarting");
    const QJsonObject staged = value.toObject();
    const QDir profile(profile_);
    QStringList launch = install.relaunch;
    // The same profile and root as this run, so an isolated installation stays isolated.
    QStringList extra;
    const QStringList arguments = QCoreApplication::arguments();
    for (int i = 1; i + 1 < arguments.size(); i++) {
      if (arguments[i] == QLatin1String("--profile") || arguments[i] == QLatin1String("--root")) extra << arguments[i] << arguments[i + 1];
    }
    if (QaFence::enabled()) launch = fixtureLaunch();
    else if (!extra.isEmpty()) {
      if (launch.first() == QLatin1String("/usr/bin/open")) launch << QStringLiteral("--args");
      launch << extra;
    }
    QJsonArray launchJson;
    for (const QString &part : launch) launchJson.push_back(part);
    const QJsonObject helper{{QStringLiteral("waitPid"), double(QCoreApplication::applicationPid())}, {QStringLiteral("install"), install.install}, {QStringLiteral("staged"), staged.value(QLatin1String("staged"))}, {QStringLiteral("backup"), staged.value(QLatin1String("backup"))}, {QStringLiteral("launch"), launchJson}, {QStringLiteral("healthFile"), profile.filePath(QStringLiteral("update-health"))}, {QStringLiteral("resultFile"), profile.filePath(QStringLiteral("update-result.json"))}, {QStringLiteral("healthTimeoutSecs"), qEnvironmentVariableIntValue("ROOM237_UPDATE_HEALTH_TIMEOUT") > 0 ? qEnvironmentVariableIntValue("ROOM237_UPDATE_HEALTH_TIMEOUT") : 90}};
    core_->call(QStringLiteral("update_launch_helper"), {{QStringLiteral("plan"), helper}}, [this](const QJsonValue &, const QString &failure) {
      if (!failure.isEmpty()) {
        qWarning("update helper: %s", qPrintable(failure));
        return fail(t("update.failed"));
      }
      emit readyToRestart();
    });
  });
}
