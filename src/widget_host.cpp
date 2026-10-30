#include "widget_host.hpp"
#include "backend.hpp"
#include <stdexcept>

namespace {
const QMap<QString, QString> commands{
    {"pi.scan", "extensions.scan"},
    {"pi.refresh", "extensions.refresh"},
    {"pi.enable", "extensions.enable"},
    {"pi.install", "extensions.install"},
    {"pi.remove", "extensions.remove"},
    {"pi.checkUpdate", "extensions.checkUpdate"},
    {"pi.details", "extensions.details"},
    {"pi.activation", "extensions.activation"}};
bool mutation(const QString &operation) {
  return QStringList{"pi.refresh", "pi.enable", "pi.install", "pi.remove", "pi.activation"}
      .contains(operation);
}
void require(bool valid, QString error) {
  if (!valid)
    throw std::runtime_error(error.toStdString());
}
QString key(const QString &widget, const QString &request) {
  return widget + "/" + request;
}
} // namespace

WidgetHost::WidgetHost(DesktopPlugins *manager)
    : QObject(manager), m(manager), b(manager->b) {
  activityTimer.setSingleShot(true);
  activityTimer.setInterval(200);
  connect(&activityTimer, &QTimer::timeout, this, &WidgetHost::flushActivity);
}

QVariantMap WidgetHost::capabilities() const {
  QVariantMap operations;
  auto names = commands.keys();
  names << "pi.status" << "pi.job" << "pi.network" << "pi.cancel";
  for (const auto &name : names)
    operations[name] =
        QVariantMap{{"permission", mutation(name) || name=="pi.cancel" ? "pi.manage" : "pi.read"},
                    {"confirmation", mutation(name)}};
  return {{"version", 1},
          {"operations", operations},
          {"requestBytes", 16384},
          {"responseBytes", 262144},
          {"mutationsRequireIdle", true},
          {"runtimes", QStringList{"node"}},
          {"events",
           QVariantMap{{"pi.activity", QVariantMap{{"permission", "pi.observe"},
                                                   {"maxBytes", 65536},
                                                   {"intervalMs", 200}}}}}};
}

void WidgetHost::owner(Task request) const {
  require(!m->m_closed && m->workers.value(request->widget) == request->worker,
          "Widget instance has stopped");
  require(m->enabled(request->widget) && m->sessionEnabled(request->widget),
          "Widget is not enabled");
  require(!request->session["token"].toString().isEmpty() &&
              request->session["token"] == m->m_session["token"] &&
              request->session["workspace"] == m->m_session["workspace"],
          "The target chat or workspace changed; retry from its page");
}

void WidgetHost::reply(Task request, QVariantMap value) {
  requests.remove(key(request->widget, request->id));
  if (m->workers.value(request->widget) != request->worker)
    return;
  if (QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact).size() >
      262144)
    value = {{"ok", false},
             {"error", "Host response exceeds 256 KiB; narrow the query"}};
  value["requestId"] = request->id;
  value["operation"] = request->operation;
  emit m->hostResponse(request->widget, request->id, value);
  m->send(request->widget, {{"id", U::uuid()},
                            {"operation", "event"},
                            {"arguments", QVariantMap{{"name", "host.response"},
                                                      {"data", value}}}});
}

QString WidgetHost::requestFor(QString id, QVariantMap value) {
  auto worker = m->workers.value(id);
  if (!worker)
    return {};
  auto identifier =
      value.value("id", QUuid::createUuid().toString(QUuid::Id128)).toString();
  if (!QRegularExpression("^[a-f0-9]{32}$").match(identifier).hasMatch())
    return {};
  int count = 0;
  for (const auto &request : requests)
    if (request->widget == id)
      ++count;
  if (count >= 8 || requests.contains(key(id, identifier)))
    return {};
  auto request = std::make_shared<Request>();
  request->widget = id;
  request->worker = worker;
  request->id = identifier;
  request->operation = value["operation"].toString();
  request->arguments = value.value("arguments", QVariantMap{});
  request->session = value["session"].toMap();
  requests[key(id, identifier)] = request;
  QTimer::singleShot(0, this, [this, request] { begin(request); });
  return identifier;
}

void WidgetHost::begin(Task request) {
  if (requests.value(key(request->widget, request->id)) != request)
    return;
  try {
    owner(request);
    const auto operation = request->operation;
    require(capabilities()["operations"].toMap().contains(operation),
            "Host operation is not available; inspect hostCapabilities");
    require(request->arguments.metaType().id() == QMetaType::QVariantMap &&
                QJsonDocument::fromVariant(request->arguments)
                        .toJson(QJsonDocument::Compact)
                        .size() <= 16384,
            "Host arguments must be a JSON object of at most 16 KiB");
    const auto args = request->arguments.toMap();
    const auto permission = mutation(operation) || operation=="pi.cancel" ? "pi.manage" : "pi.read";
    require(
        m->entry(request->widget)["permissions"].toList().contains(permission),
        QString("Widget permission is required: ") + permission);
    const QMap<QString, QStringList> fields{
        {"pi.status", {"offset", "limit", "query", "details"}},
        {"pi.scan", {}},
        {"pi.network", {}},
        {"pi.refresh", {}},
        {"pi.enable", {"id", "enabled"}},
        {"pi.install", {"source", "project"}},
        {"pi.job", {"id"}},
        {"pi.cancel", {"id"}},
        {"pi.remove", {"id"}},
        {"pi.checkUpdate", {"id"}},
        {"pi.details", {"id"}},
        {"pi.activation", {"id","mode"}}};
    for (auto it = args.cbegin(); it != args.cend(); ++it)
      require(fields[operation].contains(it.key()), "Unknown host arguments");
    if (operation == "pi.enable" || operation == "pi.job" || operation=="pi.cancel" ||
        operation == "pi.remove" || operation == "pi.checkUpdate" || operation == "pi.details" || operation == "pi.activation")
      require(args["id"].metaType().id() == QMetaType::QString &&
                  !args["id"].toString().isEmpty() &&
                  args["id"].toString().size() <= 4096,
              "A valid id is required");
    if (operation == "pi.enable")
      require(args["enabled"].metaType().id() == QMetaType::Bool,
              "enabled must be boolean");
    if(operation=="pi.activation") require(QStringList{"global","session","disabled"}.contains(args["mode"].toString()),"Invalid activation mode");
    if (operation == "pi.install") {
      require(args["source"].metaType().id() == QMetaType::QString &&
                  !args["source"].toString().trimmed().isEmpty() &&
                  args["source"].toString().size() <= 4096,
              "A valid package source is required");
      require(!args.contains("project") ||
                  args["project"].metaType().id() == QMetaType::Bool,
              "project must be boolean");
    }
    if (operation == "pi.status") {
      for(const auto &field:{QString("query"),QString("details")}) if(args.contains(field)) require(args[field].metaType().id()==QMetaType::QString && args[field].toString().size()<=256,"Invalid local catalog query");
      for (const auto &name : {QString("offset"), QString("limit")}) {
        const auto value = args.value(name, name == "offset" ? 0 : 100);
        require(value.metaType().id() != QMetaType::Bool &&
                    (value.metaType().id() == QMetaType::Int ||
                     value.metaType().id() == QMetaType::LongLong ||
                     value.metaType().id() == QMetaType::Double) &&
                    value.toDouble() == value.toInt() &&
                    value.toInt() >= (name == "offset" ? 0 : 1) &&
                    value.toInt() <= (name == "offset" ? 1000000 : 200),
                "Invalid offset or limit");
      }
    }
    if(operation=="pi.activation" || operation=="pi.enable")
      for(const auto &value:b->management->extensions()) {
        const auto row=value.toMap();
        if(row["id"]==args["id"]) require(!row["info"].toMap()["required"].toBool(),"Umeko 基础能力固定启用，不能修改启用范围");
      }
    if (mutation(operation)) {
      require(!b->view->busy() && !b->management->busy() && !b->kernelUpdating,
              "Pi is busy; Agent installs must use run_pi_command and its "
              "native queued reload");
      require(!pending && b->view->approval.isEmpty(),
              "Another confirmation is pending");
      pending = request;
      approval = {
          {"id", request->id},
          {"method", "confirm"},
          {"title", "Widget · " + m->entry(request->widget)["name"].toString()},
          {"detail",
           operation + "\n" +
               QString::fromUtf8(QJsonDocument::fromVariant(args).toJson(
                   QJsonDocument::Indented))}};
      emit changed();
      QTimer::singleShot(120000, this, [this, request] {
        if (pending == request)
          respondApproval(false);
      });
      return;
    }
    execute(request);
  } catch (const std::exception &error) {
    reply(request, {{"ok", false}, {"error", QString::fromUtf8(error.what())}});
  }
}

void WidgetHost::respondApproval(bool allowed) {
  auto request = pending;
  pending.reset();
  approval.clear();
  emit changed();
  if (!request)
    return;
  if (!allowed) {
    reply(request, {{"ok", false},
                    {"status", "declined"},
                    {"error", "User declined the Widget operation"}});
    return;
  }
  try {
    execute(request);
  } catch (const std::exception &error) {
    reply(request, {{"ok", false}, {"error", QString::fromUtf8(error.what())}});
  }
}

void WidgetHost::execute(Task request) {
  owner(request);
  const auto operation = request->operation;
  auto args = request->arguments.toMap();
  if(operation=="pi.activation" || operation=="pi.details") {
    bool found=false; for(const auto &row:b->management->extensions()) found|=row.toMap()["id"]==args["id"];
    require(b->management->extensionsCurrent() && found,"Refresh the current local inventory first");
  }
  if (operation == "pi.remove") {
    require(b->management->extensionsCurrent(),
            "Refresh the current workspace before removing a package");
    QVariantMap row;
    for (const auto &item : b->management->extensions())
      if (item.toMap()["id"] == args["id"])
        row = item.toMap();
    const auto source = row["source"].toString();
    require(!source.isEmpty() && source != "builtin" && source != "local" &&
                !b->store.data["agent"].toMap()["safeMode"].toBool(),
            "Only a registered third-party package outside recovery mode can "
            "be removed");
  }
  if (operation == "pi.network") {
    const auto settings = b->management->npmConfig();
    QVariantMap result;
    for (const auto &name : {"npmRegistry", "npmUserconfig", "npmCafile"})
      result[name] = settings.value(name, "");
    reply(request, {{"ok", true}, {"result", result}});
    return;
  }
  if (operation == "pi.status") {
    const auto current=b->management->extensionsCurrent();
    const auto all=current ? b->management->extensions() : QVariantList{};
    const auto query=args["query"].toString(), detailId=args["details"].toString();
    QVariantList rows;
    for(const auto &value:all) {
      const auto row=value.toMap();
      const auto text=row["name"].toString()+" "+row["path"].toString()+" "+row["info"].toMap()["description"].toString();
      if(!detailId.isEmpty() ? row["id"]==detailId : query.isEmpty() || text.contains(query,Qt::CaseInsensitive)) rows<<value;
    }
    const auto offset=args.value("offset",0).toInt(),limit=args.value("limit",100).toInt();
    const auto page=rows.mid(offset,limit);
    QSet<QString> paths; QVariantMap modes;
    for(const auto &value:page) { const auto path=value.toMap()["path"].toString(); paths.insert(path); if(b->view->extensionModes.contains(path))modes[path]=b->view->extensionModes[path]; }
    QVariantMap result{{"busy",b->management->busy()},{"status",current ? b->management->status() : QString()},
      {"extensions",page},{"total",rows.size()},{"catalogTotal",all.size()},{"hasMore",offset+limit<rows.size()},
      {"contextCurrent",current},{"workspace",request->session["workspace"]},{"activationModes",modes},
      {"extensionUpdate",current ? b->management->extensionUpdate() : QVariantMap{}}};
    if(!detailId.isEmpty()) {
      const auto doc=b->management->extensionDetails(); result["details"]=current && doc["id"]==detailId ? doc : QVariantMap{};
      QVariantList tools;
      for(const auto &value:b->view->resources["allTools"].toList()) {
        const auto tool=value.toMap(); if(!paths.contains(tool["source"].toString()))continue;
        tools<<QVariantMap{{"name",tool["name"].toString().left(256)},{"description",tool["description"].toString().left(4096)}};
        if(tools.size()>=32)break;
      }
      result["tools"]=tools;
    }
    reply(request,{{"ok",true},{"result",result}}); return;
  }
  if (operation == "pi.job" || operation=="pi.cancel") {
    auto old = jobs.value(args["id"].toString());
    require(old && old->widget == request->widget &&
                old->worker == request->worker &&
                old->session["token"] == request->session["token"] &&
                old->session["workspace"] == request->session["workspace"],
            "Host job does not belong to this Widget instance and chat");
    auto result = b->umekoRequest(
        {{"operation", "jobs.get"}, {"arguments", args}}, b->view->key, false);
    const auto ownedJobs=result["jobs"].toList();
    const auto active=ownedJobs.isEmpty() ? QVariantMap{} : ownedJobs.first().toMap();
    if(operation=="pi.cancel") {
      require(active["operation"]=="extensions.install" && active["status"]=="running","This Widget has no running install to cancel");
      b->management->cancelInstall();reply(request,{{"ok",true},{"status","cancelling"}});return;
    }
    if(active["operation"]=="extensions.install" && active["status"]=="running")
      result["progress"]=QVariantMap{{"message",b->management->installMessage().left(800)},{"state",b->management->installState()},{"cancellable",b->management->installCancellable()}};
    if (result["ok"].toBool()) {
      QVariantList summaries;
      for (const auto &value : result["jobs"].toList()) {
        const auto item = value.toMap(), detail = item["result"].toMap();
        QVariantMap summary, filtered;
        for (const auto &name : {"id", "operation", "status"})
          summary[name] = item[name];
        for (const auto &name : {"ok", "error"})
          if (detail.contains(name))
            filtered[name] = detail[name];
        summary["result"] = filtered;
        summaries << summary;
      }
      result["jobs"] = summaries;
    }
    reply(request, result);
    return;
  }
  require(!b->management->busy() && !b->kernelUpdating,
          "Pi management is busy; retry when it finishes");
  require(!mutation(operation) || !b->view->busy(),
          "The task became busy; no Widget change was applied");
  auto result = operation == "pi.install"
                    ? b->confirmedWidgetInstall(args, b->view->key)
                    : b->umekoRequest({{"operation", commands[operation]},
                                       {"arguments", args}},
                                      b->view->key, false);
  const auto job = result["jobId"].toString();
  if (!job.isEmpty()) {
    jobs[job] = request;
    jobOrder << job;
    while (jobOrder.size() > 128)
      jobs.remove(jobOrder.takeFirst());
  }
  reply(request, result);
}

void WidgetHost::stop(QString id) {
  activity.remove(id);
  if (pending && pending->widget == id)
    respondApproval(false);
  for (auto it = jobs.begin(); it != jobs.end();) {
    if (it.value()->widget == id) {
      if(it.value()->operation=="pi.install" && b->controlJobs.value(it.key()).toMap()["status"]=="running") b->management->cancelInstall();
      jobOrder.removeAll(it.key());
      it = jobs.erase(it);
    } else
      ++it;
  }
  for (auto it = requests.begin(); it != requests.end();) {
    if (it.value()->widget == id)
      it = requests.erase(it);
    else
      ++it;
  }
}

void WidgetHost::observe(QString key, QVariantMap event) {
  if (event["type"] != "tool" ||
      !QStringList{"done", "error", "interrupted"}.contains(
          event["state"].toString()))
    return;
  auto record = b->records.value(key);
  if (!record)
    return;
  QString source;
  for (const auto &value : record->resources["allTools"].toList())
    if (value.toMap()["name"] == event["name"]) {
      source = value.toMap()["source"].toString();
      break;
    }
  const QVariantMap item{
      {"kind", "toolFinished"},
      {"id", event["id"].toString().left(160)},
      {"name", event["name"].toString().left(160)},
      {"state", event["state"]},
      {"source", source.left(2048)},
      {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
      {"session", QVariantMap{{"token", record->key},
                              {"id", record->id},
                              {"workspace", record->workspace}}}};
  for (auto it = m->workers.cbegin(); it != m->workers.cend(); ++it) {
    const auto id = it.key();
    auto worker = it.value();
    if (!worker->ready ||
        !m->entry(id)["permissions"].toList().contains("pi.observe") ||
        (!m->application(id) && worker->token != record->key))
      continue;
    auto &queue = activity[id];
    if (queue.worker != worker)
      queue = {worker, {}, 0};
    queue.events << item;
    while (queue.events.size() > 64 || QJsonDocument::fromVariant(queue.events)
                                               .toJson(QJsonDocument::Compact)
                                               .size() > 64000) {
      queue.events.removeFirst();
      ++queue.dropped;
    }
  }
  if (!activity.isEmpty() && !activityTimer.isActive())
    activityTimer.start();
}

void WidgetHost::flushActivity() {
  auto queues = std::move(activity);
  activity.clear();
  for (auto it = queues.cbegin(); it != queues.cend(); ++it) {
    if (m->workers.value(it.key()) != it->worker || !m->enabled(it.key()))
      continue;
    m->send(it.key(),
            {{"id", U::uuid()},
             {"operation", "event"},
             {"arguments",
              QVariantMap{{"name", "pi.activity"},
                          {"data", QVariantMap{{"version", 1},
                                               {"events", it->events},
                                               {"dropped", it->dropped}}}}}});
  }
}
