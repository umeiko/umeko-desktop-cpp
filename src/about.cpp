#include "backend.hpp"
#include <QClipboard>
#include <QDesktopServices>

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
  if (m_busy || m_download["state"] == "downloading")
    return;
  m_busy = true;
  if (m_release["state"] != "available")
    m_release = {{"state", "checking"},
                 {"message", "正在检查 GitHub 最新正式版本…"}};
  emit changed();
  readRelease(QUrl(
      "https://api.github.com/repos/umeiko/umeko-desktop-cpp/releases/latest"));
}
void AppAbout::startAutomaticChecks() {
  if (m_automatic.isActive())
    return;
  m_automatic.start();
  QTimer::singleShot(5000, this, &AppAbout::checkUpdates);
}
void AppAbout::readRelease(QUrl url, bool minimalChannel) {
  QNetworkRequest request(url);
  request.setRawHeader("User-Agent", "Umeko-Desktop/" UMEKO_VERSION);
  request.setTransferTimeout(20000);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::SameOriginRedirectPolicy);
  auto reply = network->get(request);
  connect(reply, &QNetworkReply::finished, this, [this, reply, minimalChannel] {
    m_busy = false;
    m_checked = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm");
    int code =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    auto raw = reply->readAll();
    reply->deleteLater();
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
    if (m_release["state"] == "error" && previous["state"] == "available")
      m_release = previous;
    emit changed();
  });
}
void AppAbout::downloadUpdate() {
  if (m_busy || m_download["state"] == "downloading")
    return;
  const auto asset = m_release["asset"].toMap();
  const auto name = asset["name"].toString();
  if (asset.isEmpty() ||
      !QRegularExpression("^[A-Za-z0-9][A-Za-z0-9._-]{0,199}$")
           .match(name)
           .hasMatch()) {
    emit b->toast("该版本没有适合当前平台的更新包，请查看发布页");
    return;
  }
  auto directory =
      m_downloadDirectory.isEmpty()
          ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
          : m_downloadDirectory;
  if (directory.isEmpty())
    directory = QDir::homePath() + "/Downloads";
  directory += "/Umeko";
  if (!QDir().mkpath(directory)) {
    m_download = {{"state", "error"},
                  {"message", "无法保存更新包，请检查下载目录权限"}};
    emit changed();
    return;
  }
  QString path = directory + "/" + name;
  int number = 1;
  while (QFileInfo::exists(path) || QFileInfo(path).isSymLink())
    path = directory + "/" + QFileInfo(name).completeBaseName() + "-" +
           QString::number(number++) + "." + QFileInfo(name).suffix();
  m_downloadFile = std::make_unique<QSaveFile>(path);
  m_download = {{"state", "downloading"},
                {"progress", 0},
                {"name", name},
                {"message", "正在下载更新包…"}};
  if (!m_downloadFile->open(QIODevice::WriteOnly)) {
    m_downloadFile.reset();
    m_download = {{"state", "error"},
                  {"message", "无法保存更新包，请检查下载目录权限"}};
    emit changed();
    return;
  }
  m_downloadHash.reset();
  m_received = 0;
  m_downloadError.clear();
  QNetworkRequest request(QUrl(asset["url"].toString()));
  request.setRawHeader("User-Agent", "Umeko-Desktop/" UMEKO_VERSION);
  request.setRawHeader("Accept-Encoding", "identity");
  request.setTransferTimeout(30000);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  auto reply = network->get(request);
  m_downloadReply = reply;
  reply->setReadBufferSize(1024 * 1024);
  const auto consume = [this, reply, asset] {
    const auto bytes = reply->readAll();
    m_received += bytes.size();
    if (m_received > asset["size"].toLongLong() ||
        m_downloadFile->write(bytes) != bytes.size()) {
      m_downloadError = "更新包大小或保存状态异常，请重新下载";
      reply->abort();
      return;
    }
    m_downloadHash.addData(bytes);
    const int progress = int(m_received * 100 / asset["size"].toLongLong());
    if (m_download["progress"].toInt() != progress) {
      m_download["progress"] = progress;
      emit changed();
    }
  };
  connect(reply, &QIODevice::readyRead, this, consume);
  connect(reply, &QNetworkReply::finished, this,
          [this, reply, asset, path, consume] {
            if (reply->bytesAvailable() && m_downloadError.isEmpty())
              consume();
            const bool valid = m_downloadError.isEmpty() &&
                               reply->error() == QNetworkReply::NoError &&
                               reply->url().scheme() == "https" &&
                               m_received == asset["size"].toLongLong() &&
                               m_downloadHash.result().toHex() ==
                                   asset["sha256"].toByteArray();
            const bool saved = valid && m_downloadFile->commit();
            m_downloadFile.reset();
            m_downloadReply.clear();
            reply->deleteLater();
            m_download =
                saved ? QVariantMap{{"state", "ready"},
                                    {"path", path},
                                    {"name", asset["name"]},
                                    {"progress", 100},
                                    {"message", "更新包已下载"}}
                      : QVariantMap{
                            {"state", "error"},
                            {"message", "更新包下载或校验失败，请重新下载"}};
            emit changed();
            emit b->toast(m_download["message"].toString());
          });
  emit changed();
}
void AppAbout::showDownloadedUpdate() {
  if (m_download["state"] == "ready")
    QDesktopServices::openUrl(QUrl::fromLocalFile(
        QFileInfo(m_download["path"].toString()).absolutePath()));
}
