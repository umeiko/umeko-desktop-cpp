#include "backend.hpp"
#include "global_agents.hpp"
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace {
void require(bool ok, const QString &message) {
  if (!ok) throw std::runtime_error(message.toStdString());
}
QVariant redact(QVariant value) {
  if (value.metaType().id() == QMetaType::QVariantMap) {
    auto map = value.toMap();
    static const QRegularExpression secret("^(key|token|credentials|authorization)$|password$|secret$|apikey$|accesstoken$|refreshtoken$|bearertoken$");
    for (auto it = map.begin(); it != map.end(); ++it) {
      auto name = it.key().toLower(); name.remove(QRegularExpression("[^a-z]"));
      const auto type = it.value().metaType().id();
      const bool content = (type == QMetaType::QString && !it.value().toString().isEmpty()) ||
                           (type == QMetaType::QVariantMap && !it.value().toMap().isEmpty()) ||
                           (type == QMetaType::QVariantList && !it.value().toList().isEmpty());
      if (it.key() == "fields" && type == QMetaType::QVariantList) {
        auto fields = it.value().toList();
        for (auto &item : fields) {
          const auto original = item.toMap();
          auto field = redact(original).toMap();
          for (const auto &key : {"key", "type", "secret"})
            if (original.contains(key)) field[key] = original[key];
          if (original.value("secret").toBool() && original.contains("value")) field["value"] = "[redacted]";
          item = field;
        }
        it.value() = fields;
      } else it.value() = content && !name.startsWith("has") && secret.match(name).hasMatch() ? QVariant("[redacted]") : redact(it.value());
    }
    return map;
  }
  if (value.metaType().id() == QMetaType::QVariantList) {
    auto list = value.toList();
    for (auto &item : list) item = redact(item);
    return list;
  }
  return value;
}
QString json(QVariant value) { return QString::fromUtf8(QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact)); }
}

void Backend::initUmekoControl() {
  controlOperations = U::readJson(root + "/runtime/umeko-operations.json", QVariantMap{}).toMap()["operations"].toMap();
  for (auto it = controlOperations.begin(); it != controlOperations.end();) {
    const auto spec = it.value().toMap();
    if (spec.contains("editions") && !spec["editions"].toStringList().contains("cpp")) it = controlOperations.erase(it); else ++it;
  }
  controlTimer.setInterval(150);
  connect(about, &AppAbout::changed, this, [this] {
    const auto pending = controlAboutWork;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
      const auto value = it.key() == "updates.check" ? about->release() : it.key() == "updates.download" ? about->download() : about->installation();
      const auto state = value["state"].toString();
      const bool done = it.key() == "updates.check" ? !about->busy() : it.key() == "updates.download" ? QStringList{"ready", "paused", "error"}.contains(state) : state == "error" || value.value("busy", true) == false;
      if (!done) continue;
      controlAboutWork.remove(it.key());
      const bool ok = state != "error" && state != "paused";
      endControlWork(it.value(), ok ? QVariantMap{{"ok", true}, {"value", value}} : QVariantMap{{"ok", false}, {"error", value.value("message", "Update operation failed")}});
    }
  });
  connect(&controlTimer, &QTimer::timeout, this, [this] {
    if (closing) { controlTimer.stop(); return; }
    const auto order = controlJobOrder;
    for (const auto &id : order) {
      const auto job = controlJobs[id].toMap();
      if (job["status"] != "queued") continue;
      const auto operation = job["operation"].toString();
      const auto args = job["arguments"].toMap();
      const auto caller = records.value(job["caller"].toString());
      if (QDateTime::currentMSecsSinceEpoch() - job["created"].toLongLong() > 600000) {
        finishControlJob(id, {{"ok", false}, {"error", "Timed out waiting for an idle desktop task"}});
      } else if (umekoBlocked(controlOperations.value(operation, QVariantMap{{"idle", "all"}}).toMap(), args, caller)) break;
      else {
        auto active = job; active["status"] = "running"; active.remove("arguments"); controlJobs[id] = active;
        const auto result = umekoRequest({{"operation", operation}, {"arguments", args}}, job["caller"].toString(), false, id);
        if (!result["ok"].toBool() || result["status"] != "accepted") finishControlJob(id, result);
        emit toast(result["ok"].toBool() ? (controlJobs[id].toMap()["status"] == "running" ? "Umeko 配置任务已开始" : "Umeko 配置任务已应用") : "Umeko 配置任务失败：" + result["error"].toString());
      }
    }
    bool pending = false;
    for (const auto &job : controlJobs) pending |= job.toMap()["status"] == "queued";
    if (!pending) controlTimer.stop();
  });
}

QVariantList Backend::umekoSessions() const {
  QVariantList result;
  for (auto value : sessionIndex) {
    const auto source = value.toMap();
    QVariantMap row;
    for (const auto &key : {"id", "title", "file", "workspace", "model", "provider"}) row[key] = source.value(key, "");
    const auto meta = store.data["sessionMeta"].toMap()[row["id"].toString()].toMap();
    for (auto it = meta.cbegin(); it != meta.cend(); ++it) row[it.key()] = it.value();
    bool running = false;
    for (const auto &s : records) if (s->id == row["id"] && s->busy()) running = true;
    row["running"] = running;
    result << row;
  }
  return result;
}

bool Backend::umekoBlocked(QVariantMap spec, QVariantMap args, std::shared_ptr<Session> caller) const {
  const auto mode = spec["idle"].toString(), id = args["id"].toString();
  if (spec["target"] == "management" && (management->busy() || kernelBusy())) return true;
  if (spec["method"] == "checkKernel" && (view->busy() || kernelBusy())) return true;
  if (spec["target"] == "about" && (about->busy() || !controlAboutWork.isEmpty())) return true;
  if (mode == "all" && (modelsBusy() || management->busy() || kernelBusy())) return true;
  if (mode == "view" && (view->busy() || (caller && caller->busy()))) return true;
  if (mode == "session")
    for (const auto &s : records) if (s->id == id && s->busy()) return true;
  if (mode == "provider" && !id.isEmpty()) {
    if (modelsBusy(id)) return true;
    for (auto row : providers->rows()) if (row.toMap()["id"] == id && row.toMap()["busy"].toBool()) return true;
  }
  return false;
}

QVariantMap Backend::queueUmeko(QString operation, QVariantMap args, QString caller) {
  int pending = 0;
  for (auto job : controlJobs) if (job.toMap()["status"] == "queued") ++pending;
  require(pending < 32, "Too many pending Umeko changes; wait for existing jobs");
  const auto id = newControlJob(operation, "queued");
  auto job = controlJobs[id].toMap(); job["arguments"] = args; job["caller"] = caller; controlJobs[id] = job;
  controlTimer.start();
  return {{"ok", true}, {"status", "queued"}, {"jobId", id}, {"message", "等待相关任务结束后应用；使用 jobs.get 查看结果。"}};
}

QString Backend::newControlJob(QString operation, QString status) {
  while (controlJobOrder.size() >= 128) {
    auto found = std::find_if(controlJobOrder.cbegin(), controlJobOrder.cend(), [this](const QString &id) { return QStringList{"done", "failed"}.contains(controlJobs[id].toMap()["status"].toString()); });
    if (found == controlJobOrder.cend()) break;
    const auto id = *found; controlJobOrder.removeAll(id); controlJobs.remove(id);
  }
  const auto id = U::uuid();
  controlJobs[id] = QVariantMap{{"operation", operation}, {"status", status}, {"created", QDateTime::currentMSecsSinceEpoch()}};
  controlJobOrder << id;
  return id;
}
QString Backend::beginControlWork() {
  if (controlDispatch.isEmpty()) return {};
  const auto token = U::uuid();
  controlWorkJobs[token] = controlDispatch; controlPending[controlDispatch].insert(token);
  return token;
}
void Backend::finishControlJob(QString id, QVariantMap result) {
  if (id.isEmpty() || !controlJobs.contains(id)) return;
  auto job = controlJobs[id].toMap();
  job["status"] = result["ok"].toBool() ? "done" : "failed"; job["result"] = redact(result); job.remove("arguments");
  controlJobs[id] = job; controlPending.remove(id);
  for (auto it = controlAboutWork.begin(); it != controlAboutWork.end();) { if (controlWorkJobs.value(it.value()) == id) it = controlAboutWork.erase(it); else ++it; }
  for (auto it = controlWorkJobs.begin(); it != controlWorkJobs.end();) { if (it.value() == id) it = controlWorkJobs.erase(it); else ++it; }
}
void Backend::endControlWork(QString token, QVariantMap result) {
  const auto id = controlWorkJobs.take(token);
  if (id.isEmpty()) return;
  controlPending[id].remove(token);
  if (!result["ok"].toBool() || controlPending[id].isEmpty()) finishControlJob(id, result);
}

QVariantMap Backend::umekoRequest(QVariantMap request, QString callerKey, bool queue, QString jobId) {
  return umekoRequestImpl(request, callerKey, queue, jobId, false);
}

QVariantMap Backend::confirmedWidgetInstall(QVariantMap arguments, QString callerKey) {
  return umekoRequestImpl({{"operation", "extensions.install"}, {"arguments", arguments}}, callerKey, false, {}, true);
}

QVariantMap Backend::umekoRequestImpl(QVariantMap request, QString callerKey, bool queue, QString jobId, bool widgetInstall) {
  const auto operation = request["operation"].toString();
  auto finish = [&operation](QVariantMap result) { if (!result.contains("ok")) result["ok"] = true; result["operation"] = operation; return result; };
  try {
    require(operation != "extensions.install" || widgetInstall, "Agent package installation must use run_pi_command and its native installation permission; confirmed Widget installs use the private host entry");
    for (auto it = request.cbegin(); it != request.cend(); ++it) require(it.key() == "operation" || it.key() == "arguments", "Expected operation and arguments");
    require(request.contains("operation"), "Expected operation and arguments");
    require(!request.contains("arguments") || request["arguments"].metaType().id() == QMetaType::QVariantMap, "arguments must be an object");
    auto args = request["arguments"].toMap();
    require(json(args).size() <= 2 * 1024 * 1024, "Desktop arguments exceed 2 MiB");
    const auto caller = records.value(callerKey);
    if(operation.startsWith("agent_bus."))return finish(desktopExtensions->globalAgents()->execute(operation,args,callerKey));
    require(!caller || caller->globalAgent.isEmpty() || operation == "help" || operation == "models.list", "全局协调子智能体只能使用 agent_bus.* 和 models.list；其他任务请委派给已开启桥的工作区代理");
    if(operation.startsWith("widgets."))return finish(desktopExtensions->execute(operation,args,callerKey));
    if (operation == "help") {
      const auto directory = QFileInfo::exists(store.directory + "/agent-docs/README.md") ? store.directory + "/agent-docs" : release + "/umeko-docs";
      const QVariantMap topics{{"plugins",directory+"/plugins.md"},{"provider",directory+"/provider-plugins.md"},{"widget",directory+"/widget-plugins.md"},{"agents",directory+"/global-agents.md"}};
      const auto topic=args["topic"].toString();
      require(topic.isEmpty() || topics.contains(topic),"topic 请选择 plugins、provider、widget 或 agents");
      QVariantMap result{{"docsRoot",directory},{"entry",topic.isEmpty()?directory+"/README.md":topics[topic].toString()},{"topics",topics}};
      if(topic=="agents") {
        QFile guide(topics[topic].toString());
        require(guide.open(QIODevice::ReadOnly),"无法读取全局子智能体指南");
        result["document"]=QString::fromUtf8(guide.read(65536));
      }
      if(topic=="widget"||topic=="plugins")result["activeWidgets"]=desktopExtensions->execute("widgets.list",{{"activeOnly",true}},callerKey)["widgets"];
      return finish(result);
    }
    if (operation == "operations.list") {
      QVariantMap found;
      for (auto it = controlOperations.cbegin(); it != controlOperations.cend(); ++it)
        if (it.key().startsWith(args["prefix"].toString())) found[it.key()] = it.value();
      return finish({{"operations", found}, {"reads", QStringList{"settings.get", "providers.list", "models.list", "characters.list", "characters.get", "characters.config", "plugins.list", "plugins.getConfig", "themes.list", "projects.list", "sessions.list", "sessions.get", "mcp.get", "status.get", "jobs.get"}}});
    }
    if (operation == "batch") {
      auto items = args["operations"].toList();
      require(!items.isEmpty() && items.size() <= 32, "batch needs 1-32 operations");
      QVariantList results; bool ok = true;
      for (auto item : items) {
        require(item.metaType().id() == QMetaType::QVariantMap && item.toMap()["operation"] != "batch", "Nested or invalid batch");
        auto result = umekoRequest(item.toMap(), callerKey, queue); results << result; ok &= result["ok"].toBool();
      }
      return finish({{"ok", ok}, {"results", results}});
    }
    if (operation == "settings.get") { auto settings = config(); settings.remove("sessionMeta"); return finish({{"config", redact(settings)}, {"activeSession", activeSession()}, {"callerSession", caller ? caller->id : ""}}); }
    if (operation == "providers.list") return finish({{"providers", redact(config()["providers"])}, {"presets", commonProviders()}});
    if (operation == "models.list") {
      QVariantList models;
      for (const auto &value : modelCatalog()) {
        auto row = value.toMap();
        row["model"] = "umeko/" + row["id"].toString();
        row["provider"] = "umeko";
        row["displayProvider"] = row["owned_by"];
        models << row;
      }
      return finish({{"models", models}});
    }
    if (operation == "characters.list") return finish({{"selected", characterStyle()["id"]}, {"characters", characterPacks()}});
    if (operation == "characters.get") return finish({{"character", characterStyle()}});
    if (operation == "characters.config") return finish(characterConfiguration());
    if (operation == "plugins.list") return finish({{"accounts", redact(providers->rows())}, {"packages", providers->packages()}});
    if (operation == "plugins.resource") {
      const auto kind = args["kind"].toString();
      require(kind == "sdk" || kind == "develop", "kind must be sdk or develop");
      const auto name = kind == "sdk" ? "umeko-provider-sdk-0.2.2.zip" : "develop-umeko-provider.zip";
      for (const auto &folder : {root + "/provider-resources/", root + "/build/provider-resources/"})
        if (QFileInfo::exists(folder + name)) return finish({{"path", folder + name}});
      require(false, "Bundled provider development resource is missing");
    }
    if (operation == "plugins.getConfig") {
      require(provider(args["id"].toString())["api"] == "plugin", "参数 id 需要供应商/账号 ID；请通过 plugins.list 查看 accounts[].id，包操作使用 plugin / version 参数");
      return finish({{"config", redact(QJsonDocument::fromJson(providers->configJson(args["id"].toString()).toUtf8()).toVariant())}});
    }
    if (operation == "themes.list") return finish({{"selected", config()["theme"]}, {"themes", customThemes()}, {"builtins", QStringList{"light", "dark", "yanami"}}});
    if (operation == "themes.prompt") {
      QFile file(":/qt/qml/Umeko/App/app/theme-prompt.txt");
      if (!file.open(QIODevice::ReadOnly)) { file.setFileName(root + "/app/theme-prompt.txt"); require(file.open(QIODevice::ReadOnly), "Theme prompt is missing"); }
      return finish({{"prompt", QString::fromUtf8(file.readAll())}});
    }
    if (operation == "projects.list") return finish({{"projects", store.data["projects"]}});
    if (operation == "sessions.list") {
      const auto offset = args.value("offset", 0).toInt(), limit = args.value("limit", 100).toInt();
      require(offset >= 0 && limit >= 1 && limit <= 200, "offset >= 0; limit 1-200");
      QVariantList rows;
      for (auto row : umekoSessions()) if (args["workspace"].toString().isEmpty() || row.toMap()["workspace"] == args["workspace"]) rows << row;
      return finish({{"sessions", rows.mid(offset, limit)}, {"total", rows.size()}});
    }
    if (operation == "sessions.get") {
      auto id = args.value("id", caller ? caller->id : activeSession()).toString();
      for (auto row : umekoSessions()) if (row.toMap()["id"] == id) return finish({{"session", row}});
      require(false, "Session ID does not exist");
    }
    if (operation == "mcp.get") {
      const auto scope = args.value("scope", "global").toString();
      return finish({{"path", mcpConfigPath(scope)}, {"config", redact(QJsonDocument::fromJson(readMcp(scope).toUtf8()).toVariant())}});
    }
    if (operation == "status.get") return finish({
      {"lastSendError", sendError}, {"kernel", QVariantMap{{"busy", kernelBusy()}, {"status", kernelStatus()}, {"version", kernelVersion()}}},
      {"management", QVariantMap{{"busy", management->busy()}, {"status", management->status()}, {"extensions", management->extensions()}, {"npm", redact(management->npmConfig())}, {"npmStatus", management->npmStatus()}}},
      {"plugins", redact(providers->rows())}, {"updates", QVariantMap{{"busy", about->busy()}, {"release", about->release()}, {"download", about->download()}, {"installation", about->installation()}}},
      {"serverRunning", serverRunning()}, {"keys", proxyKeys()}});
    if (operation == "jobs.get") {
      const auto id = args["id"].toString();
      require(id.isEmpty() || controlJobs.contains(id), "Job ID does not exist");
      QVariantList rows;
      for (const auto &key : controlJobOrder) if (id.isEmpty() || id == key) {
        auto job = controlJobs[key].toMap();
        auto target=caller?caller:view;
        if(job.contains("widgetSession")&&job["widgetSession"]!=target->key&&!(job["widgetSessionId"]==target->id&&!target->id.isEmpty()&&job["widgetWorkspace"]==target->workspace))continue;
        QVariantMap row{{"id", key}};
        for (const auto &field : {"operation", "status", "result"}) if (job.contains(field)) row[field] = job[field];
        rows << row;
      }
      return finish({{"jobs", rows}});
    }
    if (operation == "models.select" || operation == "sessions.reasoning") {
      auto target = args["sessionId"].toString().isEmpty() ? (caller ? caller : view) : findSession(args["sessionId"].toString());
      require(bool(target), "Session is no longer loaded");
      if (operation == "models.select") {
        if (target->busy() && queue) { args["sessionId"] = target->id; return finish(queueUmeko(operation, args, callerKey)); }
        require(!target->busy(), "Session is still busy");
        bool valid = false;
        for (auto m : modelCatalog()) if (m.toMap()["provider"] == args["provider"] && m.toMap()["id"] == args["model"]) valid = true;
        require(valid, "Configured model ID does not exist");
        target->provider = args["provider"].toString(); target->model = args["model"].toString(); target->usage.clear();
        if (target == view) { store.data["selectedProvider"] = target->provider; store.data["selectedModel"] = target->model; }
      } else {
        auto value = args["value"].toString();
        require(QStringList{"off", "low", "high", "max"}.contains(value), "Reasoning must be off, low, high or max");
        target->reasoningEffort = value;
        if (!target->id.isEmpty()) setMetadata(target->id, {{"reasoningEffort", value}});
        if (target == view) { auto ui = store.data["ui"].toMap(); ui["reasoningEffort"] = value; store.data["ui"] = ui; }
        writePi(target, {{"type", "reasoning_effort"}, {"value", value}});
      }
      store.save(); emit changed(); emit reasoningChanged(); emit contextUsageChanged();
      return finish({{"status", "applied"}, {"sessionId", target->id}});
    }
    require(controlOperations.contains(operation), "Unknown Umeko operation; read the documentation or use operations.list");
    const auto spec = controlOperations[operation].toMap();
    const auto fields = spec["args"].toList();
    QSet<QString> names; QVariantList values;
    for (auto field : fields) {
      auto entry = field.toList(); const auto name = entry[0].toString(), kind = entry[1].toString(); names.insert(name);
      require(args.contains(name) || entry.size() > 2, "Missing argument: " + name);
      auto value = args.value(name, entry.value(2)); const auto type = value.metaType().id();
      const bool numeric = type == QMetaType::Double || type == QMetaType::Int || type == QMetaType::LongLong;
      const bool valid = kind == "string" ? type == QMetaType::QString : kind == "boolean" ? type == QMetaType::Bool :
                         kind == "json" ? type == ((name == "config" || name == "fields") ? QMetaType::QVariantMap : QMetaType::QVariantList) :
                         numeric && std::isfinite(value.toDouble()) && (kind != "integer" || value.toDouble() == std::floor(value.toDouble()));
      require(valid, name + " must be " + kind); values << value;
    }
    for (auto it = args.cbegin(); it != args.cend(); ++it) require(names.contains(it.key()), "Unknown argument: " + it.key());
    if (operation == "plugins.addAccount" || operation == "plugins.uninstall") {
      bool found = false;
      for (const auto &value : providers->packages()) { const auto package = value.toMap(); if (package["id"] == args["plugin"] && package["version"] == args["version"]) found = true; }
      require(found, "参数 plugin 需要已安装的插件包 ID，version 需要该包的已安装版本；请通过 plugins.list 查看 packages[].id / packages[].version。账号操作的 id 使用 accounts[].id");
    }
    const auto exists = spec["exists"].toString(), id = args.value("id").toString();
    if (!exists.isEmpty()) {
      const auto rows = exists == "provider" ? config()["providers"].toList() : exists == "project" ? store.data["projects"].toList() : umekoSessions();
      bool found = false; for (auto row : rows) if (row.toMap()["id"] == id && (spec["target"] != "plugins" || row.toMap()["api"] == "plugin")) found = true;
      require(found, spec["target"] == "plugins" ? QString("参数 id 需要供应商/账号 ID，不能使用插件包 ID；请通过 plugins.list 查看 accounts[].id，包操作使用 plugin / version 参数") : exists + " ID does not exist");
    }
    if (operation == "appearance.language") require(QStringList{"zh", "en"}.contains(args["language"].toString()), "language must be zh or en");
    if (operation == "appearance.fontScale") require(args["scale"].toDouble() >= .5 && args["scale"].toDouble() <= 2, "scale must be 0.5-2");
    if (operation == "themes.select") {
      bool found = QStringList{"light", "dark", "yanami"}.contains(id);
      for (auto theme : customThemes()) if (theme.toMap()["id"] == id) found = true;
      require(found, "Theme ID does not exist");
    }
    if (operation == "themes.remove") {
      bool found = false; for (auto theme : customThemes()) if (theme.toMap()["id"] == id) found = true;
      require(found, "Custom theme ID does not exist");
    }
    if (operation == "items.pin") {
      const auto kind = args["kind"].toString();
      const auto rows = kind == "project" ? store.data["projects"].toList() : kind == "session" ? umekoSessions() : QVariantList{};
      bool found = false; for (auto row : rows) if (row.toMap()["id"] == id) found = true;
      require(found, "Pin target does not exist");
    }
    if (operation == "projects.add") {
      require(!values[0].toList().isEmpty(), "paths must contain existing directories");
      for (auto path : values[0].toList()) require(path.metaType().id() == QMetaType::QString && QDir(U::cleanPath(path.toString())).exists(), "paths must contain existing directories");
    }
    if (operation == "workspace.set") require(QDir(U::cleanPath(args["path"].toString())).exists(), "Workspace directory does not exist");
    if (operation == "extensions.activation") require(QStringList{"global","session","disabled"}.contains(args["mode"].toString()), "Invalid extension activation mode");
    if (operation == "agent.save") {
      const auto patch = values[0].toMap();
      const QSet<QString> allowed{"maxTokens", "instructions", "autoCompaction", "agentDir", "skillPaths", "extensionPaths"};
      for (auto it = patch.cbegin(); it != patch.cend(); ++it) require(allowed.contains(it.key()), "Unknown Agent setting");
      if (patch.contains("maxTokens")) {
        const auto value = patch["maxTokens"];
        const auto type = value.metaType().id();
        require((type == QMetaType::Int || type == QMetaType::LongLong || type == QMetaType::Double) && value.toDouble() == std::floor(value.toDouble()) && value.toDouble() >= 256 && value.toDouble() <= 32768, "maxTokens must be an integer from 256 to 32768");
      }
      if (patch.contains("autoCompaction")) require(patch["autoCompaction"].metaType().id() == QMetaType::Bool, "autoCompaction must be boolean");
      for (auto key : {"instructions", "agentDir"}) if (patch.contains(key)) require(patch[key].metaType().id() == QMetaType::QString, QString(key) + " must be a string");
      require(patch["instructions"].toString().size() <= 16000, "instructions exceeds 16000 characters");
      for (auto key : {"skillPaths", "extensionPaths"}) if (patch.contains(key)) {
        require(patch[key].metaType().id() == QMetaType::QVariantList, QString(key) + " must be an array of paths");
        for (auto path : patch[key].toList()) require(path.metaType().id() == QMetaType::QString, "Resource path must be a string");
      }
    }
    if (operation == "network.npm") {
      auto patch = values[0].toMap(); auto merged = management->npmConfig();
      for (auto it = patch.cbegin(); it != patch.cend(); ++it) { require(merged.contains(it.key()), "Unknown npm setting"); merged[it.key()] = it.value(); }
      values[0] = merged;
    }
    if (queue && umekoBlocked(spec, args, caller)) return finish(queueUmeko(operation, args, callerKey));
    require(!umekoBlocked(spec, args, caller), "The relevant task is still busy");
    if (operation == "agent.save") { auto merged = store.data["agent"].toMap(); auto patch = values[0].toMap(); for (auto it = patch.cbegin(); it != patch.cend(); ++it) merged[it.key()] = it.value(); values[0] = merged; }
    if (operation == "projects.add") { QVariantList urls; for (auto path : values[0].toList()) urls << QUrl::fromLocalFile(U::cleanPath(path.toString())).toString(); values[0] = urls; }
    for (int i = 0; i < fields.size(); ++i) {
      auto field = fields[i].toList();
      if (field[0] == "path") values[i] = QUrl::fromLocalFile(U::cleanPath(values[i].toString())).toString();
      if (field[1] == "json") values[i] = json(values[i]);
    }
    const auto targetName = spec["target"].toString();
    QObject *target = targetName == "plugins" ? static_cast<QObject *>(providers) : targetName == "management" ? static_cast<QObject *>(management) : targetName == "about" ? static_cast<QObject *>(about) : this;
    QMetaMethod method;
    for (int i = 0; i < target->metaObject()->methodCount(); ++i) {
      auto candidate = target->metaObject()->method(i);
      if (candidate.name() == spec["method"].toString().toUtf8() && candidate.parameterCount() == values.size()) { method = candidate; break; }
    }
    require(method.isValid(), "Desktop method unavailable in this edition");
    QGenericArgument arguments[6];
    for (int i = 0; i < values.size(); ++i) {
      require(values[i].convert(method.parameterMetaType(i)), "Invalid desktop parameter type");
      arguments[i] = QGenericArgument(method.parameterMetaType(i).name(), values[i].constData());
    }
    QVariant result;
    if (method.returnMetaType().id() != QMetaType::Void) result = QVariant(method.returnMetaType());
    QGenericReturnArgument returned;
    if (method.returnMetaType().id() != QMetaType::Void) returned = QGenericReturnArgument(method.returnMetaType().name(), result.data());
    if (spec["async"].toBool() && jobId.isEmpty()) jobId = newControlJob(operation, "running");
    const auto previousDispatch = controlDispatch;
    controlDispatch = jobId;
    const auto restoreDispatch = qScopeGuard([this, previousDispatch] { controlDispatch = previousDispatch; });
    if (spec["async"].toBool() && targetName == "about") controlAboutWork[operation] = beginControlWork();
    QStringList messages;
    const auto listener = connect(this, &Backend::toast, this, [&messages](QString text) { messages << text; });
    QSet<QString> before; for (auto p : config()["providers"].toList()) before.insert(p.toMap()["id"].toString());
    const bool invoked = method.invoke(target, Qt::DirectConnection, returned, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4], arguments[5]);
    disconnect(listener);
    controlDispatch = previousDispatch;
    require(invoked, "Desktop method could not be invoked");
    if ((operation == "extensions.install" || operation == "mcp.add") && management->installState() == "failed")
      require(false, management->installMessage());
    bool rejected = result.metaType().id() == QMetaType::Bool && !result.toBool();
    for (const auto &message : messages) if (QRegularExpression("无效|失败|请先|请等待|不能|不存在|请选择|无法|不支持|错误|不能为空").match(message).hasMatch()) rejected = true;
    require(!rejected, messages.isEmpty() ? "Umeko rejected the operation" : messages.last());
    QVariantMap output{{"status", spec["async"].toBool() ? "accepted" : "applied"}};
    if (result.isValid()) output["result"] = redact(result);
    if (!messages.isEmpty()) output["messages"] = messages;
    QStringList created; for (auto p : config()["providers"].toList()) if (!before.contains(p.toMap()["id"].toString())) created << p.toMap()["id"].toString();
    if (!created.isEmpty()) output["createdProviderIds"] = created;
    if (!jobId.isEmpty()) {
      output["jobId"] = jobId;
      if (controlJobs[jobId].toMap()["status"] == "failed") { auto failed = controlJobs[jobId].toMap()["result"].toMap(); failed["jobId"] = jobId; return finish(failed); }
      if (targetName == "about" && !about->busy() && about->download().value("state") != "downloading" && about->download().value("state") != "verifying" && about->installation().value("state") != "preparing") {
        endControlWork(controlAboutWork.take(operation), {{"ok", true}, {"messages", messages}});
      }
      if (controlPending.value(jobId).isEmpty() && controlJobs[jobId].toMap()["status"] == "running") finishControlJob(jobId, finish(output));
    }
    return finish(output);
  } catch (const std::exception &error) {
    auto result = finish({{"ok", false}, {"status", "failed"}, {"error", QString::fromUtf8(error.what())}});
    if (!jobId.isEmpty()) { finishControlJob(jobId, result); result["jobId"] = jobId; }
    return result;
  }
}
