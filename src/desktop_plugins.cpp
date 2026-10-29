#include "desktop_plugins.hpp"
#include "backend.hpp"
#include <QDesktopServices>

static QByteArray frame(QVariantMap value) {
  return QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact) +
         '\n';
}
static QString boundedText(QString file, qint64 size = 65536) {
  QFile f(file);
  return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(size))
                                     : QString{};
}
DesktopPlugins::DesktopPlugins(Backend *backend)
    : QObject(backend), b(backend) {
  root = b->store.directory + "/desktop-plugins";
  registry = root + "/settings.json";
  settings = U::readJson(registry, QVariantMap{}).toMap();
  entries = settings.value("installed").toMap();
  for (auto &id : entries.keys()) {
    auto record = entries[id].toMap();
    record["builtin"] = false;
    entries[id] = record;
    if (!QRegularExpression("^[a-z][a-z0-9-]{0,63}$").match(id).hasMatch() ||
        !QRegularExpression("^\\d+\\.\\d+\\.\\d+(?:-[A-Za-z0-9.-]+)?$")
             .match(record["version"].toString())
             .hasMatch())
      entries.remove(id);
  }
  QDir shipped(b->root + "/umeko-plugins");
  for (const auto &id : shipped.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    auto manifest =
        U::readJson(shipped.filePath(id + "/manifest.json"), QVariantMap{})
            .toMap();
    if (manifest["kind"] != "umeko-plugin" || manifest["apiVersion"] != 1 ||
        manifest["id"] != id)
      continue;
    manifest["builtin"] = true;
    entries[id] = manifest;
  }
  connect(b, &Backend::viewChanged, this, &DesktopPlugins::synchronize);
  contextRefresh.setSingleShot(true);
  contextRefresh.setInterval(200);
  connect(&contextRefresh, &QTimer::timeout, this,
          &DesktopPlugins::synchronize);
  connect(b, &Backend::chatChanged, this, [this] {
    if (!contextRefresh.isActive())
      contextRefresh.start();
  });
  synchronize();
}
DesktopPlugins::~DesktopPlugins() { close(); }
QVariantMap DesktopPlugins::entry(QString id) const {
  return entries.value(id).toMap();
}
QString DesktopPlugins::packagePath(QString id) const {
  auto e = entry(id);
  if (e.isEmpty())
    return {};
  return e["builtin"].toBool()
             ? b->root + "/umeko-plugins/" + id
             : root + "/packages/" + id + "/" + e["version"].toString();
}
bool DesktopPlugins::enabled(QString id) const {
  return settings.value("enabled").toMap().value(id, true).toBool();
}
bool DesktopPlugins::sessionEnabled(QString id) const {
  auto sessions = settings.value("sessions").toMap();
  return sessions.contains(storageKey)
             ? sessions[storageKey].toList().contains(id)
             : entry(id)["builtin"].toBool();
}
QVariantList DesktopPlugins::rows() const {
  QVariantList result;
  for (auto id : entries.keys()) {
    auto e = entry(id);
    e["enabled"] = enabled(id);
    e["sessionEnabled"] = sessionEnabled(id);
    e["hasPage"] = !e["page"].toString().isEmpty();
    result << e;
  }
  std::sort(
      result.begin(), result.end(), [](const QVariant &a, const QVariant &c) {
        return a.toMap()["name"].toString() < c.toMap()["name"].toString();
      });
  return result;
}
QVariantList DesktopPlugins::sessionPlugins() const {
  QVariantList result;
  for (auto v : rows()) {
    auto e = v.toMap();
    if (e["enabled"].toBool() && e["sessionEnabled"].toBool() &&
        e["hasPage"].toBool())
      result << e;
  }
  return result;
}
QVariantMap DesktopPlugins::session() const {
  auto result = m_session;
  if (!entry(m_page)["permissions"].toList().contains("session.read"))
    result.remove("messages");
  return result;
}
QVariantMap DesktopPlugins::page() const {
  if (m_page.isEmpty())
    return {};
  auto e = entry(m_page);
  auto w = workers.value(m_page);
  return {
      {"id", m_page},
      {"title", e["name"]},
      {"url",
       QUrl::fromLocalFile(packagePath(m_page) + "/" + e["page"].toString())
           .toString()},
      {"config",
       settings.value("config").toMap().value(
           m_page, U::readJson(packagePath(m_page) + "/" +
                                   e.value("config", "config.json").toString(),
                               QVariantMap{}))},
      {"error", m_error},
      {"loading", !w || !w->ready}};
}
QVariantMap DesktopPlugins::details(QString id) const {
  auto e = entry(id);
  if (e.isEmpty())
    return {};
  e["readmeText"] = boundedText(packagePath(id) + "/" +
                                e.value("readme", "README.md").toString());
  auto config = settings.value("config").toMap().value(
      id, U::readJson(packagePath(id) + "/" +
                          e.value("config", "config.json").toString(),
                      QVariantMap{}));
  e["configText"] = QString::fromUtf8(
      QJsonDocument::fromVariant(config).toJson(QJsonDocument::Indented));
  return e;
}
bool DesktopPlugins::save() {
  QVariantMap installed;
  for (auto id : entries.keys())
    if (!entry(id)["builtin"].toBool())
      installed[id] = entry(id);
  auto data = settings;
  data["installed"] = installed;
  QString error;
  if (!U::writeJson(registry, data, &error)) {
    m_status = "插件设置保存失败";
    emit changed();
    return false;
  }
  return true;
}
void DesktopPlugins::synchronize() {
  if (m_closed)
    return;
  bool switched = false;
  auto v = b->view;
  auto key = v->id.isEmpty() ? "draft:" + v->key : v->id;
  if (m_session.value("token").toString() != v->key ||
      m_session.value("workspace").toString() != v->workspace) {
    closePage();
    for (auto id : workers.keys())
      stop(id);
    storageKey = key;
    switched = true;
  } else if (storageKey != key) {
    auto sessions = settings.value("sessions").toMap();
    if (sessions.contains(storageKey)) {
      sessions[key] = sessions.take(storageKey);
      settings["sessions"] = sessions;
      save();
    }
    storageKey = key;
    switched = true;
  }
  QVariantList messages;
  for (auto row : v->messages.mid(qMax(qsizetype(0), v->messages.size() - 8))) {
    auto m = row.toMap();
    messages << QVariantMap{{"role", m["role"]},
                            {"text", m["text"].toString().left(2000)}};
  }
  QVariantMap next{{"token", v->key},           {"id", v->id},
                   {"workspace", v->workspace}, {"title", v->title},
                   {"busy", v->busy()},         {"messages", messages}};
  if (next == m_session)
    return;
  m_session = next;
  emit sessionChanged();
  if (switched)
    emit changed();
  for (auto id : workers.keys())
    send(id, {{"id", U::uuid()},
              {"operation", "session"},
              {"arguments", QVariantMap{{"session", m_session}}}});
  // Headless plugins run only for the current, explicitly enabled session.
  for (auto id : entries.keys())
    if (enabled(id) && sessionEnabled(id) &&
        entry(id)["page"].toString().isEmpty())
      start(id);
}
void DesktopPlugins::setEnabled(QString id, bool value) {
  if (!entries.contains(id))
    return;
  auto options = settings.value("enabled").toMap();
  auto old = settings;
  options[id] = value;
  settings["enabled"] = options;
  if (!save()) {
    settings = old;
    return;
  }
  if (!value) {
    if (m_page == id)
      closePage();
    stop(id);
  } else if (sessionEnabled(id) && entry(id)["page"].toString().isEmpty())
    start(id);
  emit changed();
}
void DesktopPlugins::setSessionEnabled(QString id, bool value) {
  if (!entries.contains(id))
    return;
  QVariantList selected;
  for (auto k : entries.keys())
    if (sessionEnabled(k))
      selected << k;
  selected.removeAll(id);
  if (value)
    selected << id;
  if (selected.size() > 8) {
    emit b->toast("每个会话最多启用 8 个 Umeko 插件");
    return;
  }
  auto old = settings;
  auto sessions = settings.value("sessions").toMap();
  sessions[storageKey] = selected;
  settings["sessions"] = sessions;
  if (!save()) {
    settings = old;
    return;
  }
  if (!value) {
    if (m_page == id)
      closePage();
    stop(id);
  } else if (enabled(id) && entry(id)["page"].toString().isEmpty())
    start(id);
  emit changed();
}
bool DesktopPlugins::openPage(QString id) {
  if (!enabled(id) || !sessionEnabled(id) ||
      entry(id)["page"].toString().isEmpty())
    return false;
  closePage();
  m_page = id;
  m_error.clear();
  start(id);
  emit sessionChanged();
  emit pageChanged();
  emit requested();
  return true;
}
void DesktopPlugins::closePage() {
  if (m_page.isEmpty())
    return;
  auto old = m_page;
  m_page.clear();
  m_error.clear();
  stop(old);
  emit sessionChanged();
  emit pageChanged();
  emit dismissed();
}
void DesktopPlugins::stop(QString id) {
  auto w = workers.take(id);
  if (!w)
    return;
  for (auto request : w->pending)
    emit response(request, {{"ok", false}, {"error", "插件任务已取消"}});
  if (w->process) {
    auto p = w->process;
    disconnect(p, nullptr, this, nullptr);
    if (p->state() == QProcess::Starting)
      p->kill();
    else {
      p->closeWriteChannel();
#ifdef Q_OS_WIN
      QTimer::singleShot(100, p, [this, p] {
        if (p->state() == QProcess::NotRunning)
          return;
        auto killer = new QProcess(this);
        U::hideConsole(killer);
        connect(killer,
                qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                killer, &QObject::deleteLater);
        killer->start(qEnvironmentVariable("SystemRoot", "C:/Windows") +
                          "/System32/taskkill.exe",
                      {"/PID", QString::number(p->processId()), "/T", "/F"});
      });
#else
      p->terminate();
#endif
    }
    QTimer::singleShot(400, p, [p] {
      if (p->state() != QProcess::NotRunning)
        p->kill();
    });
    connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), p,
            &QObject::deleteLater);
  }
}
void DesktopPlugins::start(QString id) {
  if (workers.contains(id) || m_closed)
    return;
  auto w = std::make_shared<Worker>();
  w->token = m_session.value("token").toString();
  workers[id] = w;
  auto p = new QProcess(this);
  w->process = p;
  U::hideConsole(p);
  p->setProcessEnvironment(U::environment());
  connect(p, &QProcess::readyReadStandardOutput, this,
          [this, id, w] { receive(id, w); });
  connect(p, &QProcess::readyReadStandardError, this,
          [p] { p->readAllStandardError(); });
  connect(p, &QProcess::started, this, [this, id, w] {
    auto e = entry(id);
    auto config = settings.value("config").toMap().value(
        id, U::readJson(packagePath(id) + "/" +
                            e.value("config", "config.json").toString(),
                        QVariantMap{}));
    auto payload = QVariantMap{
        {"id", "initialize"},
        {"operation", "initialize"},
        {"arguments",
         QVariantMap{{"path", packagePath(id)},
                     {"config", config},
                     {"session", m_session},
                     {"stateDir", root + "/state/" + id + "/" +
                                      U::sha(storageKey.toUtf8())}}}};
    w->process->write(frame(payload));
  });
  auto failed = [this, id, w] {
    if (workers.value(id) != w)
      return;
    workers.remove(id);
    if (w->process)
      w->process->deleteLater();
    m_error = "插件进程已停止，点击重试可重新打开";
    for (auto r : w->pending)
      emit response(r, {{"ok", false}, {"error", m_error}});
    emit pageChanged();
  };
  connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [failed](int, QProcess::ExitStatus) { failed(); });
  connect(p, &QProcess::errorOccurred, this,
          [failed](QProcess::ProcessError) { failed(); });
  p->start(b->providers->resolvePython(),
           {"-B", b->root + "/plugin-runtime/worker.py"});
  QTimer::singleShot(20000, this, [this, id, w] {
    if (workers.value(id) == w && !w->ready) {
      m_error = "插件启动超时";
      stop(id);
      emit pageChanged();
    }
  });
}
void DesktopPlugins::send(QString id, QVariantMap request) {
  auto w = workers.value(id);
  if (!w)
    return;
  if (!w->ready) {
    if (request["operation"] == "session") {
      for (qsizetype index = w->queue.size(); index > 0; --index)
        if (w->queue[index - 1].toMap()["operation"] == "session")
          w->queue.removeAt(index - 1);
    }
    if (w->queue.size() < 16)
      w->queue << request;
    return;
  }
  w->process->write(frame(request));
}
QString DesktopPlugins::call(QString method, QString raw) {
  auto w = workers.value(m_page);
  if (!w || w->pending.size() >= 8)
    return {};
  QJsonParseError error;
  auto args = QJsonDocument::fromJson(raw.toUtf8(), &error);
  if (error.error != QJsonParseError::NoError || !args.isObject() ||
      raw.size() > 65536 || method.size() > 128)
    return {};
  auto request = U::uuid(), id = m_page;
  w->pending.insert(request);
  send(id, {{"id", request},
            {"operation", "call"},
            {"arguments", QVariantMap{{"method", method},
                                      {"arguments", args.toVariant()}}}});
  QTimer::singleShot(20000, this, [this, id, w, request] {
    if (workers.value(id) == w && w->pending.contains(request)) {
      m_error = "插件操作超时，已停止";
      stop(id);
      emit pageChanged();
    }
  });
  return request;
}
void DesktopPlugins::receive(QString id, std::shared_ptr<Worker> w) {
  if (workers.value(id) != w)
    return;
  w->buffer += w->process->readAllStandardOutput();
  if (w->buffer.size() > 2 * 1024 * 1024) {
    m_error = "插件输出超过限制";
    stop(id);
    emit pageChanged();
    return;
  }
  while (w->buffer.contains('\n')) {
    auto offset = w->buffer.indexOf('\n');
    auto raw = w->buffer.left(offset);
    w->buffer.remove(0, offset + 1);
    auto value = QJsonDocument::fromJson(raw).toVariant().toMap();
    if (value["type"] == "event") {
      if (w->token == m_session.value("token").toString())
        hostEvent(id, value);
      continue;
    }
    auto request = value["id"].toString();
    if (request == "initialize") {
      if (!value["ok"].toBool()) {
        m_error = value["error"].toString();
        stop(id);
        emit pageChanged();
        return;
      }
      w->ready = true;
      auto queue = w->queue;
      w->queue.clear();
      for (auto item : queue)
        send(id, item.toMap());
      emit pageChanged();
    } else if (w->pending.remove(request))
      emit response(request, value);
  }
}
void DesktopPlugins::hostEvent(QString id, QVariantMap event) {
  auto kind = event["event"].toString();
  auto args = event["arguments"].toMap();
  auto perms = entry(id)["permissions"].toList();
  if (kind == "notification") {
    emit b->toast(args["message"].toString().left(1000));
    return;
  }
  if ((kind == "session.send" || kind == "session.attach") &&
      !perms.contains("session.write"))
    return;
  if (kind == "session.send") {
    auto text = args["text"].toString();
    if (text.trimmed().isEmpty() || text.size() > 32000)
      return;
    if (args["steering"].toBool())
      b->steer(text);
    else
      b->send(text);
    return;
  }
  if (kind == "session.attach" || kind == "preview.file") {
    if (!perms.contains("workspace.read"))
      return;
    auto base =
        QFileInfo(m_session["workspace"].toString()).canonicalFilePath();
    auto path = QFileInfo(args["path"].toString()).canonicalFilePath();
    if (base.isEmpty() || path.isEmpty() || !QFileInfo(path).isFile())
      return;
    const auto relative = QDir(base).relativeFilePath(path);
    if (QDir::isAbsolutePath(relative) || relative == ".." || relative.startsWith("../"))
      return;
    if (kind == "session.attach")
      b->attachChatboxFile(path);
    else
      emit b->documentRequested(path, QFileInfo(path).fileName(), "file");
  }
}
void DesktopPlugins::manage(QVariantMap request,
                            std::function<void(QVariantMap)> done) {
  if (m_busy || m_closed)
    return;
  m_busy = true;
  m_status = "正在处理插件…";
  emit changed();
  U::run(
      this, b->providers->resolvePython(),
      {"-B", b->root + "/plugin-runtime/worker.py"}, frame(request),
      [this, done](bool ok, QByteArray raw, QString) {
        m_busy = false;
        auto result =
            QJsonDocument::fromJson(raw.trimmed()).toVariant().toMap();
        if (!ok || result.isEmpty())
          result = {{"ok", false},
                    {"error", "插件管理进程失败，请检查 Python 运行环境"}};
        m_status =
            result["ok"].toBool() ? "操作已完成" : result["error"].toString();
        done(result);
        emit changed();
        emit b->toast(m_status);
      },
      30000);
}
void DesktopPlugins::importZip(QString url) {
  auto path = QUrl(url).toLocalFile();
  if (path.isEmpty()) {
    m_status = "请选择本机插件 ZIP 文件";
    emit changed();
    return;
  }
  QVariantList reserved;
  for (auto id : entries.keys())
    if (entry(id)["builtin"].toBool())
      reserved << id;
  manage({{"id", "install"},
          {"operation", "install"},
          {"arguments",
           QVariantMap{{"archive", path},
                       {"destination", root + "/packages"},
                       {"reserved", reserved},
                       {"configs", settings.value("config").toMap()}}}},
         [this](QVariantMap result) {
           if (!result["ok"].toBool())
             return;
           auto value = result["result"].toMap();
           auto m = value["manifest"].toMap();
           m["builtin"] = false;
           auto id = m["id"].toString();
           auto previous = entries;
           auto options = settings;
           auto oldPath = packagePath(id);
           if (m_page == id)
             closePage();
           stop(id);
           entries[id] = m;
           auto config = settings.value("config").toMap();
           if (!config.contains(id))
             config[id] = value["config"];
           settings["config"] = config;
           if (!save()) {
             entries = previous;
             settings = options;
             return;
           }
           if (!oldPath.isEmpty() && oldPath != packagePath(id))
             QDir(oldPath).removeRecursively();
           m_status = "插件已导入，可在当前会话启用";
         });
}
void DesktopPlugins::saveConfig(QString id, QString raw) {
  if (!entries.contains(id) || raw.size() > 65536)
    return;
  QJsonParseError error;
  auto value = QJsonDocument::fromJson(raw.toUtf8(), &error);
  if (error.error != QJsonParseError::NoError || !value.isObject()) {
    m_status = "请输入有效的 JSON 配置对象";
    emit changed();
    return;
  }
  manage({{"id", "config"},
          {"operation", "config"},
          {"arguments", QVariantMap{{"path", packagePath(id)},
                                    {"config", value.toVariant()}}}},
         [this, id, value](QVariantMap result) {
           if (!result["ok"].toBool())
             return;
           auto old = settings;
           auto config = settings.value("config").toMap();
           config[id] = value.toVariant();
           settings["config"] = config;
           if (!save()) {
             settings = old;
             return;
           }
           if (m_page == id)
             closePage();
           stop(id);
         });
}
bool DesktopPlugins::uninstall(QString id) {
  if (!entries.contains(id) || entry(id)["builtin"].toBool() || m_busy)
    return false;
  auto old = entries;
  auto options = settings;
  auto path = root + "/packages/" + id;
  if (m_page == id)
    closePage();
  stop(id);
  entries.remove(id);
  auto config = settings.value("config").toMap();
  config.remove(id);
  settings["config"] = config;
  auto flags = settings.value("enabled").toMap();
  flags.remove(id);
  settings["enabled"] = flags;
  auto sessions = settings.value("sessions").toMap();
  for (auto key : sessions.keys()) {
    auto ids = sessions[key].toList();
    ids.removeAll(id);
    sessions[key] = ids;
  }
  settings["sessions"] = sessions;
  if (!save()) {
    entries = old;
    settings = options;
    return false;
  }
  QDir(path).removeRecursively();
  emit changed();
  return true;
}
void DesktopPlugins::close() {
  if (m_closed)
    return;
  m_closed = true;
  contextRefresh.stop();
  QList<QPointer<QProcess>> processes;
  for (auto worker : workers)
    processes << worker->process;
  for (auto id : workers.keys())
    stop(id);
  for (auto process : processes)
    if (process && process->state() != QProcess::NotRunning &&
        !process->waitForFinished(300)) {
      process->kill();
      process->waitForFinished(100);
    }
}

bool DesktopPlugins::exportSdk(QString url) {
  auto output = QUrl(url).toLocalFile();
  if (output.isEmpty())
    return false;
  for (auto relative :
       {QString("desktop-plugin-resources/umeko-plugin-sdk.zip"),
        QString("build/desktop-plugin-resources/umeko-plugin-sdk.zip")}) {
    QFile source(b->root + "/" + relative);
    if (!source.open(QIODevice::ReadOnly))
      continue;
    QSaveFile target(output);
    if (target.open(QIODevice::WriteOnly) &&
        target.write(source.readAll()) >= 0 && target.commit())
      return true;
  }
  emit b->toast("无法导出插件 SDK，请重新构建插件开发资源");
  return false;
}
