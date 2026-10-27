#include "backend.hpp"
#include <cmath>
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
      it.value() = content && !name.startsWith("has") && secret.match(name).hasMatch() ? QVariant("[redacted]") : redact(it.value());
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
  connect(&controlTimer, &QTimer::timeout, this, [this] {
    if (closing) { controlTimer.stop(); return; }
    bool pending = false;
    for (const auto &id : controlJobOrder) {
      auto job = controlJobs[id].toMap();
      if (job["status"] != "queued") continue;
      const auto operation = job["operation"].toString();
      const auto args = job["arguments"].toMap();
      const auto caller = records.value(job["caller"].toString());
      if (QDateTime::currentMSecsSinceEpoch() - job["created"].toLongLong() > 600000) {
        job["status"] = "failed";
        job["result"] = QVariantMap{{"ok", false}, {"error", "Timed out waiting for an idle desktop task"}};
      } else if (umekoBlocked(controlOperations.value(operation, QVariantMap{{"idle", "all"}}).toMap(), args, caller)) {
        pending = true; break;
      } else {
        const auto result = umekoRequest({{"operation", operation}, {"arguments", args}}, job["caller"].toString(), false);
        job["result"] = result;
        job["status"] = !result["ok"].toBool() ? "failed" : result["status"] == "accepted" ? "accepted" : "completed";
        emit toast(result["ok"].toBool() ? "Umeko 配置任务已应用" : "Umeko 配置任务失败：" + result["error"].toString());
      }
      job.remove("arguments");
      controlJobs[id] = job;
    }
    for (const auto &job : controlJobs) if (job.toMap()["status"] == "queued") pending = true;
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
  while (controlJobOrder.size() >= 128) {
    bool removed = false;
    for (const auto &id : std::as_const(controlJobOrder)) if (controlJobs[id].toMap()["status"] != "queued") {
      controlJobs.remove(id); controlJobOrder.removeAll(id); removed = true; break;
    }
    if (!removed) break;
  }
  const auto id = U::uuid();
  controlJobs[id] = QVariantMap{{"operation", operation}, {"arguments", args}, {"caller", caller},
                               {"created", QDateTime::currentMSecsSinceEpoch()}, {"status", "queued"}};
  controlJobOrder << id;
  controlTimer.start();
  return {{"ok", true}, {"status", "queued"}, {"jobId", id}, {"message", "等待相关任务结束后应用；使用 jobs.get 查看结果。"}};
}

QVariantMap Backend::umekoRequest(QVariantMap request, QString callerKey, bool queue) {
  const auto operation = request["operation"].toString();
  auto finish = [&operation](QVariantMap result) { if (!result.contains("ok")) result["ok"] = true; result["operation"] = operation; return result; };
  try {
    for (auto it = request.cbegin(); it != request.cend(); ++it) require(it.key() == "operation" || it.key() == "arguments", "Expected operation and arguments");
    require(request.contains("operation"), "Expected operation and arguments");
    require(!request.contains("arguments") || request["arguments"].metaType().id() == QMetaType::QVariantMap, "arguments must be an object");
    auto args = request["arguments"].toMap();
    require(json(args).size() <= 2 * 1024 * 1024, "Desktop arguments exceed 2 MiB");
    const auto caller = records.value(callerKey);
    if (operation == "help") return finish({{"docsRoot", release + "/umeko-docs"}, {"entry", release + "/umeko-docs/README.md"}});
    if (operation == "operations.list") {
      QVariantMap found;
      for (auto it = controlOperations.cbegin(); it != controlOperations.cend(); ++it)
        if (it.key().startsWith(args["prefix"].toString())) found[it.key()] = it.value();
      return finish({{"operations", found}, {"reads", QStringList{"settings.get", "providers.list", "plugins.list", "plugins.getConfig", "themes.list", "projects.list", "sessions.list", "sessions.get", "mcp.get", "status.get", "jobs.get"}}});
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
    if (operation == "plugins.list") return finish({{"accounts", redact(providers->rows())}, {"packages", providers->packages()}});
    if (operation == "plugins.resource") {
      const auto kind = args["kind"].toString();
      require(kind == "sdk" || kind == "develop", "kind must be sdk or develop");
      const auto name = kind == "sdk" ? "umeko-provider-sdk-0.2.1.zip" : "develop-umeko-provider.zip";
      for (const auto &folder : {root + "/provider-resources/", root + "/build/provider-resources/"})
        if (QFileInfo::exists(folder + name)) return finish({{"path", folder + name}});
      require(false, "Bundled provider development resource is missing");
    }
    if (operation == "plugins.getConfig") {
      require(!provider(args["id"].toString()).isEmpty(), "Provider ID does not exist");
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
      {"management", QVariantMap{{"busy", management->busy()}, {"status", management->status()}, {"extensions", management->extensions()}, {"npm", redact(management->npmConfig())}, {"npmStatus", management->npmStatus()}}},
      {"plugins", redact(providers->rows())}, {"updates", QVariantMap{{"busy", about->busy()}, {"release", about->release()}, {"download", about->download()}, {"installation", about->installation()}}},
      {"serverRunning", serverRunning()}, {"keys", proxyKeys()}});
    if (operation == "jobs.get") {
      const auto id = args["id"].toString();
      require(id.isEmpty() || controlJobs.contains(id), "Job ID does not exist");
      QVariantList rows;
      for (const auto &key : controlJobOrder) if (id.isEmpty() || id == key) {
        auto job = controlJobs[key].toMap();
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
    const auto exists = spec["exists"].toString(), id = args.value("id").toString();
    if (!exists.isEmpty()) {
      const auto rows = exists == "provider" ? config()["providers"].toList() : exists == "project" ? store.data["projects"].toList() : umekoSessions();
      bool found = false; for (auto row : rows) if (row.toMap()["id"] == id) found = true;
      require(found, exists + " ID does not exist");
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
    QStringList messages;
    const auto listener = connect(this, &Backend::toast, this, [&messages](QString text) { messages << text; });
    QSet<QString> before; for (auto p : config()["providers"].toList()) before.insert(p.toMap()["id"].toString());
    const bool invoked = method.invoke(target, Qt::DirectConnection, returned, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4], arguments[5]);
    disconnect(listener);
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
    return finish(output);
  } catch (const std::exception &error) { return finish({{"ok", false}, {"error", QString::fromUtf8(error.what())}}); }
}
