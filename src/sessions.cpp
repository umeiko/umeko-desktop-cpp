#include "backend.hpp"

std::shared_ptr<Session> Backend::fresh(QString workspace) {
  auto s = std::make_shared<Session>();
  s->workspace = workspace;
  s->model = store.data["selectedModel"].toString();
  s->provider = store.data["selectedProvider"].toString();
  static const QStringList greetings = {
      "来啦？今天想先做点什么？",
      "先把今天的难题解决掉，再去吃点好吃的吧。",
      "我已经准备好了，就等你开口啦。",
      "零食先放一边，陪你把这件事做完。",
      "有点棘手？没关系，我们慢慢拆开看。",
      "今天的计划是什么？我来搭把手。",
      "又见面啦，把想做的事告诉我吧。",
      "先从一小步开始，做完再奖励自己。",
      "这次想写点什么？我已经好奇起来了。",
      "忙了一天了吧？这道难题我们一起看。",
      "工作可以认真，休息和好吃的也不能少。",
      "好啦，开工吧。今天也请多关照。"};
  s->greeting =
      greetings[QRandomGenerator::global()->bounded(greetings.size())];
  s->portrait = QRandomGenerator::global()->bounded(12);
  records[s->key] = s;
  lru.removeAll(s->key);
  lru << s->key;
  return s;
}
std::shared_ptr<Session> Backend::findSession(QString id) {
  for (auto s : records)
    if (s->id == id && !id.isEmpty())
      return s;
  return {};
}
void Backend::markRead(std::shared_ptr<Session> s) {
  if (!s->id.isEmpty() && chatVisible)
    setMetadata(s->id, {{"unread", false}});
}
void Backend::activate(std::shared_ptr<Session> s) {
  streaming.stop();
  view = s;
  lru.removeAll(s->key);
  lru << s->key;
  if (!s->workspace.isEmpty()) {
    auto p = ensureProject(s->workspace);
    if (!p.isEmpty())
      store.data["activeProject"] = p["id"];
    store.data["workspace"] = s->workspace;
  }
  bool valid = false;
  for (auto v : modelCatalog()) {
    auto m = v.toMap();
    if (m["provider"] == s->provider && m["id"] == s->model)
      valid = true;
  }
  if (!valid && !s->busy()) {
    repairSelection();
    s->provider = store.data["selectedProvider"].toString();
    s->model = store.data["selectedModel"].toString();
    s->usage.clear();
  } else if (valid) {
    store.data["selectedProvider"] = s->provider;
    store.data["selectedModel"] = s->model;
  }
  markRead(s);
  store.save();
  emit changed();
  emit chatChanged();
  emit viewChanged();
  emit attachmentsChanged();
  emit approvalChanged();
  emit contextUsageChanged();
  emit editorText(s->draft);
  trimPool();
}
void Backend::trimPool() {
  int active = 0;
  for (auto s : records)
    if (s->process)
      ++active;
  for (auto key : lru) {
    if (active <= 8)
      break;
    auto s = records.value(key);
    if (s && s != view && s->process && !s->busy()) {
      retire(s);
      --active;
    }
  }
}
void Backend::retire(std::shared_ptr<Session> s) {
  if (!s->process)
    return;
  auto p = s->process;
  s->process = nullptr;
  s->ready = false;
  s->stale = false;
  disconnect(p, nullptr, this, nullptr);
  p->write(QJsonDocument(QJsonObject{{"type", "shutdown"}})
               .toJson(QJsonDocument::Compact) +
           '\n');
  p->closeWriteChannel();
  connect(p, &QProcess::finished, p, &QObject::deleteLater);
  QTimer::singleShot(4000, p, [p] {
    if (p->state() != QProcess::NotRunning)
      p->kill();
  });
}
void Backend::invalidate() {
  for (auto s : records) {
    if (s->busy())
      s->stale = true;
    else {
      retire(s);
      s->usage.clear();
    }
  }
  emit contextUsageChanged();
}
void Backend::indexSession(std::shared_ptr<Session> s) {
  if (s->id.isEmpty())
    return;
  QVariantMap info{
      {"id", s->id},       {"file", s->file},   {"workspace", s->workspace},
      {"title", s->title}, {"model", s->model}, {"provider", s->provider}};
  for (auto &v : sessionIndex)
    if (v.toMap()["id"] == s->id) {
      v = info;
      return;
    }
  sessionIndex.prepend(info);
}
void Backend::sessionInfo(std::shared_ptr<Session> s, QVariantMap info) {
  auto old = s->id;
  s->id = info.value("id", s->id).toString();
  if (!info["file"].toString().isEmpty())
    s->file = info["file"].toString();
  if (!info["title"].toString().isEmpty())
    s->title = info["title"].toString();
  if (!info["workspace"].toString().isEmpty())
    s->workspace = info["workspace"].toString();
  if (!old.isEmpty() && old != s->id) {
    for (int i = sessionIndex.size() - 1; i >= 0; --i)
      if (sessionIndex[i].toMap()["id"] == old)
        sessionIndex.removeAt(i);
    auto all = store.data["sessionMeta"].toMap();
    auto meta = all.take(old).toMap();
    all[s->id] = meta;
    store.data["sessionMeta"] = all;
  }
  if (!s->file.isEmpty() || s->action == "prompt")
    indexSession(s);
}
void Backend::saveSessions() { store.saveSessions(sessionIndex); }
void Backend::newSession() {
  auto s = fresh(store.data["workspace"].toString());
  drafts[s->workspace] = s;
  activate(s);
}
void Backend::openSession(QString id) {
  auto s = findSession(id);
  if (!s) {
    QVariantMap saved;
    for (auto v : sessionIndex)
      if (v.toMap()["id"] == id)
        saved = v.toMap();
    if (saved.isEmpty())
      return;
    s = fresh(saved["workspace"].toString());
    s->id = id;
    s->title = saved["title"].toString();
    s->file = saved["file"].toString();
    s->provider = saved["provider"].toString();
    s->model = saved["model"].toString();
    if (!s->file.isEmpty()) {
      auto history = U::preview(s->file);
      s->messages = history.first;
      s->tree = history.second;
    }
  }
  activate(s);
  if (!s->model.isEmpty() && !s->process && !s->busy())
    command(s, "inspect");
}
void Backend::loadNativeIndex() {
  auto directory = store.directory + "/pi/sessions";
  if (!QFileInfo(directory).isDir() || U::node().isEmpty())
    return;
  U::run(this, U::node(),
         {release + "/bridge.mjs", "--list-sessions", directory}, {},
         [this](bool ok, QByteArray raw, QString) {
           if (!ok || closing)
             return;
           auto rows = QJsonDocument::fromJson(raw)
                           .object()["sessions"]
                           .toArray()
                           .toVariantList();
           for (auto v : rows) {
             auto item = v.toMap();
             bool found = false;
             for (auto &sv : sessionIndex) {
               auto s = sv.toMap();
               if (s["id"] == item["id"]) {
                 for (auto it = item.begin(); it != item.end(); ++it)
                   s[it.key()] = it.value();
                 sv = s;
                 found = true;
                 break;
               }
             }
             if (!found)
               sessionIndex << item;
             if (!item["workspace"].toString().isEmpty())
               ensureProject(item["workspace"].toString());
           }
           for (auto s : records)
             if (!s->id.isEmpty() &&
                 (!s->file.isEmpty() || !s->messages.isEmpty()))
               indexSession(s);
           saveSessions();
           store.save();
           emit chatChanged();
         });
}
void Backend::writePi(std::shared_ptr<Session> s, QVariantMap payload) {
  if (s->process && s->process->state() == QProcess::Running)
    s->process->write(
        QJsonDocument::fromVariant(payload).toJson(QJsonDocument::Compact) +
        '\n');
}
bool Backend::command(std::shared_ptr<Session> s, QString action,
                      QVariantMap values) {
  if (s->busy() || kernelBusy()) {
    emit toast(gatewayPort ? "请等待当前任务结束"
                           : "模型网关尚未准备好，请检查 Node.js");
    return false;
  }
  if (s->model.isEmpty()) {
    emit toast("请先配置供应商并选择模型");
    return false;
  }
  if (s->stale || (s->process && !s->ready))
    retire(s);
  s->action = action;
  s->failed = false;
  s->cancelled = false;
  s->phase =
      s->process
          ? (action == "prompt" || action == "compact" ? "running" : "working")
          : "loading";
  s->status = s->phase == "loading" ? "正在加载 Pi" : "正在生成";
  values["type"] = action;
  values["model"] = s->model;
  if (s->process) {
    writePi(s, values);
    emit chatChanged();
    return true;
  }
  auto bridge = release + "/bridge.mjs";
  if (!QFileInfo(bridge).isFile() || U::node().isEmpty()) {
    s->phase = "idle";
    emit toast("Pi 内核或 Node.js 不可用，请检查安装");
    return false;
  }
  auto fallback = store.directory + "/workspace";
  QDir().mkpath(fallback);
  values["gateway"] = QString("http://127.0.0.1:%1/").arg(gatewayPort);
  values["token"] = privateToken;
  values["workspace"] = s->workspace;
  values["defaultCwd"] = fallback;
  values["agentDir"] = agentDir();
  values["sessionDir"] = store.directory + "/pi/sessions";
  values["sessionFile"] = s->file;
  values["models"] = modelCatalog();
  values["agent"] = store.data["agent"];
  QVariantMap envValues;
  for (auto path : {agentDir() + "/mcp.json", s->workspace + "/.pi/mcp.json"}) {
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
      auto it = QRegularExpression("\\$\\{([A-Za-z_][A-Za-z0-9_]*)\\}")
                    .globalMatch(QString::fromUtf8(f.readAll()));
      while (it.hasNext()) {
        auto name = it.next().captured(1);
        if (qEnvironmentVariableIsSet(name.toUtf8()))
          envValues[name] = qEnvironmentVariable(name.toUtf8());
      }
    }
  }
  values["mcpEnv"] = envValues;
  auto p = new QProcess(this);
  U::hideConsole(p);
  s->process = p;
  s->buffer.clear();
  s->ready = false;
  p->setProcessEnvironment(U::environment(store.data["agent"].toMap()));
  p->setProgram(U::node());
  p->setArguments({bridge});
  connect(p, &QProcess::started, this, [=, this] {
    if (s->process == p)
      writePi(s, values);
  });
  connect(p, &QProcess::readyReadStandardOutput, this, [=, this] {
    if (s->process != p || closing)
      return;
    s->buffer += p->readAllStandardOutput();
    while (s->buffer.contains('\n')) {
      auto index = s->buffer.indexOf('\n');
      auto line = s->buffer.left(index);
      s->buffer.remove(0, index + 1);
      auto payload = QJsonDocument::fromJson(line).object().toVariantMap();
      if (!payload.isEmpty())
        event(s, payload);
    }
  });
  connect(p, &QProcess::readyReadStandardError, this,
          [p] { p->readAllStandardError(); });
  connect(p, &QProcess::finished, this, [=, this] {
    if (s->process != p || closing)
      return;
    s->process = nullptr;
    event(s, {{"type", "exit"}});
    p->deleteLater();
  });
  connect(p, &QProcess::errorOccurred, this,
          [=, this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart && s->process == p) {
              s->process = nullptr;
              event(s, {{"type", "exit"},
                        {"message", "无法启动 Pi，请检查 Node.js"}});
              p->deleteLater();
            }
          });
  p->start();
  emit chatChanged();
  return true;
}
void Backend::send(QString text) { sendText(text, false); }
void Backend::steer(QString text) {
  if (text.trimmed().isEmpty() && view->attachments.isEmpty()) {
    if (view->phase == "running" && !view->queue.isEmpty())
      writePi(view, {{"type", "promote-queue"}});
    return;
  }
  sendText(text, true);
}
void Backend::editQueued(QString id) {
  for (auto row : view->queue)
    if (row.toMap()["id"] == id) {
      writePi(view, {{"type", "edit-queue"}, {"id", id}});
      return;
    }
}
void Backend::sendText(QString text, bool steering) {
  auto s = view;
  text = text.trimmed();
  if ((text.isEmpty() && s->attachments.isEmpty()) ||
      (s->busy() && s->phase != "running") || kernelBusy())
    return;
  auto id = s->phase == "running" ? s->provider
                                  : store.data["selectedProvider"].toString();
  auto model =
      s->phase == "running" ? s->model : store.data["selectedModel"].toString();
  bool vision = false;
  for (auto v : provider(id)["models"].toList())
    if (v.toMap()["alias"] == model)
      vision = U::normalizeModel(v.toMap())["vision"].toBool();
  QVariantList images, attachments = s->attachments;
  for (auto &v : attachments) {
    auto a = v.toMap();
    if (a["kind"] == "image") {
      if (vision)
        images << QVariantMap{
            {"type", "image"}, {"data", a["data"]}, {"mimeType", a["mime"]}};
      else {
        a["kind"] = "file";
        v = a;
        emit toast("未启用视觉，降级为普通文件路径");
      }
    }
  }
  text = U::attachmentPrompt(text, attachments);
  if (s->phase == "running") {
    writePi(s, {{"type", steering ? "steer" : "follow-up"},
                {"text", text},
                {"images", images}});
    s->attachments.clear();
    s->draft.clear();
    emit attachmentsChanged();
    emit editorText("");
    return;
  }
  s->provider = id;
  s->model = model;
  if (model.isEmpty()) {
    emit toast("请先选择模型");
    return;
  }
  if (s->id.isEmpty())
    s->id = "pending-" + s->key;
  if (s->messages.isEmpty())
    s->title = text.simplified().left(28);
  s->messages << QVariantMap{{"role", "user"}, {"text", text}, {"error", false}}
              << QVariantMap{
                     {"role", "assistant"}, {"text", ""}, {"error", false}};
  s->optimistic = true;
  indexSession(s);
  setMetadata(s->id, {{"unread", false}});
  if (command(s, "prompt", {{"text", text}, {"images", images}})) {
    s->attachments.clear();
    s->draft.clear();
    emit attachmentsChanged();
    emit editorText("");
    saveSessions();
    emit transcriptChanged();
  }
}
void Backend::restoreQueued(std::shared_ptr<Session> s, QStringList raw,
                            QVariantList images) {
  QStringList texts;
  if (!s->draft.isEmpty())
    texts << s->draft;
  for (auto text : raw) {
    auto parts = U::splitAttachments(text);
    if (!parts.first.isEmpty())
      texts << parts.first;
    for (auto v : parts.second) {
      auto a = v.toMap();
      bool found = false;
      for (auto existing : s->attachments)
        if (existing.toMap()["path"] == a["path"])
          found = true;
      if (!found && s->attachments.size() < 20) {
        a["id"] = U::uuid();
        if (a["kind"] == "image") {
          if (images.isEmpty())
            a["kind"] = "file";
          else {
            auto image = images.takeFirst().toMap();
            a["data"] = image["data"];
            a["mime"] = image["mimeType"];
          }
        }
        s->attachments << a;
      }
    }
  }
  s->draft = texts.join("\n\n");
  if (s == view) {
    emit attachmentsChanged();
    emit editorText(s->draft);
  }
}
QVariantList Backend::queuedMessages() const {
  QVariantList out;
  for (auto v : view->queue) {
    auto q = v.toMap();
    auto parsed = U::splitAttachments(q["text"].toString());
    q["text"] = parsed.first;
    q["attachments"] = parsed.second;
    out << q;
  }
  return out;
}
void Backend::event(std::shared_ptr<Session> s, QVariantMap payload) {
  if (closing)
    return;
  auto type = payload["type"].toString();
  bool visible = s == view, transcript = false;
  if (type == "ready") {
    s->ready = true;
    s->phase =
        s->action == "prompt" || s->action == "compact" ? "running" : "working";
    s->status = s->phase == "running" ? "正在生成" : "正在处理";
    sessionInfo(s, payload["session"].toMap());
  } else if (type == "session")
    sessionInfo(s, payload["session"].toMap());
  else if (type == "delta" || type == "thinking_delta") {
    if (s->messages.isEmpty() ||
        s->messages.last().toMap()["role"] != "assistant")
      s->messages << QVariantMap{
          {"role", "assistant"}, {"text", ""}, {"error", false}};
    auto last = s->messages.last().toMap();
    auto key = type == "delta" ? "text" : "thinking";
    last[key] = last[key].toString() + payload["text"].toString();
    s->messages.last() = last;
    transcript = true;
  } else if (type == "user_start") {
    if (s->optimistic)
      s->optimistic = false;
    else {
      s->messages << QVariantMap{{"role", "user"},
                                 {"text", payload["text"]},
                                 {"error", false}}
                  << QVariantMap{
                         {"role", "assistant"}, {"text", ""}, {"error", false}};
      transcript = true;
    }
  } else if (type == "assistant_start") {
    auto last =
        s->messages.isEmpty() ? QVariantMap{} : s->messages.last().toMap();
    if (last["role"] != "assistant" || !last["text"].toString().isEmpty() ||
        !last["thinking"].toString().isEmpty() ||
        !last["tools"].toList().isEmpty()) {
      s->messages << QVariantMap{
          {"role", "assistant"}, {"text", ""}, {"error", false}};
      transcript = true;
    }
  } else if (type == "queue") {
    s->queue.clear();
    if (payload.contains("items"))
      s->queue = payload["items"].toList();
    else {
      for (auto v : payload["steering"].toList())
        s->queue << QVariantMap{{"text", v}, {"mode", "steering"}};
      for (auto v : payload["followUp"].toList())
        s->queue << QVariantMap{{"text", v}, {"mode", "followUp"}};
    }
  } else if (type == "queue_edited") {
    restoreQueued(s, {payload["text"].toString()}, payload["images"].toList());
  } else if (type == "queue_rejected") {
    restoreQueued(s, {payload["text"].toString()});
    if (visible)
      emit toast(payload["message"].toString());
  } else if (type == "queue_returned") {
    QStringList texts;
    for (auto v : payload["steering"].toList())
      texts << v.toString();
    for (auto v : payload["followUp"].toList())
      texts << v.toString();
    restoreQueued(s, texts);
  } else if (type == "message" && payload["role"] == "assistant") {
    if (!s->messages.isEmpty() &&
        s->messages.last().toMap()["role"] == "assistant") {
      auto last = s->messages.last().toMap();
      auto text = payload["text"].toString();
      if (text.isEmpty())
        text = payload["error"].toString();
      if (!text.isEmpty())
        last["text"] = text;
      last["error"] = !payload["error"].toString().isEmpty();
      if (payload.contains("thinking"))
        last["thinking"] = payload["thinking"];
      s->messages.last() = last;
      transcript = true;
    }
  } else if (type == "tool") {
    s->status = payload["name"].toString() + " · " +
                (payload["state"] == "running" ? "运行中"
                 : payload["state"] == "error" ? "失败"
                                               : "完成");
    int row = -1, index = -1;
    for (int i = s->messages.size() - 1; i >= 0 && row < 0; --i) {
      auto tools = s->messages[i].toMap()["tools"].toList();
      for (int j = 0; j < tools.size(); ++j)
        if (tools[j].toMap()["id"] == payload["id"]) {
          row = i;
          index = j;
          break;
        }
    }
    if (row < 0) {
      if (s->messages.isEmpty() ||
          s->messages.last().toMap()["role"] != "assistant")
        s->messages << QVariantMap{
            {"role", "assistant"}, {"text", ""}, {"error", false}};
      row = s->messages.size() - 1;
    }
    auto message = s->messages[row].toMap();
    auto tools = message["tools"].toList();
    auto tool = index < 0 ? U::toolView(payload["id"].toString(),
                                        payload["name"].toString(),
                                        payload["args"].toMap(), s->workspace)
                          : tools[index].toMap();
    tool["state"] = payload["state"];
    if (payload.contains("result"))
      tool["result"] = payload["result"];
    if (index < 0)
      tools << tool;
    else
      tools[index] = tool;
    message["tools"] = tools;
    s->messages[row] = message;
    transcript = true;
  } else if (type == "status")
    s->status = payload["text"].toString();
  else if (type == "notice") {
    if (visible)
      emit toast(payload["message"].toString());
  } else if (type == "editor") {
    s->draft = payload["text"].toString();
    if (visible)
      emit editorText(s->draft);
  } else if (type == "ui_request") {
    s->approval = payload;
    if (visible)
      emit approvalChanged();
  } else if (type == "resources") {
    s->resources = payload["resources"].toMap();
    if (visible)
      emit changed();
  } else if (type == "context_usage") {
    s->usage = payload["usage"].toMap();
    if (visible)
      emit contextUsageChanged();
    return;
  } else if (type == "error") {
    s->failed = true;
    if (!s->messages.isEmpty() &&
        s->messages.last().toMap()["text"].toString().isEmpty()) {
      auto last = s->messages.last().toMap();
      last["text"] = payload["message"];
      last["error"] = true;
      s->messages.last() = last;
      transcript = true;
    }
    if (visible)
      emit toast(payload["message"].toString());
  } else if (type == "state") {
    sessionInfo(s, payload["session"].toMap());
    s->messages = payload["messages"].toList();
    s->tree = payload["tree"].toList();
    s->resources = payload["resources"].toMap();
    s->usage = payload["contextUsage"].toMap();
    for (auto v : payload["sessions"].toList()) {
      auto ns = v.toMap();
      bool exists = false;
      for (auto &old : sessionIndex) {
        auto previous = old.toMap();
        if (previous["id"] == ns["id"]) {
          for (auto it = ns.begin(); it != ns.end(); ++it)
            previous[it.key()] = it.value();
          old = previous;
          exists = true;
          break;
        }
      }
      if (!exists)
        sessionIndex << ns;
      if (!ns["workspace"].toString().isEmpty())
        ensureProject(ns["workspace"].toString());
    }
    for (auto live : records)
      if (!live->id.isEmpty() &&
          (!live->file.isEmpty() || !live->messages.isEmpty()))
        indexSession(live);
    store.save();
    saveSessions();
    if (visible) {
      emit changed();
      emit contextUsageChanged();
    }
    transcript = true;
  } else if (type == "done" || type == "exit") {
    bool was = s->busy();
    if (!s->queue.isEmpty()) {
      QStringList texts;
      for (auto row : s->queue)
        texts << row.toMap()["text"].toString();
      s->queue.clear();
      restoreQueued(s, texts);
    }
    s->phase = "idle";
    s->status.clear();
    s->approval.clear();
    if (type == "exit") {
      s->ready = false;
      if (was) {
        s->failed = true;
        if (!s->messages.isEmpty() &&
            s->messages.last().toMap()["text"].toString().isEmpty()) {
          auto m = s->messages.last().toMap();
          m["error"] = true;
          m["text"] =
              payload.value("message", "Pi 内核已退出，会话已保存在本机");
          s->messages.last() = m;
          transcript = true;
        }
      }
    }
    if (was && s->action == "prompt" && !s->cancelled && !s->id.isEmpty())
      setMetadata(s->id, {{"unread", !(visible && chatVisible)}});
    if (visible)
      emit approvalChanged();
    if (s->stale || (type == "done" && !s->ready))
      retire(s);
    trimPool();
    saveSessions();
  } else
    return;
  if (visible) {
    if (type == "delta" || type == "thinking_delta") {
      if (!streaming.isActive())
        streaming.start();
    } else {
      streaming.stop();
      emit chatChanged();
      if (transcript)
        emit transcriptChanged();
    }
  } else
    emit sidebarChanged();
}
void Backend::stop() {
  if (view->phase == "running") {
    view->cancelled = true;
    writePi(view, {{"type", "abort"}});
    view->approval.clear();
    emit approvalChanged();
  } else if (view->busy()) {
    retire(view);
    view->phase = "idle";
    emit chatChanged();
  }
}
void Backend::respondApproval(bool allowed) {
  writePi(view,
          {{"type", "ui_response"},
           {"id", view->approval["id"]},
           {"value", view->approval["method"] == "confirm" ? QVariant(allowed)
                                                           : QVariant()}});
  view->approval.clear();
  emit approvalChanged();
}
void Backend::respondPiDialog(QString value) {
  writePi(view, {{"type", "ui_response"},
                 {"id", view->approval["id"]},
                 {"value", value}});
  view->approval.clear();
  emit approvalChanged();
}
void Backend::operateSession(QString id, QVariantMap action) {
  QVariantMap saved;
  for (auto v : sessionIndex)
    if (v.toMap()["id"] == id)
      saved = v.toMap();
  if (saved.isEmpty())
    return;
  auto record = findSession(id);
  if (record && record->busy()) {
    emit toast("请等待该会话结束后操作");
    return;
  }
  if (record)
    retire(record);
  action["sessionFile"] = saved["file"];
  action["cwd"] = saved.value("workspace", store.directory);
  action["sessionDir"] = store.directory + "/pi/sessions";
  U::run(this, U::node(), {release + "/bridge.mjs", "--session-action"},
         QJsonDocument::fromVariant(action).toJson(QJsonDocument::Compact),
         [this, id, record](bool ok, QByteArray raw, QString error) {
           auto result = QJsonDocument::fromJson(raw).object().toVariantMap();
           if (!ok || !result["ok"].toBool()) {
             emit toast(result.value("error", error).toString());
             return;
           }
           auto info = result["session"].toMap();
           for (auto &v : sessionIndex) {
             auto saved = v.toMap();
             if (saved["id"] == id) {
               for (auto it = info.begin(); it != info.end(); ++it)
                 saved[it.key()] = it.value();
               v = saved;
             }
           }
           if (record)
             sessionInfo(record, info);
           saveSessions();
           emit chatChanged();
         });
}
void Backend::renameSession(QString id, QString title) {
  title = title.trimmed().left(100);
  if (!title.isEmpty())
    operateSession(id, {{"action", "rename"}, {"title", title}});
}
void Backend::exportSession(QString id, QString url) {
  if (!QUrl(url).isLocalFile())
    return;
  operateSession(
      id, {{"action", "export"}, {"outputFile", QUrl(url).toLocalFile()}});
}
void Backend::forkSessionById(QString id) {
  auto original = findSession(id);
  if (original && original->busy()) {
    emit toast("此会话结束后可创建分支");
    return;
  }
  for (auto v : sessionIndex) {
    auto source = v.toMap();
    if (source["id"] != id)
      continue;
    auto s = fresh(source["workspace"].toString());
    s->model = source.value("model", s->model).toString();
    s->provider = source.value("provider", s->provider).toString();
    activate(s);
    command(s, "fork", {{"targetFile", source["file"]}});
    return;
  }
}
void Backend::forkSession() {
  if (!view->id.isEmpty() && !view->busy())
    forkSessionById(view->id);
}
void Backend::deleteSession(QString id) {
  auto live = findSession(id);
  if (live && live->busy()) {
    emit toast("此会话正在运行，请先停止该会话");
    return;
  }
  for (int i = sessionIndex.size() - 1; i >= 0; --i) {
    auto s = sessionIndex[i].toMap();
    if (s["id"] != id)
      continue;
    auto file = s["file"].toString();
    if (!file.isEmpty()) {
      auto base = U::cleanPath(store.directory + "/pi/sessions") + "/";
      if (!U::cleanPath(file).startsWith(base, Qt::CaseInsensitive)) {
        emit toast("只能删除 Umeko 会话目录中的文件");
        return;
      }
      if (live)
        retire(live);
      if (QFileInfo::exists(file) && !QFile::remove(file)) {
        emit toast("无法删除会话文件");
        return;
      }
    }
    sessionIndex.removeAt(i);
  }
  if (live)
    records.remove(live->key);
  auto meta = store.data["sessionMeta"].toMap();
  meta.remove(id);
  store.data["sessionMeta"] = meta;
  store.save();
  saveSessions();
  if (view->id == id)
    newSession();
  emit chatChanged();
}
