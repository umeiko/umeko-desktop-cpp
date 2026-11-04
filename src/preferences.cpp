#include "backend.hpp"
#include <cmath>
#include <QFontDatabase>
#include <QGuiApplication>
#include <stdexcept>
#include <QSslCertificate>
QStringList Backend::reasoningLevelsFor(std::shared_ptr<Session> session) const {
  for (const auto &p : store.data["providers"].toList()) if (p.toMap()["id"] == session->provider)
    for (const auto &m : p.toMap()["models"].toList()) if (m.toMap()["alias"] == session->model)
      return U::reasoningLevels(m.toMap());
  return U::reasoningLevels({});
}
void Backend::reconcileReasoning(std::shared_ptr<Session> session) {
  const auto value = U::reconcileEffort(session->reasoningEffort, reasoningLevelsFor(session));
  if (value == session->reasoningEffort) return;
  session->reasoningEffort = value;
  if (!session->id.isEmpty()) setMetadata(session->id, {{"reasoningEffort", value}});
  if (session == view) {
    auto ui = store.data["ui"].toMap(); ui["reasoningEffort"] = value; store.data["ui"] = ui;
    emit reasoningChanged();
  }
}
bool Backend::setReasoningEffort(QString value) {
  if (value != "off" && !reasoningLevels().contains(value))
    return false;
  view->reasoningEffort = value;
  auto ui = store.data["ui"].toMap();
  ui["reasoningEffort"] = value;
  store.data["ui"] = ui;
  if (!view->id.isEmpty())
    setMetadata(view->id, {{"reasoningEffort", value}});
  store.save();
  writePi(view, {{"type", "reasoning_effort"}, {"value", value}});
  emit reasoningChanged();
  return true;
}

void Backend::setLanguage(QString language) {
  if (!QStringList{"zh", "en"}.contains(language) ||
      language == this->language())
    return;
  auto ui = store.data["ui"].toMap();
  ui["language"] = language;
  store.data["ui"] = ui;
  store.save();
  emit appearanceChanged();
}
void Backend::setFontScale(double scale) {
  if (!std::isfinite(scale))
    return;
  scale = qRound(qBound(0.3, scale, 2.0) * 100) / 100.;
  if (scale == fontScale())
    return;
  auto ui = store.data["ui"].toMap();
  ui["fontScale"] = scale;
  store.data["ui"] = ui;
  emit typographyChanged();
  appearanceSave.start();
}
QStringList Backend::availableFonts() const {
  static const auto result=[] {
    const auto installed=QFontDatabase::families();QStringList names{"系统默认"};
    for(const auto &family:QStringList{"Noto Sans CJK SC","Noto Sans SC","Microsoft YaHei UI","WenQuanYi Micro Hei","WenQuanYi Zen Hei","PingFang SC","SimSun","sans-serif","serif"})
      if(installed.contains(family) || family=="sans-serif" || family=="serif")names<<family;
    return names;
  }();return result;
}
void Backend::setFontFamily(QString family) {
  if(family=="系统默认")family.clear();
  if(family.size()>128 || (!family.isEmpty() && family!="sans-serif" && family!="serif" && !QFontDatabase::families().contains(family)))return;
  if(family==fontFamily())return;
  store.data["fontFamily"]=family;
  auto font=family.isEmpty() ? QFontDatabase::systemFont(QFontDatabase::GeneralFont) : QGuiApplication::font();
#ifdef Q_OS_WIN
  if(family.isEmpty())font=QFont("Microsoft YaHei UI",10);
#endif
  if(!family.isEmpty())font.setFamily(family);
  QGuiApplication::setFont(font);emit typographyChanged();emit changed();appearanceSave.start();
}
void Backend::setLineSpacing(QString spacing) {
  if(!QStringList{"compact","standard","relaxed"}.contains(spacing) || spacing==lineSpacing())return;
  auto ui=store.data["ui"].toMap();ui["lineSpacing"]=spacing;store.data["ui"]=ui;
  emit typographyChanged();appearanceSave.start();
}
bool Backend::setProviderProxy(QString id, QString mode, QString address) {
  return setProviderNetwork(id, mode, address, provider(id)["proxy"].toMap()["caFile"].toString());
}
bool Backend::setProviderCertificate(QString id, QString caFile) {
  const auto proxy = provider(id)["proxy"].toMap();
  return setProviderNetwork(id, proxy.value("mode", "system").toString(), proxy["url"].toString(), caFile);
}
bool Backend::setProviderNetwork(QString id, QString mode, QString address, QString caFile) {
  try {
    if (!QStringList{"system", "direct", "custom"}.contains(mode) ||
        provider(id).isEmpty())
      throw std::runtime_error("代理设置无效");
    bool managing = false;
    for (const auto &row : providers->rows())
      if (row.toMap()["id"] == id && row.toMap()["busy"].toBool())
        managing = true;
    if (modelsBusy(id) || managing)
      throw std::runtime_error("该供应商仍有任务运行");
    if (mode == "custom") {
      QUrl url(address.trimmed());
      if (!url.isValid() ||
          !QStringList{"http", "https"}.contains(url.scheme()) ||
          url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() ||
          url.hasFragment() || (url.path() != "" && url.path() != "/") ||
          url.port(80) < 1 || url.port(80) > 65535)
        throw std::runtime_error("请输入有效的 HTTP / HTTPS 代理地址");
      address = url.toString();
    } else
      address.clear();
    caFile = caFile.trimmed();
    if (!caFile.isEmpty()) {
      caFile = U::cleanPath(caFile);
      QFile file(caFile);
      if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024 ||
          QSslCertificate::fromData(file.readAll(), QSsl::Pem).isEmpty())
        throw std::runtime_error("CA 证书文件不存在、不可读或不是有效的 PEM 证书（最大 4 MiB）");
    }
    QVariantMap network{{"mode", mode}, {"url", address}};
    if (!caFile.isEmpty()) network["caFile"] = caFile;
    auto rows = store.data["providers"].toList();
    for (auto &value : rows) {
      auto record = value.toMap();
      if (record["id"] == id) {
        record["proxy"] = network;
        value = record;
      }
    }
    store.data["providers"] = rows;
    store.save();
    configureGateway();
    invalidate();
    emit changed();
    emit providers->changed();
    return true;
  } catch (const std::exception &error) {
    emit toast(QString::fromUtf8(error.what()));
    return false;
  }
}
QString Backend::requestReport(QString id) const {
  if (!QRegularExpression("^[a-f0-9-]{36}$").match(id).hasMatch())
    return {};
  QFile file(store.directory + "/request-errors/" + id + ".json");
  return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                        : QString();
}

bool Backend::saveDesktopSettings(QString raw) {
  QJsonParseError error;auto doc=QJsonDocument::fromJson(raw.toUtf8(),&error);
  if(error.error!=QJsonParseError::NoError || !doc.isObject())return false;
  auto values=doc.object().toVariantMap(), desktop=store.data["desktop"].toMap();
  for(auto it=values.cbegin();it!=values.cend();++it) {
    if(QStringList{"autoOpenSubmissions","chatWidthHints"}.contains(it.key())){if(it.value().metaType().id()!=QMetaType::Bool)return false;}
    else if(it.key()=="webDisplay"){if(!QStringList{"embedded","external"}.contains(it.value().toString()))return false;}
    else return false;
    desktop[it.key()]=it.value();
  }
  store.data["desktop"]=desktop;store.save();
  for(const auto &s:records){auto web=webviewConfiguration();web["autoOpen"]=desktop.value("autoOpenSubmissions",true);web["display"]=desktop.value("webDisplay","embedded");writePi(s,{{"type","desktop_settings"},{"webview",web}});}
  emit changed();emit desktopSettingsChanged();return true;
}
bool Backend::openExternalHtml(QString path) {
  if(store.data["desktop"].toMap()["webDisplay"]!="external" || !QStringList{"html","htm"}.contains(QFileInfo(path).suffix().toLower()))return false;
  openChatboxPath(path,false);return true;
}
void Backend::openWebPreview(QString raw,QString title) {
  const auto url=normalizeWebUrl(raw);if(url.isEmpty())return;
  if(store.data["desktop"].toMap()["webDisplay"]=="external")QDesktopServices::openUrl(QUrl(url));
  else if(webviewAvailable())emit webRequested(url,title);
  else QDesktopServices::openUrl(QUrl(url));
}

void Backend::setToolPermissionMode(QString mode) {
  if(!QStringList{"auto","ask"}.contains(mode)){emit toast("工具执行权限模式无效");return;}
  auto agent=store.data["agent"].toMap();if(agent.value("toolPermissionMode","auto")==mode)return;
  const auto previous=agent;agent["toolPermissionMode"]=mode;store.data["agent"]=agent;
  if(!store.save()){store.data["agent"]=previous;emit toast("工具执行权限保存失败："+store.error);return;}
  emit changed();
  for(auto session:records)writePi(session,{{"type","tool_permission_mode"},{"mode",mode},{"language",language()},{"dataDir",store.directory}});
}
void Backend::compactSession() {
  if(!compactionAvailable() || view->busy())return;
  command(view,"compact");
}
