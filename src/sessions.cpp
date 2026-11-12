#include "backend.hpp"
#include "pi_capabilities.hpp"
#include "global_agents.hpp"
#include <QtConcurrent>

QVariantMap Backend::sessionTurnStatus(std::shared_ptr<Session> s) const {
  if(s->busy() && (s->action=="prompt" || s->action=="widget-notify") && s->promptStarted){
    const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-*s->promptStarted).count();
    return {{"state",s->cancelled?"stopping":"running"},{"elapsed",elapsed},{"startedAt",QDateTime::currentMSecsSinceEpoch()-elapsed*1000}};
  }
  const auto saved=metadata(s->id)["lastTurn"].toMap();if(!saved.isEmpty())return saved;
  for(int i=s->messages.size()-1;i>=0;--i){auto m=s->messages[i].toMap();if(m["customType"]=="umeko-compaction")continue;if(m["role"]=="user")break;if(m["role"]=="assistant"){const auto reason=m["stopReason"].toString();return {{"state",m["error"].toBool()||reason=="error"?"failed":reason=="aborted"||reason=="length"?"stopped":""}};}}
  return {};
}
void Backend::finishTurn(std::shared_ptr<Session> s,bool failed) {
  if(!s->promptStarted || (s->action!="prompt" && s->action!="widget-notify"))return;
  const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-*s->promptStarted).count();
  if(!s->id.isEmpty())setMetadata(s->id,{{"lastTurn",QVariantMap{{"state",s->cancelled?"stopped":failed||s->failed?"failed":"finished"},{"elapsed",elapsed}}}});
}
std::shared_ptr<Session> Backend::fresh(QString workspace) {
  auto s = std::make_shared<Session>();
  s->workspace = workspace;
  s->model = store.data["selectedModel"].toString();
  s->provider = store.data["selectedProvider"].toString();
  auto effort =
      store.data["ui"].toMap().value("reasoningEffort", "off").toString();
  s->reasoningEffort = U::reconcileEffort(effort, reasoningLevelsFor(s));
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
  const QScopedValueRollback<bool> transition(switchingView,true);
  if(!s->workspace.isEmpty())s->workspace=U::cleanPath(s->workspace);
  streaming.stop();
  if(view && view!=s && !view->id.isEmpty())setMetadata(view->id,{{"viewport",QVariantMap{{"index",view->scrollIndex},{"offset",view->scrollY},{"follow",view->followTail}}}});
  view = s;
  chat.switchContext(s->key);
  const int anchor=s->followTail ? qMax(0,int(s->displaySnapshot.size())-4) : qMax(0,s->scrollIndex);
  if(!s->displaySnapshot.isEmpty()){chat.seed(s->messages,s->displaySnapshot,anchor);s->displaySnapshot.clear();}
  else chat.requestRange(qMax(0,s->scrollIndex),qMax(0,s->scrollIndex)+4);
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
  if (s->backend=="dsh" && s->dshOwnFixed && s->dshOwn) valid=true;
  if (!valid && !s->busy()) {
    repairSelection();
    s->provider = store.data["selectedProvider"].toString();
    s->model = store.data["selectedModel"].toString();
    s->usage.clear();
  } else if (valid) {
    store.data["selectedProvider"] = s->provider;
    store.data["selectedModel"] = s->model;
  }
  reconcileReasoning(s);
  markRead(s);
  // Fold navigation metadata/model selection into the existing deferred save.
  // close() still flushes the final snapshot before shutting down.
  appearanceSave.start();
  emit changed();
  emit chatChanged();
  emit viewChanged();
  emit attachmentsChanged();
  emit approvalChanged();
  emit contextUsageChanged();
  emit reasoningChanged();
  emit retryChanged();
  emit editorText(s->draft);
  trimPool();
}
void Backend::trimPool() {
  const auto idle=[](const std::shared_ptr<Session> &s){return s && s->process && !s->busy() && !s->subagentsActive;};
  int count=0;for(const auto &s:records)if(idle(s))++count;
  const int limit=qMax(idle(view)?1:0,qBound(0,store.data["ui"].toMap().value("sessionCacheSize",3).toInt(),64));
  for(const auto &key:lru) {
    if(count<=limit)break;
    const auto s=records.value(key);
    if(s!=view && idle(s)){retire(s);--count;}
  }
}
void Backend::setSessionCacheSize(int count) {
  if(count<0 || count>64)return;
  auto ui=store.data["ui"].toMap();if(ui.value("sessionCacheSize",3).toInt()==count)return;
  ui["sessionCacheSize"]=count;store.data["ui"]=ui;store.save();trimPool();emit changed();
}
void Backend::retire(std::shared_ptr<Session> s) {
  if(s->dshPreparing){auto p=s->dshPreparing;s->dshPreparing=nullptr;p->kill();p->deleteLater();}
  endControlWork(controlNativeWork.take(s->key), {{"ok", false}, {"error", "Pi operation was cancelled before completion"}});
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
  QTimer::singleShot(s->backend=="dsh"?12000:4000, p, [p] {
    if (p->state() != QProcess::NotRunning)
      p->kill();
  });
}
void Backend::invalidate() {
  for (auto s : records) {
    if (s->busy() || s->subagentsActive)
      s->stale = true;
    else {
      retire(s);
      s->usage.clear();
      reconcileReasoning(s);
    }
  }
  emit contextUsageChanged();
}
void Backend::indexSession(std::shared_ptr<Session> s) {
  if (s->id.isEmpty() || !s->globalAgent.isEmpty())
    return;
  QVariantMap info{
      {"id", s->id},       {"file", s->file},   {"workspace", s->workspace},
      {"title", s->title}, {"model", s->model}, {"provider", s->provider}};
  if(s->backend=="dsh")info.insert({{"backend","dsh"},{"acpSessionId",s->acpSessionId},{"dshHome",s->dshHome},{"dshOwnConfig",s->dshOwn},{"file",store.directory+"/dsh/transcripts/"+s->id+".json"}});
  for (auto &v : sessionIndex)
    if (v.toMap()["id"] == s->id) {
      v = info;
      return;
    }
  sessionIndex.prepend(info);
}
void Backend::sessionInfo(std::shared_ptr<Session> s, QVariantMap info) {
  if (!s->globalAgent.isEmpty()) { s->id=info.value("id",s->id).toString(); if(!info["file"].toString().isEmpty())s->file=info["file"].toString(); return; }
  auto old = s->id;
  s->id = info.value("id", s->id).toString();
  if (!info["file"].toString().isEmpty())
    s->file = info["file"].toString();
  if (!info["title"].toString().isEmpty())
    s->title = info["title"].toString();
  if (!info["workspace"].toString().isEmpty() && !(s->workspace.isEmpty() && U::cleanPath(info["workspace"].toString())==U::cleanPath(store.directory+"/workspace")))
    s->workspace = U::cleanPath(info["workspace"].toString());
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
  if (!s->id.isEmpty())
    setMetadata(s->id, {{"reasoningEffort", s->reasoningEffort}, {"extensionModes",s->extensionModes}, {"blockedTools",s->blockedTools},{"piCapabilities",s->piCapabilities},{"skillModes",s->skillModes}});
}
void Backend::saveSessions() { store.saveSessions(sessionIndex); }
void Backend::newSession() {
  auto s = fresh(store.data["workspace"].toString());
  drafts[s->workspace] = s;
  activate(s);
}
void Backend::openSession(QString id) {
  if(files->pending.contains(id)||files->deleted.contains(id))return;
  auto s = findSession(id);
  if (!s) {
    static int next=1000000;const int request=++next;
    auto connection=std::make_shared<QMetaObject::Connection>();
    *connection=connect(previewLoader,&SessionPreview::ready,this,[this,request,connection](int token,bool ok){if(token!=request)return;disconnect(*connection);if(ok)previewLoader->activate(token);});
    previewLoader->prepare(id,request);return;
  }
  activate(s);
  // History browsing is read-only; start Pi only for an explicit task or inventory request.
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
             if(files->deleted.contains(item["id"].toString())||files->pending.contains(item["id"].toString()))continue;
             if(U::cleanPath(item["workspace"].toString())==U::cleanPath(store.directory+"/workspace"))item["workspace"]="";
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
  if(s->backend=="dsh") {
    const auto type=payload["type"].toString();
    if(type=="reasoning_effort"){switchDshConfig(s);return;}
    if(type=="abort")payload["type"]="cancel";
    if(QStringList{"edit-queue","delete-queue","steer-queued","promote-queue"}.contains(type)) {
      if(type=="steer-queued" || type=="promote-queue"){emit toast("DeepSeek Harness 的排队消息会在当前任务结束后发送。");return;}
      for(int i=0;i<s->queue.size();++i)if(s->queue[i].toMap()["id"]==payload["id"]){auto item=s->queue.takeAt(i).toMap();if(type=="edit-queue")restoreQueued(s,{item["text"].toString()});break;}
      emit chatChanged();return;
    }
    if(!QStringList{"start","prompt","open","cancel","abort","set_config","tool_permission_mode","ui_response","shutdown"}.contains(type))return;
  }
  if (s->process && s->process->state() == QProcess::Running)
    s->process->write(
        QJsonDocument::fromVariant(payload).toJson(QJsonDocument::Compact) +
        '\n');
}
bool Backend::command(std::shared_ptr<Session> s, QString action,
                      QVariantMap values) {
  if(files && (files->pending.contains(s->id)||files->deleted.contains(s->id))){emit toast("会话数据正在删除，请稍后再试");return false;}
  if (desktopExtensions->checkpointLocked(s->workspace)) return false;
  cancelReasoningDetection(s->provider);
  if(s->backend=="dsh")return commandDsh(s,action,values);
  if (s->busy() || kernelBusy()) {
    emit toast(gatewayPort ? "请等待当前任务结束"
                           : "模型网关尚未准备好，请检查 Node.js");
    return false;
  }
  if(!desktopExtensions->prepareSession(s))return false;
  if (s->model.isEmpty()) {
    emit toast("请先配置供应商并选择模型");
    return false;
  }
  if (s->stale || (s->process && !s->ready))
    retire(s);
  s->action = action;
  s->promptStarted.reset();
  if (action == "prompt" || action=="widget-notify") s->promptStarted = std::chrono::steady_clock::now();
  s->failed = false;
  s->cancelled = false;
  s->phase =
      s->process
          ? (action == "prompt" || action == "widget-notify" || action == "compact" ? "running" : "working")
          : "loading";
  s->status = s->phase == "loading" ? "正在加载 Pi" : "正在生成";
  values["systemPromptEnabled"] = desktopExtensions->systemPromptEnabled(s->key);
  values["systemPromptStateDir"] = store.directory+"/system-prompts";
  values["widgetPrompts"] = desktopExtensions->promptSources(s->key);
  values["type"] = action;
  values["model"] = s->model;
  reconcileReasoning(s);
  values["reasoningEffort"] = s->reasoningEffort;
  if (s->process) {
    controlNativeWork[s->key] = beginControlWork();
    writePi(s, values);
    emit chatChanged();
    return true;
  }
  auto bridge = release + "/bridge.mjs";
  if (!QFileInfo(bridge).isFile() || U::node().isEmpty()) {
    finishTurn(s,true);s->phase = "idle";
    s->promptStarted.reset();
    emit toast("Pi 内核或 Node.js 不可用，请检查安装");
    return false;
  }
  auto fallback = store.directory + "/workspace";
  QDir().mkpath(fallback);
  values["gateway"] = QString("http://127.0.0.1:%1/").arg(gatewayPort);
  values["token"] = privateToken;
  values["auxiliaryVision"] = auxiliaryVision();
  values["generation"] = generationConfig();
  values["workspace"] = s->workspace;
  values["defaultCwd"] = fallback;
  values["agentDir"] = agentDir();
  auto web=webviewConfiguration();web["autoOpen"]=store.data["desktop"].toMap().value("autoOpenSubmissions",true);web["display"]=store.data["desktop"].toMap().value("webDisplay","embedded");
  values["webview"] = web;
  values["sessionDir"] = store.directory + "/pi/sessions";
  values["docsDir"] = store.directory + "/agent-docs";
  values["sessionFile"] = s->file;
  values["models"] = modelCatalog();
  values["agent"] = store.data["agent"];
  values["permissionMode"]=store.data["agent"].toMap().value("toolPermissionMode","auto");
  values["dangerousAllowed"]=s->dangerousAllowed && s->pluginOwner.isEmpty();
  if(!s->pluginOwner.isEmpty())values["pluginSessionPolicy"]=s->pluginPolicy;
  values["language"]=language();values["dataDir"]=store.directory;
  QVariantMap extensions;
  for(auto it=s->extensionModes.cbegin();it!=s->extensionModes.cend();++it)
    if(it.value()=="session" || it.value()=="disabled") extensions[it.key()]=it.value()=="session";
  values["sessionExtensions"] = extensions;
  values["blockedTools"] = s->blockedTools;
  values["piCapabilities"] = s->piCapabilities;
  values["sessionSkills"] = s->skillModes;
  values["skillsEnabled"] = desktopExtensions->skillsEnabled(s->key);
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
  values["loopReview"] = true;
  if (!s->globalAgent.isEmpty()) { const auto extra=desktopExtensions->globalAgents()->runtimeConfig(s); for(auto it=extra.cbegin();it!=extra.cend();++it) values[it.key()]=it.value(); }
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
  controlNativeWork[s->key] = beginControlWork();
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
void Backend::steerQueued(QString id) {
  for (auto row : view->queue)
    if (row.toMap()["id"] == id) {
      writePi(view, {{"type", "steer-queued"}, {"id", id}});
      return;
    }
}
void Backend::removeQueued(QString id) {
  for (auto row : view->queue)
    if (row.toMap()["id"] == id) {
      writePi(view, {{"type", "delete-queue"}, {"id", id}});
      return;
    }
}
void Backend::sendText(QString text, bool steering) {
  if(files->pending.contains(view->id)){emit toast("会话数据正在删除，请稍后再试");return;}
  auto s = view;
  text = text.trimmed();
  sendError.clear();
  if(s->backend=="dsh"){sendDsh(text,steering);return;}
  if (desktopExtensions->checkpointLocked(s->workspace)) {
    sendError = "该工作目录正在恢复代码检查点，完成后即可发送消息。";
    emit toast(sendError); emit changed(); return;
  }
  if (kernelBusy()) {
    sendError = "Pi 内核检查或更新正在进行；完成后即可再次发送消息。";
    emit toast(sendError); emit changed(); return;
  }
  if ((text.isEmpty() && s->attachments.isEmpty()) ||
      (s->busy() && s->phase != "running"))
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
      if (vision || (auxiliaryVision()["autoAnalyze"].toBool() && !auxiliaryVision()["model"].toString().isEmpty()))
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
  if(s->backend=="dsh"){if(dshEvent(s,payload))return;if(type=="ui_request")payload["backend"]="dsh";}
  if(type=="dangerous_approval_cleared"){s->dangerousAllowed=false;if(s==view)emit approvalChanged();return;}
  desktopExtensions->observe(s->key,payload);
  if(type=="tool")s->petActivity="working";
  else if(QStringList{"assistant_start","thinking_delta","done","error","exit"}.contains(type))s->petActivity.clear();
  if (type == "subagents" || type.startsWith("subagent_")) { children->handle(s, payload); return; }
  bool visible = s == view, transcript = false;
  if (type == "ready") {
    s->ready = true;
    s->phase =
        s->action == "prompt" || s->action == "widget-notify" || s->action == "compact" ? "running" : "working";
    s->status = s->action=="compact"?"正在压缩上下文":s->phase == "running" ? "正在生成" : "正在处理";
    sessionInfo(s, payload["session"].toMap());
  } else if (type == "session")
    sessionInfo(s, payload["session"].toMap());
  else if (type == "history_checkpoint") {
    s->searchLeaf = payload["leafId"].toString();
  } else if (type == "native_entry") {
    s->searchLeaf = payload["entryId"].toString();
    for(int i=s->messages.size()-1;i>=0;--i){
      auto row=s->messages[i].toMap();
      if(row["role"]==payload["role"] && row["entryId"].toString().isEmpty()){
        row["entryId"]=payload["entryId"];s->messages[i]=row;transcript=true;break;
      }
    }
  } else if (type == "delta" || type == "thinking_delta") {
    if (s->messages.isEmpty() ||
        (s->messages.last().toMap()["role"] != "assistant" || s->messages.last().toMap()["customType"]=="umeko-compaction"))
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
    if (payload.contains("timestamp"))
      for (int i = s->messages.size() - 1; i >= 0; --i) {
        auto message = s->messages[i].toMap();
        if (message["role"] == "user") {
          message["timestamp"] = payload["timestamp"];
          s->messages[i] = message;
          transcript = true;
          break;
        }
      }
  } else if (type == "assistant_start") {
    if (payload["retry"].toBool() && !s->messages.isEmpty() &&
        s->messages.last().toMap()["role"] == "assistant" && s->messages.last().toMap()["customType"]!="umeko-compaction" &&
        s->messages.last().toMap()["tools"].toList().isEmpty()) {
      s->messages.last() =
          QVariantMap{{"role", "assistant"}, {"text", ""}, {"error", false}};
      transcript = true;
    }
    auto last =
        s->messages.isEmpty() ? QVariantMap{} : s->messages.last().toMap();
    if (last["role"] != "assistant" || last["customType"]=="umeko-compaction" || !last["text"].toString().isEmpty() ||
        !last["thinking"].toString().isEmpty() ||
        !last["tools"].toList().isEmpty()) {
      s->messages << QVariantMap{
          {"role", "assistant"}, {"text", ""}, {"error", false}};
      transcript = true;
    }
    if (!s->messages.isEmpty() && payload.contains("timestamp")) {
      auto message = s->messages.last().toMap();
      message["timestamp"] = payload["timestamp"];
      s->messages.last() = message;
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
    if (!payload["error"].toString().isEmpty() || payload["stopReason"] == "error") s->failed = true;
    if (!s->messages.isEmpty() &&
        s->messages.last().toMap()["role"] == "assistant") {
      auto last = s->messages.last().toMap();
      auto text = payload["text"].toString();
      if (text.isEmpty())
        text = payload["error"].toString();
      if (!text.isEmpty())
        last["text"] = text;
      last["error"] = !payload["error"].toString().isEmpty();
      last["stopReason"] = payload.value("stopReason", "");
      if (payload.contains("timestamp"))
        last["timestamp"] = payload["timestamp"];
      if (payload.contains("thinking"))
        last["thinking"] = payload["thinking"];
      s->messages.last() = last;
      transcript = true;
    }
  } else if (type == "tool") {
    if (!payload["subagent"].toMap().isEmpty()) children->merge(s,payload["subagent"].toMap());
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
          (s->messages.last().toMap()["role"] != "assistant" || s->messages.last().toMap()["customType"]=="umeko-compaction"))
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
    if(s->backend=="dsh" && payload["args"].typeId()==QMetaType::QVariantMap) {
      const auto view=U::toolView(payload["id"].toString(),payload["name"].toString(),payload["args"].toMap(),s->workspace);
      tool["arguments"]=view["arguments"];tool["path"]=view["path"];
    }
    tool["state"] = payload["state"];
    if(payload.contains("video"))tool["video"]=payload["video"];
    if (!payload["subagent"].toMap().isEmpty()) tool["subagent"] = payload["subagent"];
    if (!payload["media"].toList().isEmpty()) {
      tool["media"] = payload["media"];
      if (visible && payload["name"] == "submit_to_chatbox" && payload["state"] == "done")
        for (const auto &value : payload["media"].toList()) {
          const auto item = value.toMap();
          const auto url = normalizeWebUrl(item["url"].toString());
          if (item["kind"] == "web" && !url.isEmpty()) openWebPreview(url, item["title"].toString());
          const auto kind = U::documentKind(item["path"].toString(), item["kind"].toString());
          if (!kind.isEmpty() && QFileInfo(item["path"].toString()).isAbsolute() && !openExternalHtml(item["path"].toString())) emit documentRequested(item["path"].toString(), item["title"].toString(), kind);
        }
    }
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
    if(!s->pluginOwner.isEmpty() && desktopExtensions->captureSessionApproval(s->key,payload))return;
    s->approval = payload;
    if (visible)
      emit approvalChanged();
  } else if (type == "umeko_request") {
    writePi(s, {{"type", "umeko_response"}, {"id", payload["id"]},
                {"result", umekoRequest({{"operation", payload["operation"]}, {"arguments", payload.value("arguments", QVariantMap{})}}, s->key)}});
    return;
  } else if (type == "resources") {
    s->resources = PiCapabilities::mergeResources(s->resources, payload["resources"].toMap(), s->piCapabilities);
    if (visible)
      emit changed();
  } else if(type=="compaction_event") {
    auto row=U::compactionView({{"type","custom"},{"customType","umeko-compaction"},{"data",payload},{"id",payload["entryId"]},{"timestamp",payload["timestamp"]}});
    if(row.isEmpty())return;
    int found=-1;for(int i=0;i<s->messages.size();++i)if(s->messages[i].toMap()["compaction"].toMap()["eventId"]==payload["eventId"]){found=i;break;}
    if(found<0)s->messages<<row;else{auto previous=s->messages[found].toMap();previous["compaction"]=row["compaction"];s->messages[found]=previous;}
    s->compactionRunning=payload["state"]=="running";s->compactionEvent=s->compactionRunning?payload["eventId"].toString():QString();s->status=s->compactionRunning?"正在压缩上下文":"";transcript=true;
  } else if (type == "network_retry") {
    s->networkRetry = payload["active"].toBool() ? payload : QVariantMap{};
    if (visible)
      emit retryChanged();
    return;
  } else if (type == "context_usage") {
    s->usage = payload["usage"].toMap();
    if (visible)
      emit contextUsageChanged();
    return;
  } else if (type == "error") {
    s->failed = true;
    if (!s->messages.isEmpty() &&
        s->messages.last().toMap()["text"].toString().isEmpty() && s->messages.last().toMap()["customType"]!="umeko-compaction") {
      auto last = s->messages.last().toMap();
      last["text"] = payload["message"];
      last["error"] = true;
      s->messages.last() = last;
      transcript = true;
    }
    if (visible)
      emit toast(payload["message"].toString());
  } else if (type == "state") {
    s->searchLeaf = payload["leafId"].toString();
    sessionInfo(s, payload["session"].toMap());
    s->messages = payload["messages"].toList();
    for(auto &row:s->messages)row=U::compactionStatus(row.toMap(),s->compactionRunning?s->compactionEvent:QString());
    children->sync(s);
    s->tree = payload["tree"].toList();
    s->resources = PiCapabilities::mergeResources(s->resources, payload["resources"].toMap(), s->piCapabilities);
    s->usage = payload["contextUsage"].toMap();
    if (s->globalAgent.isEmpty()) for (auto v : payload["sessions"].toList()) {
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
    for(auto &row:s->messages)row=U::compactionStatus(row.toMap());
    s->compactionRunning=false;s->compactionEvent.clear();transcript=true;
    s->networkRetry.clear();
    if (visible)
      emit retryChanged();
    bool was = s->busy();
    const bool longRunning = s->promptStarted &&
        std::chrono::steady_clock::now() - *s->promptStarted > std::chrono::minutes(3);
    finishTurn(s,type=="exit");
    s->promptStarted.reset();
    if (!s->queue.isEmpty()) {
      QStringList texts;
      for (auto row : s->queue)
        texts << row.toMap()["text"].toString();
      s->queue.clear();
      restoreQueued(s, texts);
    }
    s->phase = "idle";
    children->processReady(s);
    s->status.clear();
    s->approval.clear();
    if (type == "exit") {
      children->processExit(s);
      s->ready = false;
      if (was) {
        s->failed = true;
        if (!s->messages.isEmpty() &&
            s->messages.last().toMap()["text"].toString().isEmpty() && s->messages.last().toMap()["customType"]!="umeko-compaction") {
          auto m = s->messages.last().toMap();
          m["error"] = true;
          m["text"] =
              payload.value("message", "Pi 内核已退出，会话已保存在本机");
          s->messages.last() = m;
          transcript = true;
        }
      }
    }
    if (was && s->globalAgent.isEmpty() && s->action == "prompt" && !s->cancelled && !s->id.isEmpty()) {
      setMetadata(s->id, {{"unread", !(visible && chatVisible)}});
      emit taskFinished(s->id, s->title, type == "done" && !s->failed, longRunning);
    }
    if (visible)
      emit approvalChanged();
  if (type == "done" || type == "exit" || type == "error") {
    const bool ok = type == "done" && !s->failed && !s->cancelled;
    endControlWork(controlNativeWork.take(s->key), ok ? QVariantMap{{"ok", true}, {"action", s->action}, {"resources", s->resources}} : QVariantMap{{"ok", false}, {"error", payload.value("message", "Pi operation failed or exited before completion")}});
  }
    if (s->stale || (type == "done" && !s->ready))
      retire(s);
    trimPool();
    saveSessions();
  } else
    return;

  if (type == "ready") children->processReady(s);

  if (type == "error") endControlWork(controlNativeWork.take(s->key), {{"ok", false}, {"error", payload.value("message", "Pi operation failed")}});
  if(s->backend=="dsh" && QStringList{"delta","thinking_delta","tool","error"}.contains(type))saveDsh(s);
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
    if(view->backend=="dsh"){view->dshInterrupted=true;saveDsh(view);if(view->dshPreparing){auto p=view->dshPreparing;view->dshPreparing=nullptr;p->kill();p->deleteLater();dshEvent(view,{{"type","dsh_done"},{"stopReason","cancelled"}});return;}}
    view->networkRetry.clear();
    emit retryChanged();
    writePi(view, {{"type", "abort"}});
    view->approval.clear();
    emit approvalChanged();emit chatChanged();emit sidebarChanged();
  } else if (view->busy()) {
    view->cancelled=true;finishTurn(view);view->promptStarted.reset();
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
void Backend::grantDangerousApproval() {
  if(!canGrantDangerousApproval())return;
  view->dangerousAllowed=true;
  writePi(view,{{"type","dangerous_approval"},{"allowed",true}});
  respondApproval(true);
}
void Backend::revokeDangerousApproval() {
  view->dangerousAllowed=false;
  writePi(view,{{"type","dangerous_approval"},{"allowed",false}});
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
  if(saved["backend"]=="dsh") {
    if(!QRegularExpression("^dsh-[a-f0-9-]{32,36}$").match(id).hasMatch()){emit toast("会话编号无效");return;}
    if(action["action"]=="rename") {
      auto info=saved;info["title"]=action["title"];for(auto &row:sessionIndex)if(row.toMap()["id"]==id)row=info;
      if(record)record->title=action["title"].toString();saveSessions();emit chatChanged();return;
    }
    if(action["action"]=="export") {
      const auto file=store.directory+"/dsh/transcripts/"+id+".json",target=action["outputFile"].toString();
      auto watcher=new QFutureWatcher<bool>(this);auto previous=dshWriter;
      connect(watcher,&QFutureWatcher<bool>::finished,this,[this,watcher]{const auto ok=watcher->result();watcher->deleteLater();emit toast(ok?"会话显示历史已导出":"会话导出失败");});
      watcher->setFuture(QtConcurrent::run([file,target,previous]() mutable {previous.waitForFinished();QFile source(file);if(!source.open(QIODevice::ReadOnly))return false;QSaveFile output(target);if(!output.open(QIODevice::WriteOnly))return false;while(!source.atEnd()){const auto data=source.read(65536);if(data.isEmpty() && source.error()!=QFile::NoError)return false;if(output.write(data)!=data.size())return false;}return output.commit();}));return;
    }
  }
  if (record)
    retire(record);
  const auto work = beginControlWork();
  action["sessionFile"] = saved["file"];
  action["cwd"] = saved.value("workspace", store.directory);
  action["sessionDir"] = store.directory + "/pi/sessions";
  U::run(this, U::node(), {release + "/bridge.mjs", "--session-action"},
         QJsonDocument::fromVariant(action).toJson(QJsonDocument::Compact),
         [this, id, record, work](bool ok, QByteArray raw, QString error) {
           auto result = QJsonDocument::fromJson(raw).object().toVariantMap();
           if (!ok || !result["ok"].toBool()) {
             emit toast(result.value("error", error).toString());
             endControlWork(work, {{"ok", false}, {"error", result.value("error", error)}});
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
           endControlWork(work, {{"ok", true}, {"value", result}});
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
    if(source["backend"]=="dsh"){emit toast("DeepSeek Harness 首版不支持会话分支，请新建聊天。");return;}
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
void Backend::deleteSession(QString id) { files->deleteSession(id); }
