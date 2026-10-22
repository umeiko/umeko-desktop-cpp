#include "store.hpp"
#include "qtkeychain/keychain.h"
#include <stdexcept>

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
      {"providerRuntime", QVariantMap{}},
      {"agent", QVariantMap{{"maxTokens", 8192},
                            {"instructions", ""},
                            {"autoCompaction", true},
                            {"agentDir", ""},
                            {"skillPaths", QStringList{}},
                            {"extensionPaths", QStringList{}},
                            {"safeMode", false},
                            {"npmRegistry", ""},
                            {"npmUserconfig", ""},
                            {"npmCafile", ""}}},
      {"server", QVariantMap{{"port", 8787},
                             {"openai", true},
                             {"anthropic", true},
                             {"lan", false},
                             {"keys", QVariantList{}}}},
      {"ui", QVariantMap{{"sidebarWidth", 244}, {"sidebarCollapsed", false}}}};
}
bool Store::migrateProfile(QString previous, QString target) {
  if (QFileInfo::exists(target) ||
      !QFileInfo::exists(previous + "/settings.json"))
    return false;
  previous = U::cleanPath(previous);
  target = U::cleanPath(target);
  const auto staging = target + "-migrate-" + U::uuid();
  QDir().mkpath(staging);
  QDirIterator files(
      previous, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
      QDirIterator::Subdirectories);
  bool ok = true;
  while (files.hasNext() && ok) {
    const auto source = files.next();
    const auto destination =
        staging + "/" + QDir(previous).relativeFilePath(source);
    const auto info = files.fileInfo();
    if (info.isSymLink())
      ok = QFile::link(info.symLinkTarget(), destination);
    else if (info.isDir())
      ok = QDir().mkpath(destination);
    else
      ok = QFile::copy(source, destination);
  }
  if (!ok)
    throw std::runtime_error(
        "Unable to copy user data; the original profile is unchanged");
  auto remap = [previous, target](QVariant value) -> QVariant {
    auto path = QDir::fromNativeSeparators(value.toString());
#ifdef Q_OS_WIN
    const auto sensitivity = Qt::CaseInsensitive;
#else
    const auto sensitivity = Qt::CaseSensitive;
#endif
    if (path.compare(previous, sensitivity) == 0)
      return target;
    return path.startsWith(previous + "/", sensitivity)
               ? target + path.mid(previous.size())
               : value;
  };
  QFile settingsFile(staging + "/settings.json");
  QJsonParseError parse;
  if (!settingsFile.open(QIODevice::ReadOnly))
    throw std::runtime_error("Unable to read the original user profile");
  const auto document = QJsonDocument::fromJson(settingsFile.readAll(), &parse);
  settingsFile.close();
  if (parse.error != QJsonParseError::NoError || !document.isObject())
    throw std::runtime_error("Invalid user profile; the original is unchanged");
  auto settings = document.object().toVariantMap();
  if (!settings.contains("credentialNamespace"))
    settings["credentialNamespace"] = U::sha(previous.toUtf8()).left(24);
  if (settings.contains("workspace"))
    settings["workspace"] = remap(settings["workspace"]);
  auto agent = settings["agent"].toMap();
  if (agent.contains("agentDir"))
    agent["agentDir"] = remap(agent["agentDir"]);
  settings["agent"] = agent;
  auto projects = settings["projects"].toList();
  for (auto &value : projects) {
    auto project = value.toMap();
    for (const auto &key : {"path", "workspace"})
      if (project.contains(key))
        project[key] = remap(project[key]);
    value = project;
  }
  settings["projects"] = projects;
  ok = U::writeJson(staging + "/settings.json", settings);
  if (QFileInfo::exists(staging + "/sessions.json")) {
    auto sessions =
        U::readJson(staging + "/sessions.json", QVariantList{}).toList();
    for (auto &value : sessions) {
      auto session = value.toMap();
      for (const auto &key : {"file", "workspace"})
        if (session.contains(key))
          session[key] = remap(session[key]);
      value = session;
    }
    ok = ok && U::writeJson(staging + "/sessions.json", sessions);
  }
  QDirIterator logs(staging + "/pi/sessions", {"*.jsonl"}, QDir::Files,
                    QDirIterator::Subdirectories);
  while (ok && logs.hasNext()) {
    const auto path = logs.next();
    if (logs.fileInfo().isSymLink())
      continue;
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly)) {
      ok = false;
      break;
    }
    auto header =
        QJsonDocument::fromJson(input.readLine()).object().toVariantMap();
    if (header["type"] != "session")
      continue;
    bool changed = false;
    for (const auto &key : {"cwd", "parentSession"})
      if (header.contains(key) && remap(header[key]) != header[key]) {
        header[key] = remap(header[key]);
        changed = true;
      }
    if (!changed)
      continue;
    const auto remainder = input.readAll();
    input.close();
    QSaveFile output(path);
    const auto bytes =
        QJsonDocument::fromVariant(header).toJson(QJsonDocument::Compact) +
        '\n' + remainder;
    ok = output.open(QIODevice::WriteOnly) &&
         output.write(bytes) == bytes.size() && output.commit();
  }
  if (!ok || !QDir().rename(staging, target))
    throw std::runtime_error(
        "Unable to migrate user data; the original profile is unchanged");
  return true;
}

Store::Store(QString path) {
  QString migratedFrom;
  if (path.isEmpty())
    path = qEnvironmentVariable("UMEKO_CPP_DATA_DIR");
  if (path.isEmpty())
    path = qEnvironmentVariable("UMEKO_DATA_DIR");
  if (path.isEmpty()) {
    const auto target = QDir::homePath() + "/.umeko/desktop-cpp";
#if defined(Q_OS_WIN)
    path = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("UmekoCpp");
#elif defined(Q_OS_MACOS)
    path = QDir::homePath() + "/Library/Application Support/UmekoCpp";
#else
    path = QDir(qEnvironmentVariable("XDG_DATA_HOME",
                                     QDir::homePath() + "/.local/share"))
               .filePath("umeko-cpp");
#endif
    if (migrateProfile(path, target))
      migratedFrom = path;
    path = target;
  }
  directory = U::cleanPath(path);
  QDir().mkpath(directory);
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
  if (!QStringList{"zh", "en"}.contains(ui["language"].toString())) {
    const auto preferred = QLocale::system().uiLanguages();
    const auto systemLanguage =
        preferred.isEmpty() ? QLocale::system().name() : preferred.first();
    auto language = qEnvironmentVariable(
        "UMEKO_LANGUAGE",
        systemLanguage.startsWith("zh", Qt::CaseInsensitive) ? "zh" : "en");
    ui["language"] = language == "zh" ? "zh" : "en";
  }
  bool numeric = false;
  auto scale = ui.value("fontScale", 1.0).toDouble(&numeric);
  ui["fontScale"] =
      numeric && ui.value("fontScale").metaType().id() != QMetaType::Bool &&
              std::isfinite(scale)
          ? qBound(0.5, scale, 2.0)
          : 1.0;
  ui["sidebarWidth"] = qBound(200, ui.value("sidebarWidth", 244).toInt(), 480);
  data["ui"] = ui;
  // Keychain entries retain their original namespace when the profile moves.
  if (!data.contains("credentialNamespace"))
    data["credentialNamespace"] =
        U::sha((migratedFrom.isEmpty() ? directory : migratedFrom).toUtf8())
            .left(24);
  keyPrefix = data["credentialNamespace"].toString() + ":";
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
