#include "backend.hpp"
#include <cmath>
#include <stdexcept>
#include <QSslCertificate>
bool Backend::setReasoningEffort(QString value) {
  if (!QStringList{"off", "low", "high", "max"}.contains(value))
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
  scale = qRound(qBound(0.5, scale, 2.0) * 100) / 100.;
  if (scale == fontScale())
    return;
  auto ui = store.data["ui"].toMap();
  ui["fontScale"] = scale;
  store.data["ui"] = ui;
  emit appearanceChanged();
  appearanceSave.start();
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
