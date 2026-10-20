#include "store.hpp"
#include "qtkeychain/keychain.h"

QVariantMap Store::defaults() {
  return {
      {"projects", QVariantList{}},
      {"activeProject", ""},
      {"removedProjects", QVariantList{}},
      {"sessionMeta", QVariantMap{}},
      {"workspace", ""},
      {"dark", false},
      {"theme", "light"},
      {"providers", QVariantList{}},
      {"selectedProvider", ""},
      {"selectedModel", ""},
      {"providerRuntime", QVariantMap{{"python_path", ""}}},
      {"agent", QVariantMap{{"maxTokens", 8192},
                            {"instructions", ""},
                            {"autoCompaction", true},
                            {"agentDir", ""},
                            {"skillPaths", QStringList{}},
                            {"extensionPaths", QStringList{}},
                            {"safeMode", false}}},
      {"server", QVariantMap{{"port", 8787},
                             {"openai", true},
                             {"anthropic", true},
                             {"lan", false},
                             {"keys", QVariantList{}}}},
      {"ui", QVariantMap{{"sidebarWidth", 244}, {"sidebarCollapsed", false}}}};
}
Store::Store(QString path) {
  if (path.isEmpty())
    path = qEnvironmentVariable("UMEKO_CPP_DATA_DIR");
  if (path.isEmpty())
    path = qEnvironmentVariable("UMEKO_DATA_DIR");
  if (path.isEmpty()) {
#if defined(Q_OS_WIN)
    path = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("UmekoCpp");
#elif defined(Q_OS_MACOS)
    path = QDir::homePath() + "/Library/Application Support/UmekoCpp";
#else
    path = QDir(qEnvironmentVariable("XDG_DATA_HOME",
                                     QDir::homePath() + "/.local/share"))
               .filePath("umeko-cpp");
#endif
  }
  directory = U::cleanPath(path);
  QDir().mkpath(directory);
  keyPrefix = U::sha(directory.toUtf8()).left(24) + ":";
  data = defaults();
  QFile settings(directory + "/settings.json");
  if (settings.exists() && settings.open(QIODevice::ReadOnly)) {
    QJsonParseError parse;
    auto document = QJsonDocument::fromJson(settings.readAll(), &parse);
    settings.close();
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
      auto backup = settings.fileName() + ".invalid-" + U::uuid();
      writable = QFile::copy(settings.fileName(), backup);
      error = writable ? "配置格式损坏，原文件已备份"
                       : "配置格式损坏且无法备份，已停止写入";
    }
  }
  auto saved = U::readJson(directory + "/settings.json", QVariantMap{}).toMap();
  for (auto it = saved.begin(); it != saved.end(); ++it) {
    if (it.value().metaType().id() == QMetaType::QVariantMap) {
      auto merged = data[it.key()].toMap();
      auto values = it.value().toMap();
      for (auto v = values.begin(); v != values.end(); ++v)
        merged[v.key()] = v.value();
      data[it.key()] = merged;
    } else
      data[it.key()] = it.value();
  }
  auto ui = data["ui"].toMap();
  ui["sidebarWidth"] = qBound(200, ui.value("sidebarWidth", 244).toInt(), 480);
  data["ui"] = ui;
}
bool Store::save() {
  if (!writable)
    return false;
  return U::writeJson(directory + "/settings.json", data, &error);
}
QByteArray Store::secret(QString key) {
  if (cached.contains(key))
    return cached[key];
  auto job = new QKeychain::ReadPasswordJob("umeko-desktop-cpp");
  job->setAutoDelete(false);
  job->setKey(keyPrefix + key);
  job->setInsecureFallback(false);
  QEventLoop loop;
  QObject::connect(job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
  job->start();
  loop.exec();
  QByteArray result;
  if (job->error() == QKeychain::NoError)
    result = job->binaryData();
  else if (job->error() != QKeychain::EntryNotFound)
    error = "系统凭据存储不可用，请解锁密钥环";
  const bool read = job->error() == QKeychain::NoError ||
                    job->error() == QKeychain::EntryNotFound;
  delete job;
  if (read)
    cached[key] = result;
  return result;
}
bool Store::setSecret(QString key, QByteArray value) {
  auto job = new QKeychain::WritePasswordJob("umeko-desktop-cpp");
  job->setAutoDelete(false);
  job->setKey(keyPrefix + key);
  job->setInsecureFallback(false);
  job->setBinaryData(value);
  QEventLoop loop;
  QObject::connect(job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
  job->start();
  loop.exec();
  bool ok = job->error() == QKeychain::NoError;
  if (!ok)
    error = "无法保存凭据，请检查系统密钥环";
  else
    cached[key] = value;
  delete job;
  return ok;
}
QVariantList Store::sessions() const {
  return U::readJson(directory + "/sessions.json", QVariantList{}).toList();
}
bool Store::saveSessions(QVariantList rows) {
  return U::writeJson(directory + "/sessions.json", rows, &error);
}
