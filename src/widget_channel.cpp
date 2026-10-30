#include "widget_channel.hpp"
#include "backend.hpp"
#include "desktop_plugins.hpp"
#include <stdexcept>
namespace {
void check(bool ok, QString error) {
  if (!ok)
    throw std::runtime_error(error.toStdString());
}
QByteArray json(QVariant v) {
  return QJsonDocument::fromVariant(v).toJson(QJsonDocument::Compact);
}
QString workspace(QString path) {
  auto value = QFileInfo(path).absoluteFilePath();
#ifdef Q_OS_WIN
  return value.toCaseFolded();
#else
  return value;
#endif
}
bool pending(QVariant value) {
  return QStringList{"pending", "sending", "queued"}.contains(value.toString());
}
} // namespace
WidgetChannel::WidgetChannel(DesktopPlugins *manager)
    : QObject(manager), m(manager) {
  connect(m, &DesktopPlugins::sessionChanged, this, &WidgetChannel::bind);
  connect(m, &DesktopPlugins::response, this,
          [this](QString id, QVariantMap value) {
            if (!calls.contains(id) || calls[id]["status"] != "running")
              return;
            if (json(value).size() > 524288)
              value = {
                  {"ok", false},
                  {"error",
                   "Widget 返回超过 512 KiB，请通过工作区文件提供大型结果"}};
            auto &item = calls[id];
            item["status"] = value["ok"].toBool() ? "completed" : "failed";
            item["result"] = value;
            m->b->finishControlJob(item["jobId"].toString(), value);
          });
  connect(m, &DesktopPlugins::managed, this,
          [this](QString token, QVariantMap value) {
            auto job = management.take(token);
            if (!job.isEmpty())
              m->b->finishControlJob(job, value);
          });
}
QVariantMap WidgetChannel::snapshot(std::shared_ptr<Session> r) const {
  QVariantList messages;
  for (auto value :
       r->messages.mid(qMax(qsizetype(0), r->messages.size() - 8))) {
    auto row = value.toMap();
    messages << QVariantMap{{"role", row["role"]},
                            {"text", row["text"].toString().left(2000)}};
  }
  return {{"token", r->key},   {"id", r->id},       {"workspace", r->workspace},
          {"title", r->title}, {"busy", r->busy()}, {"messages", messages}};
}
QString WidgetChannel::logPath(std::shared_ptr<State> state) const {
  return m->root + "/messages/" + U::sha(state->key.toUtf8()) + ".json";
}
void WidgetChannel::save(std::shared_ptr<State> s) {
  while (s->messages.size() > 100 ||
         (!s->messages.isEmpty() && json(s->messages).size() > 1024 * 1024))
    s->messages.removeFirst();
  if (!U::writeJson(logPath(s), QVariantMap{{"sequence", s->sequence},
                                            {"messages", s->messages}}))
    emit m->b->toast("Widget 消息记录保存失败");
}
std::shared_ptr<WidgetChannel::State>
WidgetChannel::remember(std::shared_ptr<Session> r) {
  const auto key = r->id.isEmpty() ? "draft:" + r->key : r->id;
  auto state = states.value(r->key);
  if (!state)
    for (auto candidate : states)
      if (candidate->key == key &&
          workspace(candidate->snapshot["workspace"].toString()) ==
              workspace(r->workspace)) {
        state = candidate;
        break;
      }
  if (!state) {
    state = std::make_shared<State>();
    state->token = r->key;
    state->key = key;
    state->snapshot = snapshot(r);
    auto file = logPath(state);
    if (QFileInfo(file).size() <= 2 * 1024 * 1024) {
      auto value = U::readJson(file, QVariantMap{}).toMap();
      state->sequence = value["sequence"].toLongLong();
      for (auto item : value["messages"].toList()) {
        auto row = item.toMap();
        if (row["sequence"].toLongLong() < 1 ||
            row["widgetId"].toString().isEmpty())
          continue;
        if (pending(row["delivery"]))
          row["delivery"] = "interrupted";
        state->messages << row;
      }
    }
  }
  bool changed = state->key != key;
  if (workspace(state->snapshot["workspace"].toString()) !=
          workspace(r->workspace) ||
      state->token != r->key) {
    for (auto &item : state->messages) {
      auto row = item.toMap();
      if (pending(row["delivery"])) {
        row["delivery"] = "cancelled";
        row["error"] = "目标聊天引用已改变";
        item = row;
      }
    }
    changed = true;
  }
  state->token = r->key;
  state->key = key;
  state->snapshot = snapshot(r);
  states[r->key] = state;
  if (states.size() > 128)
    for (auto token : states.keys()) {
      auto candidate = states[token];
      if (candidate == state || candidate == current)
        continue;
      bool busy = false;
      for (auto row : candidate->messages)
        busy |= pending(row.toMap()["delivery"]);
      for (auto call : calls)
        busy |= call["token"] == token && call["status"] == "running";
      if (!busy) {
        states.remove(token);
        break;
      }
    }
  if (changed)
    save(state);
  return state;
}
void WidgetChannel::bind() {
  if (m->m_closed)
    return;
  current = remember(m->b->view);
  QTimer::singleShot(0, this, &WidgetChannel::flush);
}
QVariantMap WidgetChannel::require(QString id, bool active) const {
  check(m->entries.contains(id), "Widget ID 不存在");
  check(!active || (m->enabled(id) && m->sessionEnabled(id)),
        "请先在当前会话激活该 Widget");
  return m->entry(id);
}
QString WidgetChannel::document(QString id) const {
  auto entry = require(id);
  auto base = QFileInfo(m->packagePath(id)).canonicalFilePath();
  auto name = entry["agent"].toMap()["document"].toString();
  auto path = QFileInfo(base + "/" + name).canonicalFilePath();
  check(!name.isEmpty() && !base.isEmpty() && path.startsWith(base + "/") &&
            QFileInfo(path).isFile() && QFileInfo(path).size() <= 65536,
        "Widget 缺少可读取的 Agent 使用文档，请更新插件包");
  return path;
}
QVariantList WidgetChannel::list(bool activeOnly,
                                 std::shared_ptr<Session> caller) const {
  QVariantList rows;
  for (auto value : m->rows()) {
    auto e = value.toMap();
    auto id = e["id"].toString();
    bool active = e["enabled"].toBool() &&
                  (m->application(id) ||
                   (e["sessionEnabled"].toBool() && caller == m->b->view));
    if (activeOnly && !active)
      continue;
    QString doc;
    try {
      doc = document(id);
    } catch (...) {
    }
    rows << QVariantMap{
        {"id", id},
        {"name", e["name"]},
        {"version", e["version"]},
        {"summary", e["summary"]},
        {"lifecycle", e["lifecycle"]},
        {"enabled", e["enabled"]},
        {"active", active},
        {"open", m->m_page == id && caller == m->b->view},
        {"running", e["running"]},
        {"starting", e["starting"]},
        {"document", doc},
        {"methods", e["agent"].toMap()["methods"].toMap().keys()}};
  }
  return rows;
}
QVariantMap WidgetChannel::execute(QString operation, QVariantMap args,
                                   QString callerKey) {
  auto b = m->b;
  check(b->controlOperations.contains(operation), "未知 Widget 操作");
  QSet<QString> names;
  for (auto value : b->controlOperations[operation].toMap()["args"].toList()) {
    const auto field = value.toList();
    const auto name = field[0].toString(), kind = field[1].toString();
    names.insert(name);
    check(args.contains(name) || field.size() > 2, "缺少参数：" + name);
    auto v = args.value(name, field.value(2));
    const auto type = v.typeId();
    const bool number = type == QMetaType::Int || type == QMetaType::LongLong ||
                        type == QMetaType::Double;
    const bool valid = kind == "string"    ? type == QMetaType::QString
                       : kind == "boolean" ? type == QMetaType::Bool
                       : kind == "json"
                           ? type == QMetaType::QVariantMap
                           : number && std::isfinite(v.toDouble()) &&
                                 v.toDouble() == std::floor(v.toDouble());
    check(valid, "参数类型无效：" + name);
    args[name] = v;
  }
  for (auto key : args.keys())
    check(names.contains(key), "未知参数：" + key);
  auto target = b->records.value(callerKey);
  if (!target) {
    check(callerKey.isEmpty() || callerKey == b->view->key, "目标聊天已关闭");
    target = b->view;
  }
  auto name = operation.mid(8), id = args["id"].toString();
  check(target == b->view ||
            QStringList{"list", "resource", "messages", "result"}.contains(
                name) ||
            (QStringList{"docs", "call"}.contains(name) && m->application(id)),
        "会话级 Widget 接口绑定当前显示的主会话，请切回该会话后调用");
  auto state = remember(target);
  if (name == "list")
    return {{"widgets", list(args["activeOnly"].toBool(), target)},
            {"sessionId", target->id}};
  if (name == "resource") {
    for (auto prefix :
         {"desktop-plugin-resources/", "build/desktop-plugin-resources/"}) {
      auto path = b->root + "/" + prefix + "umeko-plugin-sdk.zip";
      if (QFileInfo(path).isFile())
        return {{"path", path}};
    }
    check(false, "Widget SDK 资源缺失，请重新构建插件开发资源");
  }
  if (name == "docs") {
    auto e = require(id);
    auto path = document(id);
    QFile f(path);
    check(f.open(QIODevice::ReadOnly), "无法读取文档");
    return {{"id", id},
            {"summary", e["summary"]},
            {"lifecycle", e.value("lifecycle", "session")},
            {"document", QVariantMap{{"path", path},
                                     {"text", QString::fromUtf8(f.readAll())}}},
            {"methods", e["agent"].toMap()["methods"]},
            {"callOperation", "widgets.call"},
            {"resultOperation", "widgets.result"},
            {"updatesOperation", "widgets.messages"}};
  }
  if (name == "messages") {
    if (!id.isEmpty())
      require(id);
    int after = args.value("after", 0).toInt(),
        limit = args.value("limit", 50).toInt();
    check(after >= 0 && limit >= 1 && limit <= 100,
          "after >= 0；limit 必须为 1–100");
    QVariantList found;
    for (auto value : state->messages) {
      auto row = value.toMap();
      if (row["sequence"].toLongLong() > after &&
          (id.isEmpty() || row["widgetId"] == id))
        found << row;
    }
    auto page = found.mid(0, limit);
    return {
        {"updates", page},
        {"nextCursor", page.isEmpty()
                           ? qMax(qint64(after), state->sequence)
                           : page.last().toMap()["sequence"].toLongLong()},
        {"hasMore", found.size() > limit},
        {"historyTruncated",
         !state->messages.isEmpty() &&
             after <
                 state->messages.first().toMap()["sequence"].toLongLong() - 1}};
  }
  if (name == "result") {
    auto request = args["requestId"].toString();
    auto item = calls.value(request);
    auto captured = item["session"].toMap();
    const bool same = !captured["id"].toString().isEmpty() &&
                      !captured["id"].toString().startsWith("pending-") &&
                      captured["id"] == target->id &&
                      workspace(captured["workspace"].toString()) ==
                          workspace(target->workspace);
    check(!item.isEmpty() && (item["token"] == target->key || same),
          "该 Widget 请求不存在或不属于当前会话");
    QVariantList updates;
    for (auto row : state->messages)
      if (row.toMap()["requestId"] == request &&
          row.toMap()["widgetId"] == item["id"])
        updates << row;
    item.remove("token");
    item.remove("session");
    item["requestId"] = request;
    item["updates"] = updates;
    return item;
  }
  QString token;
  if (name == "call") {
    auto e = require(id, true);
    auto method = args["method"].toString();
    auto arguments = args.value("arguments", QVariantMap{});
    int timeout = args.value("timeoutMs", 20000).toInt();
    check(arguments.typeId() == QMetaType::QVariantMap &&
              json(arguments).size() <= 65536,
          "arguments 必须为最多 64 KiB 的 JSON 对象");
    check(e["agent"].toMap()["methods"].toMap().contains(method),
          "该方法未在 Widget 的 Agent 接口中公开，请先调用 widgets.docs");
    check(timeout >= 1000 && timeout <= 120000, "timeoutMs 必须为 1000–120000");
    while (calls.size() >= 64) {
      auto it =
          std::find_if(callOrder.begin(), callOrder.end(), [this](QString key) {
            return calls[key]["status"] != "running";
          });
      check(it != callOrder.end(), "Widget 待完成请求过多，请稍后重试");
      calls.remove(*it);
      callOrder.erase(it);
    }
    token = m->callFor(id, method, QString::fromUtf8(json(arguments)), true,
                       timeout, snapshot(target));
    check(!token.isEmpty(), "Widget 当前无法接受调用");
    auto job = b->newControlJob(operation, "running");
    auto meta = b->controlJobs[job].toMap();
    meta["widgetSession"] = target->key;
    meta["widgetSessionId"] = target->id;
    meta["widgetWorkspace"] = target->workspace;
    b->controlJobs[job] = meta;
    calls[token] = {{"id", id},
                    {"method", method},
                    {"token", target->key},
                    {"session", snapshot(target)},
                    {"status", "running"},
                    {"jobId", job}};
    callOrder << token;
    return {{"status", "accepted"},
            {"requestId", token},
            {"jobId", job},
            {"resultOperation", "widgets.result"}};
  }
  if (name == "import") {
    auto path = args["path"].toString();
    check(QFileInfo(path).isFile(), "请选择存在的 Widget ZIP");
    token = m->importZip(
        QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString());
  } else if (name == "config") {
    require(id);
    check(args["config"].typeId() == QMetaType::QVariantMap,
          "config 必须为 JSON 对象");
    token = m->saveConfig(id, QString::fromUtf8(json(args["config"])));
  } else if (name == "enable" || name == "activate") {
    require(id);
    check(args["enabled"].typeId() == QMetaType::Bool, "enabled 必须是布尔值");
    bool value = args["enabled"].toBool();
    if (name == "enable")
      m->setEnabled(id, value);
    else
      m->setSessionEnabled(id, value);
    check((name == "enable" ? m->enabled(id) : m->sessionEnabled(id)) == value,
          "Widget 设置未能保存");
    return {{"status", "applied"}, {"id", id}, {"enabled", value}};
  } else if (name == "open") {
    require(id, true);
    check(m->openPage(id), "该 Widget 没有可打开的页面");
    return {{"status", "applied"}, {"id", id}};
  } else if (name == "close") {
    m->closePage();
    return {{"status", "applied"}};
  } else if (name == "uninstall") {
    require(id);
    check(m->uninstall(id), "不能卸载内置 Widget，或当前正在管理插件");
    return {{"status", "applied"}};
  } else if (name == "exportSdk") {
    auto path = args["path"].toString();
    check(QDir::isAbsolutePath(path), "path 必须是本机绝对路径");
    check(m->exportSdk(QUrl::fromLocalFile(path).toString()),
          "无法导出 Widget SDK");
    return {{"status", "applied"}, {"path", path}};
  } else
    check(false, "未知 Widget 操作");
  check(!token.isEmpty(), "Widget 管理正在进行，请稍后重试");
  auto job = b->newControlJob(operation, "running");
  auto meta = b->controlJobs[job].toMap();
  meta["widgetSession"] = target->key;
  meta["widgetSessionId"] = target->id;
  meta["widgetWorkspace"] = target->workspace;
  b->controlJobs[job] = meta;
  management[token] = job;
  return {{"status", "accepted"}, {"jobId", job}};
}
void WidgetChannel::reject(QString widget, QString id, QString request,
                           QString error) {
  m->send(widget, {{"id", "delivery-" + id},
                   {"operation", "event"},
                   {"arguments",
                    QVariantMap{{"name", "agent.delivery"},
                                {"data", QVariantMap{{"id", id},
                                                     {"requestId", request},
                                                     {"delivery", "failed"},
                                                     {"error", error}}}}}});
}
void WidgetChannel::notify(QString id, QVariantMap args) {
  if (m->m_closed || !m->enabled(id) || !m->sessionEnabled(id))
    return;
  auto text = args["message"].toString(), eventId = args["id"].toString(),
       request = args["requestId"].toString();
  if (text.trimmed().isEmpty() || text.size() > 8000 ||
      !QRegularExpression("^[a-f0-9]{32}$").match(eventId).hasMatch() ||
      request.size() > 64 || args["data"].typeId() != QMetaType::QVariantMap ||
      args["wake"].typeId() != QMetaType::Bool || json(args).size() > 32768)
    return;
  auto call = calls.value(request);
  auto worker = m->workers.value(id);
  if (!request.isEmpty() &&
      ((!call.isEmpty() && call["id"] != id) ||
       (call.isEmpty() && (!worker || !worker->pending.contains(request)))))
    return;
  auto target = args["session"].toMap();
  if (!call.isEmpty()) {
    if (args.contains("session") && target["token"] != call["token"]) {
      reject(id, eventId, request, "通知的目标聊天与原请求不一致");
      return;
    }
    target = call["session"].toMap();
  } else if (!args.contains("session")) {
    if (m->application(id)) {
      reject(id, eventId, request, "应用级通知必须指定已捕获的目标聊天");
      return;
    }
    target = m->m_session;
  }
  auto state = states.value(target["token"].toString());
  if (!state || workspace(target["workspace"].toString()) !=
                    workspace(state->snapshot["workspace"].toString())) {
    reject(id, eventId, request, "目标聊天引用已失效或工作目录已改变");
    return;
  }
  if (!m->application(id) && state != current)
    return;
  for (auto row : state->messages)
    if (row.toMap()["id"] == eventId)
      return;
  auto now = QDateTime::currentMSecsSinceEpoch();
  auto &rate = rates[id];
  while (!rate.isEmpty() && now - rate.first() > 60000)
    rate.removeFirst();
  if (rate.size() >= 60)
    return;
  rate << now;
  state->messages << QVariantMap{
      {"id", eventId},        {"sequence", ++state->sequence},
      {"widgetId", id},       {"name", m->entry(id)["name"]},
      {"requestId", request}, {"sessionId", state->snapshot["id"]},
      {"session", target},    {"message", text},
      {"data", args["data"]}, {"wake", args["wake"]},
      {"timestamp", now},     {"delivery", "pending"}};
  save(state);
  flush();
}
void WidgetChannel::receipt(std::shared_ptr<State> state, int index,
                            QString status, QString error) {
  auto row = state->messages[index].toMap();
  row["delivery"] = status;
  if (!error.isEmpty())
    row["error"] = error.left(1000);
  state->messages[index] = row;
  save(state);
  QVariantMap data{{"id", row["id"]},
                   {"requestId", row["requestId"]},
                   {"sessionId", state->snapshot["id"]},
                   {"delivery", status}};
  if (!error.isEmpty())
    data["error"] = error;
  auto id = row["widgetId"].toString();
  auto worker = m->workers.value(id);
  if (worker && (m->application(id) || worker->token == state->token))
    m->send(id, {{"id", "delivery-" + row["id"].toString()},
                 {"operation", "event"},
                 {"arguments",
                  QVariantMap{{"name", "agent.delivery"}, {"data", data}}}});
  if (m->m_page == id && state == current)
    emit m->event("agent.delivery", data);
}
void WidgetChannel::stopNotifications(QString id) {
  for (auto s : states)
    for (int i = 0; i < s->messages.size(); ++i) {
      auto row = s->messages[i].toMap();
      if (row["widgetId"] == id && row["delivery"] == "pending")
        receipt(s, i, "cancelled", "插件已停止");
    }
}
void WidgetChannel::flush() {
  if (m->m_closed)
    return;
  QSet<State *> seen;
  for (auto s : states) {
    if (seen.contains(s.get()))
      continue;
    seen.insert(s.get());
    auto b = m->b;
    auto r = b->view->key == s->token ? b->view : b->records.value(s->token);
    if (!r && !s->snapshot["id"].toString().isEmpty())
      r = b->findSession(s->snapshot["id"].toString());
    if (!r || workspace(r->workspace) !=
                  workspace(s->snapshot["workspace"].toString()))
      continue;
    for (int i = 0; i < s->messages.size(); ++i) {
      auto row = s->messages[i].toMap();
      if (row["delivery"] != "pending")
        continue;
      auto id = row["widgetId"].toString();
      if (!m->enabled(id) ||
          (!m->application(id) && (s != current || !m->sessionEnabled(id)))) {
        receipt(s, i, "cancelled");
        continue;
      }
      if (r->busy() && (!r->ready || r->phase != "running"))
        break;
      if (!r->ready && !row["wake"].toBool())
        continue;
      receipt(s, i, "sending");
      row.remove("delivery");
      if (!r->busy() && row["wake"].toBool()) {
        if (!b->command(r, "widget-notify", {{"notification", row}}))
          receipt(s, i, "failed", "Agent 无法开始处理通知");
      } else if (r->ready)
        b->writePi(r, {{"type", "widget-notify"}, {"notification", row}});
      else
        receipt(s, i, "pending");
    }
  }
}
void WidgetChannel::observe(QString key, QVariantMap value) {
  auto s = states.value(key);
  if (!s)
    return;
  auto type = value["type"].toString();
  if (QStringList{"ready", "session", "state"}.contains(type)) {
    auto record = m->b->records.value(key);
    if (record)
      QTimer::singleShot(0, this, [this, record] {
        if (!m->m_closed)
          remember(record);
      });
  }
  if (type == "widget_delivery") {
    for (int i = 0; i < s->messages.size(); ++i)
      if (s->messages[i].toMap()["id"] == value["id"])
        receipt(s, i, value.value("status", "failed").toString(),
                value["error"].toString());
  } else if (type == "ready" || type == "done")
    QTimer::singleShot(0, this, &WidgetChannel::flush);
  else if (type == "exit")
    for (int i = 0; i < s->messages.size(); ++i)
      if (QStringList{"sending", "queued"}.contains(
              s->messages[i].toMap()["delivery"].toString()))
        receipt(s, i, "failed", "Agent 进程已退出");
}
