#include "desktop_plugins.hpp"
#include "backend.hpp"
#include "desktop_pet.hpp"
#include "global_agents.hpp"
#include "widget_channel.hpp"
#include "widget_host.hpp"
#include <QClipboard>
#include <QDesktopServices>
#include <QMimeData>

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
  icons = U::readJson(b->root + "/plugin-runtime/umeko_plugin_sdk/icons.json",
                      QVariantList{})
              .toList();
  applicationToken = "application:" + U::uuid();
  channel = new WidgetChannel(this);
  host = new WidgetHost(this);
  companion = new DesktopPet(this);
  coordinators = new GlobalAgents(this);
  synchronize();
}
DesktopPlugins::~DesktopPlugins() { close(); }
QObject *DesktopPlugins::hostServices() const { return host; }
QString DesktopPlugins::requestHost(QString operation, QString raw) {
  if (raw.toUtf8().size() > 16384) return {};
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(raw.toUtf8(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) return {};
  return host->requestFor(m_page, {{"operation", operation},
      {"arguments", document.object().toVariantMap()}, {"session", m_session}});
}
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
  return entries.contains(id) &&
         settings.value("enabled")
             .toMap()
             .value(id, !application(id) || ((id == "desktop-pet" || id == "global-agents") &&
                                             entry(id)["builtin"].toBool()))
             .toBool();
}
bool DesktopPlugins::sessionEnabled(QString id) const {
  if (application(id))
    return enabled(id);
  auto sessions = settings.value("sessions").toMap();
  return sessions.contains(storageKey)
             ? sessions[storageKey].toList().contains(id)
             : entry(id)["builtin"].toBool();
}
QVariantList DesktopPlugins::rows() const {
  QVariantList result;
  for (auto id : entries.keys()) {
    auto e = entry(id);
    const auto icon = iconFor(id);
    e["iconGlyph"] = icon["iconGlyph"];
    e["iconSource"] = icon["iconSource"];
    e["lifecycle"] = e.value("lifecycle", "session");
    e["running"] = workers.contains(id) && workers.value(id)->ready;
    e["starting"] = workers.contains(id) && !workers.value(id)->ready;
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
      {"iconGlyph", iconFor(m_page)["iconGlyph"]},
      {"iconSource", iconFor(m_page)["iconSource"]},
      {"selection", selection},
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
  e["settingsUrl"] = e["settingsPage"].toString().isEmpty()
                         ? ""
                         : QUrl::fromLocalFile(packagePath(id) + "/" +
                                               e["settingsPage"].toString())
                               .toString();
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
    if (m_page != "global-agents") closePage();
    for (auto id : workers.keys())
      if (!application(id))
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
        (application(id) || entry(id)["page"].toString().isEmpty()))
      start(id);
}
void DesktopPlugins::setEnabled(QString id, bool value) {
  if (!entries.contains(id))
    return;
  if (value && application(id)) {
    int count = 0;
    for (auto other : entries.keys())
      count += other != id && application(other) && enabled(other);
    if (count >= 8) {
      emit b->toast("应用级后台服务最多同时启用 8 个");
      return;
    }
  }
  auto options = settings.value("enabled").toMap();
  auto old = settings;
  options[id] = value;
  settings["enabled"] = options;
  if (!save()) {
    settings = old;
    return;
  }
  if (value)
    failedServices.remove(id);
  if (!value) {
    if (m_page == id)
      closePage();
    stop(id);
  } else if (sessionEnabled(id) &&
             (application(id) || entry(id)["page"].toString().isEmpty()))
    start(id);
  emit changed();
}
void DesktopPlugins::setSessionEnabled(QString id, bool value) {
  if (application(id)) {
    setEnabled(id, value);
    return;
  }
  if (!entries.contains(id))
    return;
  QVariantList selected;
  for (auto k : entries.keys())
    if (!application(k) && sessionEnabled(k))
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
  if (m_page == id && m_error.isEmpty() && workers.contains(id))
    return true;
  failedServices.remove(id);
  closePage();
  m_page = id;
  m_error.clear();
  auto recent = settings["recentPages"].toMap();
  recent[U::cleanPath(m_session["workspace"].toString())] = id;
  settings["recentPages"] = recent;
  save();
  start(id);
  send(id,
       {{"id", U::uuid()},
        {"operation", "event"},
        {"arguments", QVariantMap{{"name", "page.visibility"},
                                  {"data", QVariantMap{{"visible", true}}}}}});
  emit sessionChanged();
  emit pageChanged();
  emit changed();
  emit requested();
  return true;
}
void DesktopPlugins::closePage() {
  if (m_page.isEmpty())
    return;
  auto old = m_page;
  m_page.clear();
  m_error.clear();
  selection.clear();
  send(old,
       {{"id", U::uuid()},
        {"operation", "event"},
        {"arguments", QVariantMap{{"name", "page.visibility"},
                                  {"data", QVariantMap{{"visible", false}}}}}});
  if (!application(old))
    stop(old);
  emit sessionChanged();
  emit pageChanged();
  emit dismissed();
}
void DesktopPlugins::stop(QString id) {
  host->stop(id);
  channel->stopNotifications(id);
  auto w = workers.take(id);
  if (!w)
    return;
  for (auto request : w->pending)
    emit response(request, {{"ok", false}, {"error", "插件任务已取消"}});
  if (id == "desktop-pet" && entry(id)["builtin"].toBool())
    companion->stop();
  if (id == "global-agents" && entry(id)["builtin"].toBool()) coordinators->stop();
  emit changed();
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
  if (workers.contains(id) || m_closed || failedServices.contains(id))
    return;
  auto w = std::make_shared<Worker>();
  w->token =
      application(id) ? applicationToken : m_session.value("token").toString();
  workers[id] = w;
  if ((id == "desktop-pet" || id == "global-agents") && entry(id)["builtin"].toBool()) {
    w->ready = true;
    if (id == "desktop-pet") companion->start(); else coordinators->start();
    emit changed();
    return;
  }
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
                     {"hostCapabilities", host->capabilities()},
                     {"runtimes", QVariantMap{{"node", QVariantMap{{"executable", U::node()}}}}},
                     {"stateDir",
                      root + "/state/" + id + "/" +
                          (application(id) ? QString("application")
                                           : U::sha(storageKey.toUtf8()))}}}};
    w->process->write(frame(payload));
  });
  auto failed = [this, id, w] {
    if (workers.value(id) != w)
      return;
    workers.remove(id);
    host->stop(id);
    if (w->process)
      w->process->deleteLater();
    if (application(id))
      failedServices.insert(id);
    m_error = "插件进程已停止，重新启用可重试";
    for (auto r : w->pending)
      emit response(r, {{"ok", false}, {"error", m_error}});
    emit pageChanged();
    emit changed();
  };
  connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [failed](int, QProcess::ExitStatus) { failed(); });
  connect(p, &QProcess::errorOccurred, this,
          [failed](QProcess::ProcessError) { failed(); });
  p->start(b->providers->resolvePython(),
           {"-B", b->root + "/plugin-runtime/worker.py"});
  QTimer::singleShot(20000, this, [this, id, w] {
    if (workers.value(id) == w && !w->ready) {
      if (application(id))
        failedServices.insert(id);
      m_error = "插件启动超时";
      stop(id);
      emit pageChanged();
    }
  });
}
void DesktopPlugins::send(QString id, QVariantMap request) {
  auto w = workers.value(id);
  if (!w || !w->process)
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
  return callFor(m_page, method, raw);
}
QString DesktopPlugins::settingsCall(QString id, QString method, QString raw) {
  return callFor(id, method, raw);
}
QString DesktopPlugins::callFor(QString id, QString method, QString raw,
                                bool agent, int timeout, QVariantMap snapshot) {
  if (!enabled(id) || !sessionEnabled(id) ||
      (agent &&
       !entry(id)["agent"].toMap()["methods"].toMap().contains(method)))
    return {};
  start(id);
  auto w = workers.value(id);
  if (!w || w->pending.size() >= 8 || raw.toUtf8().size() > 65536 ||
      method.size() > 128)
    return {};
  QJsonParseError error;
  auto args = QJsonDocument::fromJson(raw.toUtf8(), &error);
  if (error.error != QJsonParseError::NoError || !args.isObject())
    return {};
  const auto request = U::uuid();
  w->pending.insert(request);
  if (!w->process) {
    QTimer::singleShot(0, this, [this, id, w, method, args, request, agent] {
      if (workers.value(id) != w)
        return;
      auto result = id == "global-agents" ? coordinators->ui(method, args.object().toVariantMap(), agent) : companion->ui(method, args.object().toVariantMap());
      if (id == "desktop-pet" && method == "importAppearance" && result["ok"].toBool()) {
        const auto token = result["result"].toMap()["importId"].toString();
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection =
            connect(companion, &DesktopPet::imported, this,
                    [this, id, w, request, token,
                     connection](QString done, QVariantMap result) {
                      if (done != token)
                        return;
                      disconnect(*connection);
                      if (workers.value(id) == w && w->pending.remove(request))
                        emit response(request, result);
                    });
        return;
      }
      w->pending.remove(request);
      emit response(request, result);
    });
  } else {
    if (snapshot.isEmpty())
      snapshot = m_session;
    snapshot["language"] = b->language();
    send(id, {{"id", request},
              {"operation", agent ? "agentCall" : "call"},
              {"arguments", QVariantMap{{"method", method},
                                        {"arguments", args.toVariant()},
                                        {"session", snapshot}}}});
  }
  QTimer::singleShot(timeout, this, [this, id, w, request] {
    if (workers.value(id) == w && w->pending.contains(request)) {
      if (application(id))
        failedServices.insert(id);
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
    if (application(id))
      failedServices.insert(id);
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
      if (application(id) || w->token == m_session.value("token").toString())
        hostEvent(id, value);
      continue;
    }
    auto request = value["id"].toString();
    if (request == "initialize") {
      if (!value["ok"].toBool()) {
        if (application(id))
          failedServices.insert(id);
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
      emit changed();
    } else if (w->pending.remove(request))
      emit response(request, value);
  }
}
void DesktopPlugins::hostEvent(QString id, QVariantMap event) {
  if (event["event"] == "host.request") {
    if (event["arguments"].metaType().id() == QMetaType::QVariantMap) {
      const auto args = event["arguments"].toMap();
      if (host->requestFor(id, args).isEmpty() && QRegularExpression("^[a-f0-9]{32}$").match(args["id"].toString()).hasMatch())
        send(id, {{"id",U::uuid()},{"operation","event"},{"arguments",QVariantMap{{"name","host.response"},{"data",QVariantMap{{"requestId",args["id"]},{"operation",args["operation"]},{"ok",false},{"error","Host request queue is full or unavailable"}}}}}});
    }
    return;
  }
  auto kind = event["event"].toString();
  const auto args = event["arguments"].toMap();
  auto perms = entry(id)["permissions"].toList();
  if (kind == "agent.notify") {
    if (perms.contains("session.write"))
      channel->notify(id, args);
    return;
  }
  // Reading a missing session must not insert it: unscoped Widget events
  // target the currently open page (e.g. live terminal output).
  const auto target = args.value("session").toMap();
  if (kind == "widget.event") {
    if (args.contains("session") &&
        (target["token"] != m_session["token"] ||
         target["workspace"] != m_session["workspace"]))
      return;
    const auto topic = args["topic"].toString();
    if (id == m_page && !topic.isEmpty() && topic.size() <= 80 &&
        args["data"].typeId() == QMetaType::QVariantMap &&
        frame(args["data"].toMap()).size() <= 65536)
      emit this->event(topic, args["data"].toMap());
    return;
  }
  if (application(id) && kind != "notification" &&
      target["token"] != m_session["token"])
    return;
  if (kind == "page.open") {
    if (perms.contains("session.write"))
      openPage(id);
    return;
  }
  if (kind == "session.draft") {
    const auto text = args["text"].toString();
    if (perms.contains("session.write") &&
        target["token"] == m_session["token"] && !text.trimmed().isEmpty() &&
        text.size() <= 16000) {
      auto draft = b->view->draft;
      if (!draft.isEmpty() && !draft.endsWith('\n'))
        draft += '\n';
      b->setDraft(draft + text);
      emit b->editorText(b->view->draft);
    }
    return;
  }
  if (QStringList{"files.copy", "file.openWith", "file.reveal",
                  "explorer.reveal"}
          .contains(kind)) {
    const auto base =
        QFileInfo(m_session["workspace"].toString()).canonicalFilePath();
    if (!perms.contains("workspace.read") || base.isEmpty())
      return;
    auto valid = [&](QString path) {
      auto canonical = QFileInfo(path).canonicalFilePath();
      auto rel = QDir(base).relativeFilePath(canonical);
      return !canonical.isEmpty() && !QDir::isAbsolutePath(rel) &&
             rel != ".." && !rel.startsWith("../");
    };
    if (kind == "files.copy") {
      auto paths = args["paths"].toList();
      if (paths.isEmpty() || paths.size() > 100 ||
          (args["cut"].toBool() && !perms.contains("workspace.write")))
        return;
      QList<QUrl> urls;
      for (auto path : paths) {
        if (!valid(path.toString()))
          return;
        urls << QUrl::fromLocalFile(
            QFileInfo(path.toString()).canonicalFilePath());
      }
      auto mime = new QMimeData;
      mime->setUrls(urls);
      const auto action = args["cut"].toBool() ? "cut" : "copy";
      QByteArray gnome = QByteArray(action) + "\n";
      for (auto url : urls)
        gnome += url.toEncoded() + "\n";
      mime->setData("x-special/gnome-copied-files", gnome);
#ifdef Q_OS_WIN
      QByteArray effect(4, '\0');
      effect[0] = args["cut"].toBool() ? 2 : 1;
      mime->setData(
          "application/x-qt-windows-mime;value=\"Preferred DropEffect\"",
          effect);
#endif
      QGuiApplication::clipboard()->setMimeData(mime);
      return;
    }
    auto path = QFileInfo(args["path"].toString()).absoluteFilePath();
    if (kind == "file.reveal" && !QFileInfo::exists(path))
      path = QFileInfo(path).absolutePath();
    if (!valid(path))
      return;
    if (kind == "file.reveal")
      b->openChatboxPath(path, QFileInfo(path).isFile());
    else if (kind == "explorer.reveal") {
      if (openPage("file-tree")) {
        selection = QDir(base).relativeFilePath(path);
        emit pageChanged();
      }
    } else {
#ifdef Q_OS_WIN
      auto process = new QProcess(this);
      U::hideConsole(process);
      connect(process, &QProcess::finished, process, &QObject::deleteLater);
      process->start("rundll32.exe", {"shell32.dll,OpenAs_RunDLL", path});
#else
      emit openWithRequested(path);
#endif
    }
    return;
  }
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
    if (QDir::isAbsolutePath(relative) || relative == ".." ||
        relative.startsWith("../"))
      return;
    if (kind == "session.attach")
      b->attachChatboxFile(path);
    else
      emit b->documentRequested(path, QFileInfo(path).fileName(), "file");
  }
}
QString DesktopPlugins::manage(QVariantMap request,
                               std::function<bool(QVariantMap)> done) {
  if (m_busy || m_closed)
    return {};
  auto token = U::uuid();
  m_busy = true;
  m_status = "正在处理插件…";
  emit changed();
  U::run(
      this, b->providers->resolvePython(),
      {"-B", b->root + "/plugin-runtime/worker.py"}, frame(request),
      [this, done, token](bool ok, QByteArray raw, QString) {
        m_busy = false;
        auto result =
            QJsonDocument::fromJson(raw.trimmed()).toVariant().toMap();
        if (!ok || result.isEmpty())
          result = {{"ok", false},
                    {"error", "插件管理进程失败，请检查 Python 运行环境"}};
        m_status =
            result["ok"].toBool() ? "操作已完成" : result["error"].toString();
        if (m_closed)
          return;
        if (!done(result))
          result = {{"ok", false}, {"error", m_status}};
        emit changed();
        emit b->toast(m_status);
        emit managed(token, result);
      },
      30000);
  return token;
}
QString DesktopPlugins::importZip(QString url) {
  auto path = QUrl(url).toLocalFile();
  if (path.isEmpty())
    return {};
  QVariantList reserved;
  for (auto id : entries.keys())
    if (entry(id)["builtin"].toBool())
      reserved << id;
  return manage({{"id", "install"},
                 {"operation", "install"},
                 {"arguments", QVariantMap{{"archive", path},
                                           {"destination", root + "/packages"},
                                           {"reserved", reserved},
                                           {"configs", settings["config"]}}}},
                [this](QVariantMap result) {
                  if (!result["ok"].toBool())
                    return true;
                  auto value = result["result"].toMap(),
                       m = value["manifest"].toMap();
                  m["builtin"] = false;
                  auto id = m["id"].toString();
                  auto previous = entries, options = settings;
                  auto oldPath = packagePath(id);
                  if (m_page == id)
                    closePage();
                  stop(id);
                  entries[id] = m;
                  auto config = settings["config"].toMap();
                  if (!config.contains(id))
                    config[id] = value["config"];
                  settings["config"] = config;
                  if (application(id)) {
                    auto flags = settings["enabled"].toMap();
                    flags[id] = false;
                    settings["enabled"] = flags;
                  }
                  if (!save()) {
                    entries = previous;
                    settings = options;
                    return false;
                  }
                  if (!oldPath.isEmpty() && oldPath != packagePath(id))
                    QDir(oldPath).removeRecursively();
                  m_status = application(id)
                                 ? "后台服务已导入，请启用以随应用运行"
                                 : "插件已导入，可在当前会话启用";
                  return true;
                });
}
QString DesktopPlugins::saveConfig(QString id, QString raw) {
  if (!entries.contains(id) || raw.toUtf8().size() > 65536)
    return {};
  QJsonParseError error;
  auto doc = QJsonDocument::fromJson(raw.toUtf8(), &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject()) {
    m_status = "请输入有效的 JSON 配置对象";
    emit changed();
    return {};
  }
  if (id == "desktop-pet" && entry(id)["builtin"].toBool()) {
    auto token = U::uuid();
    QTimer::singleShot(0, this, [this, token, doc] {
      auto result = companion->ui("configure", doc.object().toVariantMap());
      emit managed(token, result);
    });
    return token;
  }
  return manage({{"id", "config"},
                 {"operation", "config"},
                 {"arguments", QVariantMap{{"path", packagePath(id)},
                                           {"config", doc.toVariant()}}}},
                [this, id, doc](QVariantMap result) {
                  if (!result["ok"].toBool())
                    return true;
                  auto old = settings;
                  auto config = settings["config"].toMap();
                  config[id] = doc.toVariant();
                  settings["config"] = config;
                  if (!save()) {
                    settings = old;
                    return false;
                  }
                  if (m_page == id)
                    closePage();
                  stop(id);
                  if (application(id) && enabled(id)) {
                    failedServices.remove(id);
                    start(id);
                  }
                  return true;
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
  coordinators->close();
  companion->close();
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

bool DesktopPlugins::application(QString id) const {
  return entry(id).value("lifecycle", "session") == "application";
}
QObject *DesktopPlugins::pet() const { return companion; }
QVariantMap DesktopPlugins::iconFor(QString id) const {
  const auto icon = entry(id).value("icon", "puzzle");
  if (icon.typeId() == QMetaType::QString)
    for (auto row : icons)
      if (row.toMap()["id"] == icon)
        return {{"iconGlyph", row.toMap()["glyph"]}, {"iconSource", ""}};
  const auto folder = QFileInfo(packagePath(id)).canonicalFilePath(),
             path = QFileInfo(packagePath(id) + "/" +
                              icon.toMap()["path"].toString())
                        .canonicalFilePath();
  if (!icon.toMap().isEmpty() && !folder.isEmpty() &&
      path.startsWith(folder + "/") && QFileInfo(path).isFile())
    return {{"iconGlyph", "puzzle"},
            {"iconSource", QUrl::fromLocalFile(path).toString()}};
  return {{"iconGlyph", "puzzle"}, {"iconSource", ""}};
}
QVariantMap DesktopPlugins::recentPlugin() const {
  auto pages = sessionPlugins();
  auto id = settings["recentPages"]
                .toMap()[U::cleanPath(m_session["workspace"].toString())];
  for (auto p : pages)
    if (p.toMap()["id"] == id)
      return p.toMap();
  for (auto p : pages) if (!application(p.toMap()["id"].toString())) return p.toMap();
  return pages.isEmpty() ? QVariantMap{} : pages.first().toMap();
}
QVariantMap DesktopPlugins::execute(QString operation, QVariantMap args,
                                    QString callerKey) {
  return channel->execute(operation, args, callerKey);
}
void DesktopPlugins::observe(QString key, QVariantMap value) {
  channel->observe(key, value);
  coordinators->observe(key, value);
  host->observe(key, value);
}
