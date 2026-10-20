#include "backend.hpp"
#include <QClipboard>
#include <QDesktopServices>

AppAbout::AppAbout(Backend *backend)
    : QObject(backend), b(backend), network(this) {
  auto build = U::readJson(b->root + "/build-info.json", QVariantMap{}).toMap();
  m_info = {{"name", "Umeko C++"},
            {"version", UMEKO_VERSION},
            {"author", "umeiko"},
            {"projectUrl", "https://github.com/umeiko/umeko-desktop-cpp"},
            {"authorUrl", "https://github.com/umeiko"},
            {"buildPlatform",
             build.value("system", UMEKO_BUILD_SYSTEM).toString() + " · " +
                 build.value("architecture", UMEKO_BUILD_ARCH).toString()},
            {"buildType", build.isEmpty() ? "源码运行" : "C++ 原生打包版"},
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
                   "buildType", "builtAt", "commit"})
    lines << QString::fromLatin1(key) + ": " + m_info[key].toString();
  QGuiApplication::clipboard()->setText(lines.join('\n'));
  emit b->toast("已复制项目信息");
}
void AppAbout::checkUpdates() {
  if (m_busy)
    return;
  m_busy = true;
  m_release = {{"state", "checking"},
               {"message", "正在检查 GitHub 最新正式版本…"}};
  emit changed();
  QNetworkRequest request(QUrl(
      "https://api.github.com/repos/umeiko/umeko-desktop-cpp/releases/latest"));
  request.setRawHeader("User-Agent", "UmekoCpp/0.2.0");
  request.setTransferTimeout(20000);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::SameOriginRedirectPolicy);
  auto reply = network.get(request);
  connect(reply, &QNetworkReply::finished, this, [this, reply] {
    m_busy = false;
    m_checked = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm");
    int code =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    auto raw = reply->readAll();
    reply->deleteLater();
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
      auto match =
          QRegularExpression("^v?(\\d+)\\.(\\d+)\\.(\\d+)$").match(tag);
      if (!match.hasMatch())
        m_release = {{"state", "error"}, {"message", "发布版本信息无效"}};
      else {
        auto remote = QVersionNumber::fromString(
                 tag.startsWith('v') ? tag.mid(1) : tag),
             current = QVersionNumber::fromString(UMEKO_VERSION);
        int compare = QVersionNumber::compare(remote, current);
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
                     {"url", url}};
      }
    }
    emit changed();
  });
}
