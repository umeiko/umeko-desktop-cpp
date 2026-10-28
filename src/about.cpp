#include "backend.hpp"
#include <QClipboard>
#include <QDesktopServices>
#include "update_proxy.hpp"
#include <QtConcurrent>

AppAbout::AppAbout(Backend *backend, QNetworkAccessManager *transport)
    : QObject(backend), b(backend),
      network(transport ? transport : new QNetworkAccessManager(this)),
      m_automatic(this) {
  auto build = U::readJson(b->root + "/build-info.json", QVariantMap{}).toMap();
  m_minimal = build.contains("bundlePython") && !build["bundlePython"].toBool();
  m_automatic.setInterval(6 * 60 * 60 * 1000);
  connect(&m_automatic, &QTimer::timeout, this, &AppAbout::checkUpdates);
  m_info = {{"name", "Umeko-Desktop"},
            {"version", UMEKO_VERSION},
            {"author", "umeiko"},
            {"projectUrl", "https://github.com/umeiko/umeko-desktop-cpp"},
            {"authorUrl", "https://github.com/umeiko"},
            {"buildPlatform",
             build.value("system", UMEKO_BUILD_SYSTEM).toString() + " · " +
                 build.value("architecture", UMEKO_BUILD_ARCH).toString()},
            {"compiler", UMEKO_COMPILER},
            {"builtAt", build.value("builtAt", "源码运行")},
            {"commit", build.value("commit", "未记录").toString().left(8)},
            {"dirty", build.value("dirty", false)}};
}
void AppAbout::openProject() {
  QDesktopServices::openUrl(QUrl(m_info["projectUrl"].toString()));
}
void AppAbout::openAuthor() {
  QDesktopServices::openUrl(QUrl(m_info["authorUrl"].toString()));
}
void AppAbout::openReleases() {
  QDesktopServices::openUrl(
      QUrl(m_release.value("url", m_info["projectUrl"].toString() + "/releases")
               .toString()));
}
void AppAbout::copyInfo() {
  QStringList lines;
  for (auto key : {"name", "version", "author", "projectUrl", "buildPlatform",
                   "compiler", "builtAt", "commit"})
    lines << QString::fromLatin1(key) + ": " + m_info[key].toString();
  QGuiApplication::clipboard()->setText(lines.join('\n'));
  emit b->toast("已复制项目信息");
}
void AppAbout::checkUpdates() {
  if (m_busy || downloadActive())
    return;
  m_busy = true;
  if (m_release["state"] != "available")
    m_release = {{"state", "checking"},
                 {"message", "正在检查 GitHub 最新正式版本…"}};
  emit changed();
  const QUrl url("https://api.github.com/repos/umeiko/umeko-desktop-cpp/releases/latest");
  auto watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, url] {
    m_updateProxy = watcher->result(); watcher->deleteLater();
    network->setProxy(qtUpdateProxy(m_updateProxy));
    readRelease(url);
  });
  watcher->setFuture(QtConcurrent::run([url] { return updateProxy(url); }));
}
void AppAbout::startAutomaticChecks() {
  if (m_automatic.isActive())
    return;
  m_automatic.start();
  QTimer::singleShot(5000, this, &AppAbout::checkUpdates);
}
void AppAbout::readRelease(QUrl url, bool minimalChannel) {
  if (m_updateProxy.startsWith("https:")) {
    auto process = new QProcess(this);
    auto environment = U::environment(); environment.insert("UMEKO_UPDATE_PROXY", m_updateProxy);
    process->setProcessEnvironment(environment); U::hideConsole(process);
    auto done = std::make_shared<bool>(false);
    connect(process, &QProcess::errorOccurred, this, [this,process,done,minimalChannel](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart && !*done) { *done = true; releaseResult({},0,minimalChannel); process->deleteLater(); }
    });
    connect(process, &QProcess::finished, this, [this,process,done,minimalChannel](int,QProcess::ExitStatus) {
      if (!*done) {
        *done = true; const auto value = QJsonDocument::fromJson(process->readAllStandardOutput().trimmed()).object();
        releaseResult(value["body"].toString().toUtf8(), value["code"].toInt(), minimalChannel);
        if (value["state"] == "error" && m_release["state"] == "error") {
          m_release["message"] = value["message"].toString(); emit changed();
        }
      }
      process->deleteLater();
    });
    process->start(U::node(), {b->root + "/runtime/update-engine.mjs", "metadata", url.toString()});
    return;
  }
  QNetworkRequest request(url);
  request.setRawHeader("User-Agent", "Umeko-Desktop/" UMEKO_VERSION);
  request.setTransferTimeout(20000);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::SameOriginRedirectPolicy);
  auto reply = network->get(request);
  auto certificateFailure = std::make_shared<bool>(false);
  connect(reply, &QNetworkReply::sslErrors, this, [certificateFailure](const QList<QSslError> &errors) { *certificateFailure = !errors.isEmpty(); });
  connect(reply, &QNetworkReply::finished, this, [this, reply, minimalChannel, url, certificateFailure] {
    const auto raw = reply->readAll();
    const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    if (code == 0 && !m_updateProxy.isEmpty()) {
      m_updateProxy.clear(); network->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
      readRelease(url, minimalChannel); return;
    }
    releaseResult(raw, code, minimalChannel);
    if (*certificateFailure && m_release["state"] == "error") {
      m_release["message"] = "无法验证更新服务器证书，请检查系统时间，并确认内网根证书已安装到系统受信任的根证书颁发机构。";
      emit changed();
    }
  });
}
void AppAbout::releaseResult(const QByteArray &raw, int code, bool minimalChannel) {
    m_busy = false;
    m_checked = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm");
    const auto previous = m_release;
    if (code == 404)
      m_release = {{"state", "none"},
                   {"message", "项目尚未发布正式版本"},
                   {"url", m_info["projectUrl"].toString() + "/releases"}};
    else if (code != 200 || raw.size() > 1024 * 1024)
      m_release = {{"state", "error"},
                   {"message", "检查失败，请检查网络或稍后重试"}};
    else {
      auto release = QJsonDocument::fromJson(raw).object();
      auto tag = release["tag_name"].toString();
      auto match = QRegularExpression(
                       minimalChannel ? "^v?(\\d+)\\.(\\d+)\\.(\\d+)-minimal$"
                                      : "^v?(\\d+)\\.(\\d+)\\.(\\d+)$")
                       .match(tag);
      if (!match.hasMatch() || release["draft"].toBool() ||
          (!minimalChannel && release["prerelease"].toBool()))
        m_release = {{"state", "error"}, {"message", "发布版本信息无效"}};
      else {
        auto remote = QVersionNumber::fromString(
                 tag.startsWith('v') ? tag.mid(1) : tag),
             current = QVersionNumber::fromString(UMEKO_VERSION);
        int compare = QVersionNumber::compare(remote, current);
        if (compare > 0 && m_minimal && !minimalChannel) {
          m_busy = true;
          readRelease(QUrl("https://api.github.com/repos/umeiko/"
                           "umeko-desktop-cpp/releases/tags/" +
                           tag + "-minimal"),
                      true);
          return;
        }
        auto url = release["html_url"].toString();
        if (!url.startsWith(
                "https://github.com/umeiko/umeko-desktop-cpp/releases/"))
          url = m_info["projectUrl"].toString() + "/releases";
        m_release = {{"state", compare > 0    ? "available"
                               : compare == 0 ? "current"
                                              : "ahead"},
                     {"message", compare > 0    ? "新版本 " + tag + " 可下载"
                                 : compare == 0 ? "当前已是最新正式版本"
                                                : "当前构建比正式版更新"},
                     {"version", tag},
                     {"notes", release["body"].toString()},
                     {"url", url}};
        QString family;
#ifdef Q_OS_WIN
        family = "windows";
#elif defined(Q_OS_MACOS)
        family = "macos";
#else
        family = "linux";
#endif
        const auto cpu = QSysInfo::buildCpuArchitecture();
        const auto target = family + (cpu == "arm64"    ? "-arm64"
                                      : cpu == "x86_64" ? "-x64"
                                                        : "-unsupported");
        const auto name = "UmekoCpp-" +
                          (tag.startsWith('v') ? tag : "v" + tag) + "-" +
                          target + (family == "linux" ? ".tar.gz" : ".zip");
        QVariantMap asset;
        int matches = 0;
        for (auto item : release["assets"].toArray()) {
          auto candidate = item.toObject();
          const auto downloadUrl = m_info["projectUrl"].toString() +
                                   "/releases/download/" + tag + "/" + name;
          const auto size = candidate["size"].toVariant().toLongLong();
          const auto digest = candidate["digest"].toString();
          if (candidate["name"] == name &&
              candidate["browser_download_url"] == downloadUrl && size > 0 &&
              size <= 2LL * 1024 * 1024 * 1024 &&
              QRegularExpression("^sha256:[a-f0-9]{64}$")
                  .match(digest)
                  .hasMatch()) {
            asset = {{"name", name},
                     {"url", downloadUrl},
                     {"sha256", digest.mid(7)},
                     {"size", size}};
            ++matches;
          }
        }
        if (matches == 1)
          m_release["asset"] = asset;
        if (previous["asset"].toMap()["name"] != asset["name"])
          m_download.clear();
      }
    }
    const auto outcome = m_release;
    const bool ok = outcome["state"] != "error";
    b->endControlWork(b->controlAboutWork.take("updates.check"), ok ? QVariantMap{{"ok", true}, {"value", outcome}} : QVariantMap{{"ok", false}, {"error", outcome["message"]}});
    if (m_release["state"] == "error" && previous["state"] == "available")
      m_release = previous;
    emit changed();

}
bool AppAbout::downloadActive() const {
  return m_downloadProcess || m_download["state"] == "downloading" || m_download["state"] == "verifying";
}
void AppAbout::consumeDownload(QProcess *process) {
  if (m_downloadProcess != process) return;
  m_downloadBuffer += process->readAllStandardOutput();
  while (m_downloadBuffer.contains('\n')) {
    const auto end = m_downloadBuffer.indexOf('\n');
    auto value = QJsonDocument::fromJson(m_downloadBuffer.left(end)).toVariant().toMap();
    m_downloadBuffer.remove(0, end + 1);
    if (value.isEmpty()) continue;
    value["progress"] = qBound(0, qRound(value["progress"].toDouble() * 100), 100);
    if (!value.contains("route")) value["route"] = m_updateProxy.isEmpty() ? "直连" : "系统代理";
    value["name"] = m_downloadAsset["name"];
    value["version"] = m_downloadVersion;
    if (value.contains("archive")) value["path"] = value["archive"];
    m_download = value;
    emit changed();
  }
}
void AppAbout::downloadUpdate() {
  if (m_busy || downloadActive()) return;
  const auto asset = m_release["asset"].toMap();
  const auto name = asset["name"].toString();
  if (asset.isEmpty() || !QRegularExpression("^[A-Za-z0-9][A-Za-z0-9._-]{0,199}$").match(name).hasMatch()) {
    emit b->toast("该版本没有适合当前平台的更新包，请查看发布页"); return;
  }
  auto directory = m_downloadDirectory.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) : m_downloadDirectory;
  if (directory.isEmpty()) directory = QDir::homePath() + "/Downloads";
  directory += "/Umeko";
  const auto jobs = b->userDataDirectory() + "/updates/downloads";
  if (!QDir().mkpath(directory) || !QDir().mkpath(jobs)) {
    m_download = {{"state","error"},{"message","无法保存更新包，请检查下载目录权限"}}; emit changed(); return;
  }
  m_downloadAsset = asset; m_downloadVersion = m_release["version"].toString();
  const auto file = jobs + "/" + asset["sha256"].toString() + ".json";
  const QVariantMap job{{"repo","umeko-desktop-cpp"},{"version",m_downloadVersion},{"cache",directory},
    {"asset",QVariantMap{{"name",name},{"url",asset["url"]},{"sha256",asset["sha256"]},{"bytes",asset["size"]}}}};
  if (!U::writeJson(file, job)) {
    m_download = {{"state","error"},{"message","无法保存下载任务"}}; emit changed(); return;
  }
  m_downloadBuffer.clear();
  m_download = {{"state","downloading"},{"progress",m_download.value("progress",0)},{"name",name},{"message","正在连接下载服务器…"}};
  auto process = new QProcess(this); m_downloadProcess = process;
  U::hideConsole(process);
  connect(process,&QProcess::readyReadStandardOutput,this,[this,process]{consumeDownload(process);});
  connect(process,&QProcess::readyReadStandardError,this,[process]{process->readAllStandardError();});
  connect(process,&QProcess::errorOccurred,this,[this,process](QProcess::ProcessError error){
    if(error != QProcess::FailedToStart || m_downloadProcess != process) return;
    m_downloadProcess.clear(); m_download = {{"state","error"},{"message","更新下载引擎无法启动，请检查内嵌 Node.js"}};
    process->deleteLater(); emit changed();
  });
  connect(process,&QProcess::finished,this,[this,process](int,QProcess::ExitStatus){
    consumeDownload(process);
    if(m_downloadProcess == process) {
      m_downloadProcess.clear();
      if(m_download["state"] != "ready" && m_download["state"] != "paused" && m_download["state"] != "error") {
        m_download["state"]="paused"; m_download["message"]="下载已暂停，进度已保存";
      }
      emit changed(); emit b->toast(m_download["message"].toString());
    }
    process->deleteLater();
  });
  auto watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [this,watcher,process,file] {
    const auto address = watcher->result(); watcher->deleteLater();
    if (m_downloadProcess != process) return;
    m_updateProxy = address;
    auto environment = U::environment(); environment.insert("UMEKO_UPDATE_PROXY", address);
    process->setProcessEnvironment(environment);
    process->start(U::node(), {b->root + "/runtime/update-engine.mjs", "download", file});
  });
  watcher->setFuture(QtConcurrent::run([url=QUrl(asset["url"].toString())] { return updateProxy(url); }));
  emit changed();
}
void AppAbout::showDownloadedUpdate() {
  if (m_download["state"] == "ready")
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QFileInfo(m_download["path"].toString()).absolutePath()));
}

AppAbout::~AppAbout() { closeDownload(); }
void AppAbout::closeDownload() {
  if (!m_downloadProcess) return;
  auto process = m_downloadProcess.data();
  if (process->state() == QProcess::NotRunning) { m_downloadProcess.clear(); return; }
  process->write("pause\n");
  if (!process->waitForFinished(1500)) { process->kill(); process->waitForFinished(1000); }
}
