#include "backend.hpp"
#include "miniz.h"
#include <QDesktopServices>
#include <stdexcept>

static bool portablePath(QString path) {
  if (path.isEmpty() || path.startsWith('/') || path.contains('\\') ||
      path.contains(':') || path.contains(QChar(0)))
    return false;
  for (auto part : path.split('/'))
    if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') ||
        part.endsWith(' ') ||
        QRegularExpression("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$",
                           QRegularExpression::CaseInsensitiveOption)
            .match(part)
            .hasMatch())
      return false;
  return true;
}
static bool referencesPackage(QVariantList providers, QString plugin,
                              QString version) {
  for (const auto &value : providers) {
    const auto p = value.toMap();
    if (p["api"] == "plugin" && p["pluginId"] == plugin &&
        p["pluginVersion"] == version)
      return true;
  }
  return false;
}
static QString retirePackage(QString root, QString path) {
  if (!QFileInfo::exists(path))
    return {};
#ifdef Q_OS_WIN
  const auto sensitivity = Qt::CaseInsensitive;
#else
  const auto sensitivity = Qt::CaseSensitive;
#endif
  const auto packages = U::cleanPath(root + "/packages");
  if (QFileInfo(path).isSymLink() ||
      !U::cleanPath(path).startsWith(packages + "/", sensitivity))
    throw std::runtime_error("插件包路径超出安装目录，无法清理");
  const auto retired = root + "/retired/" + U::uuid();
  if (!QDir().mkpath(root + "/retired") || !QDir().rename(path, retired))
    throw std::runtime_error("无法清理旧插件包，请关闭使用它的进程后重试");
  return retired;
}
static void cleanPackageTemporary(QString root, QString path, QString kind) {
  if (!path.isEmpty() && !QFileInfo(path).isSymLink() &&
      U::cleanPath(QFileInfo(path).absolutePath()) ==
          U::cleanPath(root + "/" + kind))
    QDir(path).removeRecursively();
}
static void validateConfig(QVariant value, QVariantMap schema,
                           QString field = "config") {
  auto kind = schema["type"].toString();
  auto type = value.metaType().id();
  bool valid = kind == "object"    ? type == QMetaType::QVariantMap
               : kind == "array"   ? type == QMetaType::QVariantList
               : kind == "string"  ? type == QMetaType::QString
               : kind == "boolean" ? type == QMetaType::Bool
               : kind == "integer" || kind == "number"
                   ? (type == QMetaType::Int || type == QMetaType::LongLong ||
                      type == QMetaType::Double)
                   : false;
  if (!valid ||
      (kind == "integer" && std::floor(value.toDouble()) != value.toDouble()))
    throw std::runtime_error(("配置类型无效：" + field).toStdString());
  if (schema.contains("enum") && !schema["enum"].toList().contains(value))
    throw std::runtime_error(("配置选项无效：" + field).toStdString());
  if (kind == "object") {
    auto object = value.toMap(), properties = schema["properties"].toMap();
    for (auto key : schema["required"].toList())
      if (!object.contains(key.toString()))
        throw std::runtime_error(
            ("缺少配置项：" + field + "." + key.toString()).toStdString());
    for (auto it = object.begin(); it != object.end(); ++it) {
      if (properties.contains(it.key()))
        validateConfig(it.value(), properties[it.key()].toMap(),
                       field + "." + it.key());
      else if (schema.contains("additionalProperties") &&
               !schema["additionalProperties"].toBool())
        throw std::runtime_error(("未知配置项：" + it.key()).toStdString());
    }
  }
  if (kind == "array" && schema.contains("items"))
    for (auto v : value.toList())
      validateConfig(v, schema["items"].toMap(), field + "[]");
}
Providers::Providers(Backend *backend) : QObject(backend), b(backend) {}
QString Providers::packageDir(QVariantMap p) const {
  auto id = p["pluginId"].toString(), version = p["pluginVersion"].toString();
  if (!QRegularExpression("^[a-z][a-z0-9-]{0,63}$").match(id).hasMatch() ||
      !QRegularExpression("^\\d+\\.\\d+\\.\\d+(?:-[a-zA-Z0-9.-]+)?$")
           .match(version)
           .hasMatch())
    return {};
  return b->store.directory + "/provider-plugins/packages/" + id + "/" +
         version;
}
QVariantMap Providers::manifest(QVariantMap p) const {
  return U::readJson(packageDir(p) + "/manifest.json", QVariantMap{}).toMap();
}
QString Providers::resolvePython() const {
#ifdef Q_OS_WIN
  auto bundled = b->root + "/runtime/python/python.exe";
#else
  auto bundled = b->root + "/runtime/python/bin/python3";
#endif
  if (QFileInfo(bundled).isFile() && QFileInfo(bundled).isExecutable())
    return U::cleanPath(bundled);
  return U::python();
}
QString Providers::pythonExecutable() const { return resolvePython(); }
QVariantList Providers::rows() const {
  QVariantList out;
  for (auto v : b->store.data["providers"].toList()) {
    auto p = v.toMap();
    if (p["api"] != "plugin")
      continue;
    auto m = manifest(p);
    QVariantList fields;
    auto values = p["authValues"].toMap();
    auto flags = p["secretFields"].toStringList();
    for (auto fv : m["auth"].toMap()["fields"].toList()) {
      auto f = fv.toMap();
      f["secret"] = f.value("secret", false);
      f["value"] = f["secret"].toBool() ? QVariant("")
                                        : values.value(f["key"].toString(), "");
      f["hasValue"] = flags.contains(f["key"].toString()) ||
                      !f["value"].toString().isEmpty();
      fields << f;
    }
    out << QVariantMap{
        {"id", p["id"]},
        {"name", p["name"]},
        {"version", p["pluginVersion"]},
        {"fields", fields},
        {"models", p["models"]},
        {"busy", busy.contains(p["id"].toString())},
        {"status",
         statuses.value(p["id"].toString(),
                        p.value("accountLabel", "尚未登录").toString())},
    };
  }
  return out;
}
bool Providers::importZip(QString url) {
  try {
    auto path = U::localPath(url);
    if (!QFileInfo(path).isFile() ||
        !path.endsWith(".zip", Qt::CaseInsensitive))
      throw std::runtime_error("请选择本机供应商 ZIP");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 25 * 1024 * 1024)
      throw std::runtime_error("供应商 ZIP 超过限制或不可读取");
    auto raw = f.readAll();
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, raw.constData(), raw.size(), 0))
      throw std::runtime_error("ZIP 文件无效");
    auto guard = qScopeGuard([&] { mz_zip_reader_end(&zip); });
    auto count = mz_zip_reader_get_num_files(&zip);
    if (count > 512)
      throw std::runtime_error("供应商包最多包含 512 个文件");
    QHash<QString, QByteArray> files;
    QSet<QString> seen;
    quint64 total = 0;
    QStringList manifests;
    for (mz_uint i = 0; i < count; ++i) {
      mz_zip_archive_file_stat info{};
      if (!mz_zip_reader_file_stat(&zip, i, &info))
        throw std::runtime_error("ZIP 索引无效");
      auto name = QString::fromUtf8(info.m_filename);
      bool directory = name.endsWith('/');
      if (directory)
        name.chop(1);
      if (!portablePath(name) ||
          ((info.m_external_attr >> 16) & 0170000) == 0120000)
        throw std::runtime_error("供应商包包含不安全的路径或链接");
      if (directory)
        continue;
      if (seen.contains(name.toCaseFolded()))
        throw std::runtime_error("供应商包包含重复路径");
      seen.insert(name.toCaseFolded());
      total += info.m_uncomp_size;
      if (total > 20 * 1024 * 1024)
        throw std::runtime_error("供应商包解压大小超过 20 MB");
      QByteArray bytes(info.m_uncomp_size, '\0');
      if (!mz_zip_reader_extract_to_mem(&zip, i, bytes.data(), bytes.size(), 0))
        throw std::runtime_error("ZIP 数据校验失败");
      files[name] = bytes;
      if (name.endsWith("manifest.json"))
        manifests << name;
    }
    QString prefix;
    if (!files.contains("manifest.json")) {
      if (manifests.size() != 1 || manifests[0].count('/') != 1)
        throw std::runtime_error("ZIP 应包含一个 manifest.json");
      prefix = manifests[0].section('/', 0, 0) + "/";
    }
    QHash<QString, QByteArray> entries;
    for (auto it = files.begin(); it != files.end(); ++it) {
      if (!it.key().startsWith(prefix))
        throw std::runtime_error("包目录外存在其他文件");
      entries[it.key().mid(prefix.size())] = it.value();
    }
    auto m = QJsonDocument::fromJson(entries["manifest.json"])
                 .object()
                 .toVariantMap();
    auto plugin = m["id"].toString(), version = m["version"].toString();
    if (m["apiVersion"].toInt() != 1 ||
        !QRegularExpression("^[a-z][a-z0-9-]{0,63}$")
             .match(plugin)
             .hasMatch() ||
        !QRegularExpression("^\\d+\\.\\d+\\.\\d+(?:-[a-zA-Z0-9.-]+)?$")
             .match(version)
             .hasMatch() ||
        m["name"].toString().trimmed().isEmpty())
      throw std::runtime_error(
          "manifest.json 缺少有效的 API 版本、ID、名称或版本");
    auto entry = m["entrypoint"].toString().split(':');
    if (entry.size() != 2 || !portablePath(entry[0]) ||
        !entry[0].endsWith(".py") ||
        !QRegularExpression("^[A-Za-z_]\\w*$").match(entry[1]).hasMatch() ||
        !entries.contains(entry[0]))
      throw std::runtime_error("供应商入口无效");
    auto auth = m["auth"].toMap()["fields"].toList();
    if (auth.size() > 20)
      throw std::runtime_error("登录字段过多");
    QSet<QString> authKeys;
    for (auto v : auth) {
      auto field = v.toMap();
      auto key = field["key"].toString();
      if (!QRegularExpression("^[A-Za-z][A-Za-z0-9_]{0,63}$")
               .match(key)
               .hasMatch() ||
          authKeys.contains(key) || field["label"].toString().isEmpty())
        throw std::runtime_error("登录字段无效");
      authKeys.insert(key);
    }
    auto configName = m.value("config", "config.json").toString(),
         schemaName = m.value("configSchema", "config.schema.json").toString();
    if (!portablePath(configName) || !portablePath(schemaName) ||
        !entries.contains(configName) || !entries.contains(schemaName))
      throw std::runtime_error("供应商配置文件缺失");
    auto config = QJsonDocument::fromJson(entries[configName]).toVariant();
    auto schema =
        QJsonDocument::fromJson(entries[schemaName]).object().toVariantMap();
    validateConfig(config, schema);
    auto destination = b->store.directory + "/provider-plugins/packages/" +
                       plugin + "/" + version;
    const auto providerRoot = b->store.directory + "/provider-plugins";
    const auto previous = b->store.data["providers"].toList();
    const bool referenced = referencesPackage(previous, plugin, version);
    bool exists = QFileInfo(destination).isDir();
    if (exists && referenced) {
      QHash<QString, QString> existing;
      QDirIterator it(destination, QDir::Files, QDirIterator::Subdirectories);
      while (it.hasNext()) {
        QFile file(it.next());
        auto name = QDir(destination).relativeFilePath(file.fileName());
        if (name.contains("__pycache__"))
          continue;
        if (file.open(QIODevice::ReadOnly))
          existing[name] = U::sha(file.readAll());
      }
      if (existing.size() != entries.size())
        throw std::runtime_error("该版本仍被账号使用，包内容不同；请升级插件版本号或移除所有相关账号");
      for (auto it = entries.begin(); it != entries.end(); ++it)
        if (existing[it.key()] != U::sha(it.value()))
          throw std::runtime_error("该版本仍被账号使用，包内容不同；请升级插件版本号或移除所有相关账号");
    }
    const bool install = !exists || !referenced;
    QString staging, retired;
    auto cleanup = qScopeGuard(
        [&] { cleanPackageTemporary(providerRoot, staging, "staging"); });
    if (install) {
      staging = providerRoot + "/staging/" + U::uuid();
      for (auto it = entries.begin(); it != entries.end(); ++it) {
        auto target = staging + "/" + it.key();
        QDir().mkpath(QFileInfo(target).absolutePath());
        QSaveFile output(target);
        if (!output.open(QIODevice::WriteOnly) ||
            output.write(it.value()) != it.value().size() || !output.commit())
          throw std::runtime_error("无法写入供应商包");
      }
    }
    try {
      if (install) {
        if (exists)
          retired = retirePackage(providerRoot, destination);
        if (!QDir().mkpath(QFileInfo(destination).absolutePath()) ||
            !QDir().rename(staging, destination))
          throw std::runtime_error("无法安装供应商包");
      }
      QVariantMap record{
          {"id", U::uuid()},    {"api", "plugin"},          {"name", m["name"]},
          {"pluginId", plugin}, {"pluginVersion", version}, {"baseUrl", ""},
          {"hasKey", false},    {"models", QVariantList{}}};
      auto providers = previous;
      providers << record;
      b->store.data["providers"] = providers;
      if (!b->store.save())
        throw std::runtime_error("供应商配置保存失败");
    } catch (...) {
      b->store.data["providers"] = previous;
      if (install && QFileInfo::exists(destination) &&
          !QFileInfo::exists(staging))
        QDir().rename(destination, staging);
      if (!retired.isEmpty())
        QDir().rename(retired, destination);
      throw;
    }
    cleanPackageTemporary(providerRoot, retired, "retired");
    b->notify();
    emit changed();
    emit b->toast("供应商已导入，请在本页登录并刷新模型");
    return true;
  } catch (const std::exception &error) {
    emit b->toast(QString::fromUtf8(error.what()));
    return false;
  }
}
QString Providers::configJson(QString id) const {
  auto p = b->provider(id), m = manifest(p);
  auto path = b->store.directory + "/provider-plugins/config/" +
              U::sha(id.toUtf8()) + ".json";
  auto config = U::readJson(
      path, U::readJson(packageDir(p) + "/" +
                            m.value("config", "config.json").toString(),
                        QVariantMap{}));
  return QString::fromUtf8(
      QJsonDocument::fromVariant(config).toJson(QJsonDocument::Indented));
}
bool Providers::saveConfig(QString id, QString raw) {
  if (b->modelsBusy(id) || busy.contains(id)) {
    emit b->toast("请等待供应商请求结束");
    return false;
  }
  try {
    auto p = b->provider(id), m = manifest(p);
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError)
      throw std::runtime_error("请输入有效 JSON");
    auto schema =
        U::readJson(packageDir(p) + "/" +
                    m.value("configSchema", "config.schema.json").toString())
            .toMap();
    validateConfig(doc.toVariant(), schema);
    if (!U::writeJson(b->store.directory + "/provider-plugins/config/" +
                          U::sha(id.toUtf8()) + ".json",
                      doc.toVariant()))
      throw std::runtime_error("配置保存失败");
    auto secrets = QJsonDocument::fromJson(b->store.secret("account:" + id))
                       .object()
                       .toVariantMap();
    auto credentials = secrets["credentials"];
    secrets.clear();
    if (credentials.isValid())
      secrets["credentials"] = credentials;
    b->store.setSecret(
        "account:" + id,
        QJsonDocument::fromVariant(secrets).toJson(QJsonDocument::Compact));
    statuses[id] = "配置已更新，请重新登录";
    emit changed();
    return true;
  } catch (const std::exception &e) {
    emit b->toast(QString::fromUtf8(e.what()));
    return false;
  }
}
void Providers::invoke(QString id, QString method, QVariantMap params,
                       std::function<void(QVariantMap)> done,
                       std::function<void(QVariantMap)> onEvent,
                       QString requestKey) {
  auto p = b->provider(id);
  auto program = resolvePython();
  if (program.isEmpty()) {
    done(
        {{"error",
          "未找到 Python 3.10+，请使用内置 Python 的正式版或安装本机 Python"}});
    return;
  }
  if (p["api"] != "plugin" || packageDir(p).isEmpty()) {
    done({{"error", "供应商包不存在"}});
    return;
  }
  auto process = new QProcess(this);
  U::hideConsole(process);
  auto request = requestKey.isEmpty() ? U::uuid() : requestKey;
  workers[request] = process;
  auto buffer = std::make_shared<QByteArray>();
  auto finished = std::make_shared<bool>(false);
  auto pendingSecrets = std::make_shared<QByteArray>();
  auto timeout = new QTimer(process);
  timeout->setSingleShot(true);
  timeout->setInterval(method == "events" ? 300000 : 60000);
  auto complete = [=, this](QVariantMap result) {
    if (*finished)
      return;
    *finished = true;
    workers.remove(request);
    timeout->stop();
    if (!pendingSecrets->isEmpty() &&
        *pendingSecrets != b->store.secret("account:" + id) &&
        !b->store.setSecret("account:" + id, *pendingSecrets))
      result = {{"error", "无法保护供应商凭据，请检查系统密钥环"}};
    done(result);
    if (process->state() == QProcess::NotRunning)
      process->deleteLater();
    else {
      connect(process, &QProcess::finished, process, &QObject::deleteLater);
      QTimer::singleShot(4000, process, [process] {
        if (process->state() != QProcess::NotRunning)
          process->kill();
      });
    }
  };
  auto env = U::environment();
  // Provider SDK HttpSession(environment_proxy=True) follows the user's
  // network environment; Pi's separately guarded process does not inherit it.
  auto system = QProcessEnvironment::systemEnvironment();
  for (auto key : system.keys())
    if (key.toUpper().endsWith("_PROXY"))
      env.insert(key, system.value(key));
  const auto proxy = p["proxy"].toMap();
  if (proxy["mode"] == "direct" || proxy["mode"] == "custom") {
    for (const auto &key : env.keys())
      if (key.toUpper().endsWith("_PROXY"))
        env.remove(key);
    if (proxy["mode"] == "direct")
      env.insert("NO_PROXY", "*");
    else {
      env.insert("HTTP_PROXY", proxy["url"].toString());
      env.insert("HTTPS_PROXY", proxy["url"].toString());
    }
  }
  env.insert("PYTHONUTF8", "1");
  env.insert("PYTHONUNBUFFERED", "1");
  env.insert("PYTHONDONTWRITEBYTECODE", "1");
  env.remove("PYTHONPATH");
  env.remove("PYTHONHOME");
  process->setProcessEnvironment(env);
  process->setProgram(program);
  process->setArguments({"-u", b->root + "/provider-runtime/runner.py"});
  auto state =
      b->store.directory + "/provider-plugins/state/" + U::sha(id.toUtf8());
  QDir().mkpath(state);
  auto secrets = QJsonDocument::fromJson(b->store.secret("account:" + id))
                     .object()
                     .toVariantMap();
  QVariantMap input{
      {"package", packageDir(p)},
      {"sdk", b->root + "/provider-runtime"},
      {"state_dir", state},
      {"config", QJsonDocument::fromJson(configJson(id).toUtf8()).toVariant()},
      {"secrets", secrets},
      {"proxy", proxy},
      {"method", method},
      {"params", params}};
  connect(process, &QProcess::started, this, [process, input] {
    process->write(
        QJsonDocument::fromVariant(input).toJson(QJsonDocument::Compact) +
        '\n');
    process->closeWriteChannel();
  });
  connect(process, &QProcess::readyReadStandardOutput, this, [=, this] {
    *buffer += process->readAllStandardOutput();
    if (buffer->size() > 32 * 1024 * 1024) {
      process->kill();
      complete({{"error", "供应商返回数据过大"}});
      return;
    }
    while (buffer->contains('\n')) {
      int i = buffer->indexOf('\n');
      auto raw = buffer->left(i);
      buffer->remove(0, i + 1);
      auto event = QJsonDocument::fromJson(raw).object().toVariantMap();
      auto type = event["type"].toString();
      if (type == "secrets") {
        *pendingSecrets = QJsonDocument::fromVariant(event["data"])
                              .toJson(QJsonDocument::Compact);
      } else if (type == "event") {
        timeout->start();
        if (onEvent)
          onEvent(event["data"].toMap());
      } else if (type == "result") {
        complete(event["data"].toMap());
        return;
      } else if (type == "error") {
        complete(
            {{"error", event.value("message", "供应商运行失败").toString()},
             {"diagnostics", event["diagnostics"]}});
        return;
      }
    }
  });
  connect(process, &QProcess::readyReadStandardError, this,
          [process] { process->readAllStandardError(); });
  connect(process, &QProcess::finished, this, [=](int, QProcess::ExitStatus) {
    if (!*finished)
      complete(
          {{"error", "供应商进程退出，请检查 Python 版本、依赖与插件配置"}});
  });
  connect(process, &QProcess::errorOccurred, this,
          [=](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
              complete({{"error", "无法启动 Python，请检查运行环境"}});
          });
  connect(timeout, &QTimer::timeout, this, [=] {
    process->kill();
    complete({{"error", "供应商请求超时"}});
  });
  timeout->start();
  process->start();
}
void Providers::login(QString id, QString raw) {
  if (b->modelsBusy(id) || busy.contains(id)) {
    emit b->toast("请等待供应商请求结束");
    return;
  }
  auto p = b->provider(id), m = manifest(p);
  auto values = QJsonDocument::fromJson(raw.toUtf8()).object().toVariantMap();
  auto secrets = QJsonDocument::fromJson(b->store.secret("account:" + id))
                     .object()
                     .toVariantMap();
  auto saved = secrets["credentials"].toMap();
  bool identity = false;
  auto fields = m["auth"].toMap()["fields"].toList();
  for (auto v : fields) {
    auto f = v.toMap();
    if (!f["secret"].toBool() &&
        values[f["key"].toString()] != saved[f["key"].toString()])
      identity = true;
  }
  QVariantMap credentials, publicValues;
  QStringList flags;
  for (auto v : fields) {
    auto f = v.toMap();
    auto key = f["key"].toString();
    auto value = values[key].toString();
    if (value.isEmpty() && f["secret"].toBool() && !identity)
      value = saved[key].toString();
    if (value.isEmpty() && f.value("required", true).toBool()) {
      emit b->toast("请填写" + f["label"].toString());
      return;
    }
    credentials[key] = value;
    if (f["secret"].toBool() && !value.isEmpty())
      flags << key;
    else if (!f["secret"].toBool())
      publicValues[key] = value;
  }
  busy.insert(id);
  statuses[id] = "正在登录并发现模型…";
  emit changed();
  invoke(id, "login", {{"credentials", credentials}},
         [this, id, publicValues, flags](QVariantMap result) {
           busy.remove(id);
           if (result.contains("error")) {
             statuses[id] = result["error"].toString();
             emit b->toast(statuses[id]);
             emit changed();
             return;
           }
           auto providers = b->store.data["providers"].toList();
           for (auto &v : providers) {
             auto p = v.toMap();
             if (p["id"] == id) {
               p["authValues"] = publicValues;
               p["secretFields"] = flags;
               v = p;
             }
           }
           b->store.data["providers"] = providers;
           applyModels(id, result["models"].toList(),
                       result["label"].toString());
         });
}
void Providers::refreshModels(QString id) {
  if (b->modelsBusy(id) || busy.contains(id)) {
    emit b->toast("请等待供应商请求结束");
    return;
  }
  busy.insert(id);
  statuses[id] = "正在刷新模型…";
  emit changed();
  invoke(id, "models", {}, [this, id](QVariantMap result) {
    busy.remove(id);
    if (result.contains("error")) {
      statuses[id] = result["error"].toString();
      emit b->toast(statuses[id]);
      emit changed();
      return;
    }
    applyModels(id, result["models"].toList());
  });
}
void Providers::applyModels(QString id, QVariantList rows, QString label) {
  try {
    if (rows.size() > 2000)
      throw std::runtime_error("供应商模型过多");
    auto providers = b->store.data["providers"].toList();
    QSet<QString> taken;
    for (auto v : providers)
      if (v.toMap()["id"] != id)
        for (auto m : v.toMap()["models"].toList())
          taken.insert(m.toMap()["alias"].toString());
    auto original = b->provider(id);
    QHash<QString, QVariantMap> old;
    for (auto v : original["models"].toList())
      old[v.toMap()["upstream"].toString()] = v.toMap();
    QVariantList models;
    QSet<QString> ids;
    for (auto v : rows) {
      auto m = v.toMap();
      auto upstream = m.value("id", m["upstream"]).toString();
      if (upstream.isEmpty() || ids.contains(upstream))
        throw std::runtime_error("模型 ID 缺失或重复");
      ids.insert(upstream);
      auto previous = old.value(upstream);
      auto alias = previous.value("alias", upstream).toString();
      if (taken.contains(alias))
        alias = id.left(8) + "/" + upstream;
      if (taken.contains(alias) || alias.contains(QRegularExpression("\\s")))
        throw std::runtime_error("模型别名无效");
      taken.insert(alias);
      m["upstream"] = upstream;
      m["alias"] = alias;
      for (auto key : {QString("contextWindow"), QString("vision")}) {
        auto automatic = key == "vision" ? "visionAuto" : "contextWindowAuto";
        auto source = key + "Source";
        if (m.contains(key)) {
          m[automatic] = false;
          m[source] = "provider";
        }
        if (previous.contains(automatic) && !previous[automatic].toBool() &&
            previous[source] != "provider") {
          m[key] = previous[key];
          m[automatic] = false;
          m[source] = "custom";
        }
      }
      models << U::normalizeModel(m, previous);
    }
    for (auto &v : providers) {
      auto p = v.toMap();
      if (p["id"] == id) {
        p["models"] = models;
        if (!label.isEmpty())
          p["accountLabel"] = label;
        v = p;
      }
    }
    b->store.data["providers"] = providers;
    statuses[id] = (label.isEmpty() ? "已连接" : label) +
                   QString(" · %1 个模型").arg(models.size());
    b->invalidate();
    b->notify();
    emit changed();
    emit b->toast("可用模型已更新");
  } catch (const std::exception &e) {
    statuses[id] = QString::fromUtf8(e.what());
    emit changed();
    emit b->toast(statuses[id]);
  }
}
bool Providers::saveModels(QString id, QString raw) {
  if (b->modelsBusy(id) || busy.contains(id))
    return false;
  try {
    auto rows = QJsonDocument::fromJson(raw.toUtf8()).array().toVariantList();
    auto original = b->provider(id);
    QHash<QString, QVariantMap> old;
    for (auto v : original["models"].toList())
      old[v.toMap()["upstream"].toString()] = v.toMap();
    QSet<QString> taken;
    for (auto p : b->store.data["providers"].toList())
      if (p.toMap()["id"] != id)
        for (auto m : p.toMap()["models"].toList())
          taken.insert(m.toMap()["alias"].toString());
    QVariantList models;
    QSet<QString> ids;
    for (auto v : rows) {
      auto m = v.toMap();
      auto upstream = m["upstream"].toString(),
           alias = m["alias"].toString().trimmed();
      if (alias.isEmpty())
        alias = upstream;
      if (!old.contains(upstream) || ids.contains(upstream) ||
          taken.contains(alias) || alias.contains(QRegularExpression("\\s")))
        throw std::runtime_error(
            "模型 ID 请通过刷新获取；别名不能重复或含空白");
      ids.insert(upstream);
      taken.insert(alias);
      m["alias"] = alias;
      auto normalized = U::normalizeModel(m, old[upstream]);
      for (auto key : {QString("contextWindow"), QString("vision")}) {
        auto automatic = key == "vision" ? "visionAuto" : "contextWindowAuto";
        bool same = normalized[key] == old[upstream][key] &&
                    normalized[automatic] == old[upstream][automatic];
        normalized[key + "Source"] =
            same ? old[upstream].value(key + "Source", "custom")
                 : QVariant(normalized[automatic].toBool() ? "auto" : "custom");
      }
      models << normalized;
    }
    if (ids.size() != old.size())
      throw std::runtime_error("请保留所有已发现的模型 ID");
    auto providers = b->store.data["providers"].toList();
    for (auto &v : providers) {
      auto p = v.toMap();
      if (p["id"] == id) {
        p["models"] = models;
        v = p;
      }
    }
    b->store.data["providers"] = providers;
    b->invalidate();
    b->notify();
    emit changed();
    return true;
  } catch (const std::exception &e) {
    emit b->toast(QString::fromUtf8(e.what()));
    return false;
  }
}
void Providers::remove(QString id) {
  if (b->modelsBusy(id) || busy.contains(id) || !workers.isEmpty()) {
    emit b->toast("请等待供应商请求结束后移除");
    return;
  }
  const auto provider = b->provider(id);
  if (provider["api"] != "plugin")
    return;
  const auto previous = b->store.data["providers"].toList();
  auto rows = previous;
  for (int i = rows.size() - 1; i >= 0; --i)
    if (rows[i].toMap()["id"] == id)
      rows.removeAt(i);
  b->store.data["providers"] = rows;
  if (!b->store.save()) {
    b->store.data["providers"] = previous;
    emit b->toast("供应商配置保存失败，未移除");
    return;
  }
  b->store.setSecret("account:" + id, {});
  statuses.remove(id);
  if (!referencesPackage(rows, provider["pluginId"].toString(),
                         provider["pluginVersion"].toString())) {
    try {
      const auto path = packageDir(provider);
      if (!path.isEmpty())
        cleanPackageTemporary(
            b->store.directory + "/provider-plugins",
            retirePackage(b->store.directory + "/provider-plugins", path),
            "retired");
    } catch (const std::exception &error) {
      emit b->toast(QString::fromUtf8(error.what()));
    }
  }
  b->invalidate();
  b->notify();
  emit changed();
}
QString Providers::resourceFile(QString kind) const {
  return QUrl::fromLocalFile(QDir::homePath() + "/" +
                             (kind == "sdk" ? "umeko-provider-sdk-0.2.0.zip"
                                            : "develop-umeko-provider.zip"))
      .toString();
}
bool Providers::exportResource(QString kind, QString url) {
  auto name = kind == "sdk" ? "umeko-provider-sdk-0.2.0.zip"
                            : "develop-umeko-provider.zip";
  QFile source(b->root + "/provider-resources/" + name);
  if (!source.exists())
    source.setFileName(b->root + "/build/provider-resources/" + name);
  auto target = U::localPath(url);
  if (target.isEmpty() || !source.open(QIODevice::ReadOnly)) {
    emit b->toast("开发资源不可用");
    return false;
  }
  QSaveFile output(target);
  if (!output.open(QIODevice::WriteOnly) ||
      output.write(source.readAll()) < 0 || !output.commit()) {
    emit b->toast("无法导出开发资源");
    return false;
  }
  emit b->toast("开发资源已导出");
  return true;
}
void Providers::complete(QString request, QString id, QVariantMap body) {
  if (busy.contains(id)) {
    b->gatewayWrite({{"type", "provider-result"},
                     {"id", request},
                     {"error", "供应商正在登录或刷新，请稍后重试"}});
    return;
  }
  invoke(
      id, "events", body,
      [this, request](QVariantMap result) {
        b->gatewayWrite({{"type", "provider-result"},
                         {"id", request},
                         {"diagnostics", result["diagnostics"]},
                         {"error", result.value("error", "")}});
      },
      [this, request](QVariantMap data) {
        b->gatewayWrite(
            {{"type", "provider-event"}, {"id", request}, {"event", data}});
      },
      request);
}
void Providers::cancel(QString request) {
  auto process = workers.value(request);
  if (!process)
    return;
  process->terminate();
  QTimer::singleShot(800, process, [process] {
    if (process->state() != QProcess::NotRunning)
      process->kill();
  });
}
void Providers::close() {
  for (auto p : workers) {
    disconnect(p, nullptr, this, nullptr);
    p->terminate();
    if (!p->waitForFinished(800)) {
      p->kill();
      p->waitForFinished(500);
    }
  }
  workers.clear();
}
