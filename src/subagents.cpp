#include "subagents.hpp"
#include "backend.hpp"
#include <QImageReader>
#include <QtConcurrent>
#include <algorithm>

namespace {
bool active(QVariantMap row) {
  return row["status"] == "running" || row["status"] == "queued";
}
QStringList names{"烧盐柠檬", "小鞠知花", "温水佳树", "志喜屋梦子"};
QStringList avatars{"lemon-yakishio", "chika-komari", "kaju-nukumizu",
                    "yumeko-shikiya"};
} // namespace
Subagents::Subagents(Backend *backend)
    : QObject(backend), b(backend), model(this) {
  stream.setSingleShot(true);
  stream.setInterval(32);
  connect(&stream, &QTimer::timeout, this, &Subagents::publish);
  save.setSingleShot(true);
  save.setInterval(500);
  connect(&save, &QTimer::timeout, this, [this] { b->store.save(); });
  deadline.setInterval(1000);
  connect(&deadline, &QTimer::timeout, this, [this] {
    bool expired = false;
    for (auto id : pending.keys())
      if (QDateTime::currentMSecsSinceEpoch() -
              pending[id]["time"].toLongLong() >
          30000) {
        pending.remove(id);
        expired = true;
      }
    if (expired) {
      emit b->toast("子代理暂未确认收到消息，输入内容已保留");
      emit contextChanged();
    }
  });
  deadline.start();
  connect(b, &Backend::changed, this, [this] { emit changed(); });
  connect(b, &Backend::viewChanged, this, [this] {
    closeView();
    sync(b->view);
    emit changed();
  });
}
QString Subagents::key(std::shared_ptr<Session> record, QString id) const {
  return record ? record->key + "/" + id : QString();
}
QVariantList Subagents::project(std::shared_ptr<Session> record) const {
  if (!record)
    return {};
  QVariantList out;
  int totals[4]{}, counts[4]{};
  const auto configuredNames=b->characterNames();
  for (auto v : record->subagents)
    ++totals[qBound(0, v.toMap()["avatarSlot"].toInt(), 3)];
  for (auto v : record->subagents) {
    auto row = v.toMap();
    const int slot = qBound(0, row["avatarSlot"].toInt(), 3);
    row["name"] = configuredNames.value(slot,names[slot]) + (totals[slot] > 1
                                     ? " · " + QString::number(++counts[slot])
                                     : QString());
    row["avatar"] = avatars[slot];
    row["active"] = active(row);
    out << row;
  }
  return out;
}
QVariantList Subagents::agents() const { return project(b->view); }
QVariantList Subagents::activeAgents() const {
  QVariantList out;
  for (auto v : agents())
    if (active(v.toMap()))
      out << v;
  return out;
}
QVariantMap Subagents::selected() const {
  for (auto v : project(owner))
    if (v.toMap()["id"] == selectedId)
      return v.toMap();
  return {};
}
QVariantMap Subagents::context() const {
  const auto current = key(owner, selectedId);
  bool waiting = false;
  for (auto v : pending)
    if (v["key"] == current)
      waiting = true;
  return {{"loading", loading},
          {"error", error},
          {"truncated", truncated},
          {"pending", waiting},
          {"draft", drafts.value(current)},
          {"attachments", attachments.value(current)}};
}
QVariantList Subagents::queuedMessages() const {
  QVariantList out;
  for (auto v : queues.value(key(owner, selectedId))) {
    auto row = v.toMap();
    auto parsed = U::splitAttachments(row["text"].toString());
    row["text"] = parsed.first;
    row["attachments"] = parsed.second;
    out << row;
  }
  return out;
}
void Subagents::merge(std::shared_ptr<Session> record, QVariantMap row) {
  if (row["id"].toString().isEmpty())
    return;
  int index = -1;
  QVariantMap current;
  for (int i = 0; i < record->subagents.size(); ++i)
    if (record->subagents[i].toMap()["id"] == row["id"]) {
      index = i;
      current = record->subagents[i].toMap();
      break;
    }
  for (auto it = row.begin(); it != row.end(); ++it)
    current[it.key()] = it.value();
  if (!current.contains("avatarSlot")) {
    int counts[4]{};
    for (auto v : record->subagents) {
      auto r = v.toMap();
      if (active(r))
        ++counts[qBound(0, r["avatarSlot"].toInt(), 3)];
    }
    current["avatarSlot"] = int(std::min_element(counts, counts + 4) - counts);
  }
  if (index < 0)
    record->subagents << current;
  else
    record->subagents[index] = current;
  record->subagentsActive = false;
  for (auto v : record->subagents)
    record->subagentsActive |= active(v.toMap());
  if (!record->id.isEmpty()) {
    QVariantList compact;
    for (auto v : record->subagents) {
      auto r = v.toMap();
      for (auto field : {"result", "error", "activity"})
        r.remove(field);
      compact << r;
    }
    auto meta = b->store.data["sessionMeta"].toMap();
    auto entry = meta[record->id].toMap();
    entry["subagents"] = compact;
    meta[record->id] = entry;
    b->store.data["sessionMeta"] = meta;
    save.start();
  }
  if (record == b->view)
    emit changed();
}
void Subagents::sync(std::shared_ptr<Session> record) {
  if (record->subagents.isEmpty())
    for (auto v : b->store.data["sessionMeta"]
                      .toMap()[record->id]
                      .toMap()["subagents"]
                      .toList()) {
      auto row = v.toMap();
      if (active(row))
        row["status"] = "stopped";
      merge(record, row);
    }
  for (auto message : record->messages)
    for (auto tool : message.toMap()["tools"].toList()) {
      auto child = tool.toMap()["subagent"].toMap();
      bool found = false;
      for (auto v : record->subagents)
        if (v.toMap()["id"] == child["id"])
          found = true;
      if (!child.isEmpty() && !found)
        merge(record, child);
    }
}
void Subagents::open(QString id) {
  auto record = b->view;
  sync(record);
  bool found = false;
  for (auto v : record->subagents)
    if (v.toMap()["id"] == id)
      found = true;
  if (!found) {
    emit b->toast("当前会话中没有这个子代理");
    return;
  }
  closeView();
  owner = record;
  selectedId = id;
  loading = true;
  error.clear();
  truncated = false;
  emit changed();
  emit contextChanged();
  emit queueChanged();
  emit requested();
  const auto file = selected()["outputFile"].toString();
  if (record->process) {
    if (!record->ready && !file.isEmpty())
      loadFile(file);
    command(record, {{"action", "watch"}, {"id", id}});
  } else if (!file.isEmpty())
    loadFile(file);
  else if (!command(record, {{"action", "watch"}, {"id", id}})) {
    loading = false;
    error = "暂时无法加载子代理上下文";
    emit contextChanged();
  }
}
void Subagents::closeView() {
  if (owner) {
    auto &items = deferred[owner->key];
    for (int i = items.size() - 1; i >= 0; --i)
      if (items[i].toMap()["action"] == "watch")
        items.removeAt(i);
  }
  if (owner && owner->process)
    b->writePi(owner, {{"type", "subagent-command"}, {"action", "unwatch"}});
  ++generation;
  stream.stop();
  owner.reset();
  selectedId.clear();
  messages.clear();
  model.update({});
  loading = false;
  error.clear();
  emit changed();
  emit contextChanged();
  emit queueChanged();
}
bool Subagents::command(std::shared_ptr<Session> record, QVariantMap input) {
  const bool switching =
      record->busy() &&
      QStringList{"open", "new", "reload", "fork", "navigate", "subagent-open"}
          .contains(record->action);
  if (record->process && record->ready && !switching) {
    input["type"] = "subagent-command";
    b->writePi(record, input);
    return true;
  }
  if (record->process) {
    deferred[record->key] << input;
    return true;
  }
  return !record->busy() &&
         b->command(record, "subagent-open", {{"command", input}});
}
void Subagents::processReady(std::shared_ptr<Session> record) {
  if (!record->ready ||
      record->busy() && QStringList{"open", "new", "reload", "fork", "navigate",
                                    "subagent-open"}
                            .contains(record->action))
    return;
  for (auto v : deferred.take(record->key)) {
    auto input = v.toMap();
    if (input["action"] == "watch" &&
        (record != owner || input["id"] != selectedId))
      continue;
    input["type"] = "subagent-command";
    b->writePi(record, input);
  }
}
void Subagents::processExit(std::shared_ptr<Session> record) {
  const auto prefix = record->key + "/";
  for (const auto &id : queues.keys())
    if (id.startsWith(prefix))
      restore(record, id.mid(prefix.size()), queues.take(id));
  for (const auto &id : pending.keys())
    if (pending[id]["key"].toString().startsWith(prefix))
      pending.remove(id);
  deferred.remove(record->key);
  for (auto value : record->subagents) {
    auto row = value.toMap();
    if (active(row)) {
      row["status"] = "stopped";
      merge(record, row);
    }
  }
  if (owner == record) {
    loading = false;
    emit contextChanged();
    emit queueChanged();
  }
}
void Subagents::setDraft(QString text) {
  if (owner)
    drafts[key(owner, selectedId)] = text;
}
void Subagents::copy(QString text) { b->copy(text); }
bool Subagents::addAttachments(QString raw) {
  if (!owner)
    return false;
  auto &files = attachments[key(owner, selectedId)];
  bool accepted = false;
  for (auto v : QJsonDocument::fromJson(raw.toUtf8()).array()) {
    if (files.size() >= 20) {
      emit b->toast("一次最多添加 20 个附件");
      break;
    }
    QUrl url(v.toString());
    if (!url.isLocalFile())
      continue;
    QFileInfo info(url.toLocalFile());
    auto path = info.canonicalFilePath();
    if (path.isEmpty() || path.contains('\n') || path.contains('\r') ||
        (!info.isFile() && !info.isDir()))
      continue;
    bool exists = false;
    for (auto file : files)
      if (file.toMap()["path"] == path)
        exists = true;
    if (exists) {
      accepted = true;
      continue;
    }
    QVariantMap item{{"id", U::uuid()},
                     {"path", path},
                     {"name", info.fileName()},
                     {"kind", info.isDir() ? "folder" : "file"}};
    if (QStringList{"png", "jpg", "jpeg", "webp", "gif", "bmp"}.contains(
            info.suffix().toLower())) {
      QImageReader reader(path);
      auto size = reader.size();
      if (info.size() > 20 * 1024 * 1024 || !size.isValid() ||
          qint64(size.width()) * size.height() > 40000000 ||
          reader.read().isNull()) {
        emit b->toast("图片过大或无法读取");
        continue;
      }
      item["kind"] = "image";
      item["preview"] = QUrl::fromLocalFile(path).toString();
    }
    files << item;
    accepted = true;
  }
  emit contextChanged();
  return accepted;
}
void Subagents::removeAttachment(QString id) {
  auto &files = attachments[key(owner, selectedId)];
  for (int i = files.size() - 1; i >= 0; --i)
    if (files[i].toMap()["id"] == id)
      files.removeAt(i);
  emit contextChanged();
}
bool Subagents::send(QString text) { return submit(text, false); }
bool Subagents::steer(QString text) {
  if (text.trimmed().isEmpty() && context()["attachments"].toList().isEmpty())
    return !queuedMessages().isEmpty() && queueCommand("promote-queue");
  return submit(text, true);
}
bool Subagents::submit(QString text, bool interrupt) {
  text = text.trimmed();
  const auto current = key(owner, selectedId);
  const auto files = attachments.value(current);
  if (!owner || loading || context()["pending"].toBool() ||
      (text.isEmpty() && files.isEmpty()))
    return false;
  auto prompt = U::attachmentPrompt(text, files);
  if (prompt.size() > 65536) {
    emit b->toast("请输入消息，长度不得超过 64K 字符");
    return false;
  }
  if (selected()["status"] == "queued") {
    emit b->toast("子代理正在排队，请在开始运行后发送指引");
    return false;
  }
  const auto request = U::uuid();
  QVariantList ids;
  for (auto v : files)
    ids << v.toMap()["id"];
  pending[request] = {{"key", current},
                      {"text", text},
                      {"attachmentIds", ids},
                      {"time", QDateTime::currentMSecsSinceEpoch()}};
  drafts[current] = text;
  if (!command(owner, {{"action", "send"},
                       {"id", selectedId},
                       {"text", prompt},
                       {"mode", interrupt ? "interrupt" : "followUp"},
                       {"messageId", request},
                       {"requestId", request}})) {
    pending.remove(request);
    emit b->toast("Pi 尚未就绪，请稍后重试");
    return false;
  }
  emit contextChanged();
  return true;
}
bool Subagents::queueCommand(QString action, QString id) {
  if (!owner)
    return false;
  if (!id.isEmpty()) {
    bool found = false;
    for (auto v : queuedMessages())
      if (v.toMap()["id"] == id)
        found = true;
    if (!found)
      return false;
  }
  return command(owner, {{"action", action},
                         {"id", selectedId},
                         {"queueId", id},
                         {"requestId", U::uuid()}});
}
void Subagents::editQueued(QString id) { queueCommand("edit-queue", id); }
void Subagents::removeQueued(QString id) { queueCommand("delete-queue", id); }
void Subagents::steerQueued(QString id) { queueCommand("steer-queued", id); }
void Subagents::stop() {
  if (owner)
    command(owner, {{"action", "stop"}, {"id", selectedId}});
}
void Subagents::restore(std::shared_ptr<Session> record, QString id,
                        QVariantList items) {
  const auto current = key(record, id);
  QStringList text;
  if (!drafts[current].isEmpty())
    text << drafts[current];
  auto &files = attachments[current];
  for (auto v : items) {
    auto row = v.toMap();
    auto parsed = U::splitAttachments(row["text"].toString());
    auto request = row["id"].toString();
    bool already = false;
    if (pending.contains(request) && pending[request]["key"] == current) {
      already = drafts[current] == pending[request]["text"].toString();
      pending[request]["returned"] = true;
    }
    if (!already && !parsed.first.isEmpty())
      text << parsed.first;
    for (auto f : parsed.second) {
      auto file = f.toMap();
      bool found = false;
      for (auto old : files)
        if (old.toMap()["path"] == file["path"])
          found = true;
      if (!found) {
        file["id"] = U::uuid();
        files << file;
      }
    }
  }
  drafts[current] = text.join("\n\n");
  emit contextChanged();
  if (owner == record && selectedId == id)
    emit draftRestored(id);
}
void Subagents::handle(std::shared_ptr<Session> record, QVariantMap event) {
  const auto type = event["type"].toString(), id = event["id"].toString();
  if (type == "subagents") {
    for (auto row : event["agents"].toList())
      merge(record, row.toMap());
  } else if (type == "subagent_queue") {
    queues[key(record, id)] = event["items"].toList();
    emit queueChanged();
  } else if (type == "subagent_queue_edited" ||
             type == "subagent_queue_returned")
    restore(record, id,
            type == "subagent_queue_edited" ? QVariantList{event["item"]}
                                            : event["items"].toList());
  else if (type == "subagent_command") {
    auto item = pending.take(event["requestId"].toString());
    if (!event["ok"].toBool()) {
      emit b->toast(event.value("message", "子代理操作失败").toString());
      if (event["action"] == "watch" && record == owner) {
        loading = false;
        error = event["message"].toString();
      }
    } else if (!item.isEmpty() && !item["returned"].toBool()) {
      const auto current = item["key"].toString();
      if (drafts[current] == item["text"].toString())
        drafts[current].clear();
      auto &files = attachments[current];
      for (int i = files.size() - 1; i >= 0; --i)
        if (item["attachmentIds"].toList().contains(files[i].toMap()["id"]))
          files.removeAt(i);
      if (current == key(owner, selectedId))
        emit messageSent(selectedId);
    }
    emit contextChanged();
  } else if (record == owner && id == selectedId) {
    if (type == "subagent_context") {
      ++generation;
      error.clear();
      if (!event["file"].toString().isEmpty())
        loadFile(event["file"].toString());
      else {
        messages = event["messages"].toList();
        truncated = event["truncated"].toBool();
        loading = false;
        publish();
      }
    } else if (type == "subagent_event")
      liveEvent(event["event"].toMap());
  }
}
void Subagents::liveEvent(QVariantMap event) {
  loading = false;
  const auto type = event["type"].toString();
  if (type == "user_start")
    messages << QVariantMap{{"role", "user"},
                            {"text", event["text"].toString().right(131072)},
                            {"timestamp", event["timestamp"]}};
  else if (type == "assistant_start") {
    auto last = messages.isEmpty() ? QVariantMap{} : messages.last().toMap();
    if (last["role"] != "assistant" || !last["text"].toString().isEmpty() ||
        !last["thinking"].toString().isEmpty() ||
        !last["tools"].toList().isEmpty())
      messages << QVariantMap{{"role", "assistant"},
                              {"text", ""},
                              {"timestamp", event["timestamp"]}};
  } else if (QStringList{"delta", "thinking_delta", "message", "tool"}.contains(
                 type)) {
    if (messages.isEmpty() || messages.last().toMap()["role"] != "assistant")
      messages << QVariantMap{{"role", "assistant"}, {"text", ""}};
    auto last = messages.last().toMap();
    if (type == "delta" || type == "thinking_delta") {
      const auto field = type == "delta" ? "text" : "thinking";
      auto value = last[field].toString() + event["text"].toString();
      truncated |= value.size() > 131072;
      last[field] = value.right(131072);
    } else if (type == "message") {
      for (auto field :
           {"text", "thinking", "error", "stopReason", "timestamp"})
        if (event.contains(field))
          last[field] = event[field];
      for (auto field : {"text", "thinking"}) {
        auto text = last[field].toString();
        truncated |= text.size() > 131072;
        last[field] = text.right(131072);
      }
    } else {
      auto tools = last["tools"].toList();
      int index = -1;
      for (int i = 0; i < tools.size(); ++i)
        if (tools[i].toMap()["id"] == event["id"])
          index = i;
      auto tool =
          index < 0
              ? U::toolView(event["id"].toString(), event["name"].toString(),
                            event["args"].toMap(), owner->workspace)
              : tools[index].toMap();
      tool["state"] = event["state"];
      if (event.contains("result"))
        tool["result"] = event["result"].toString().left(8192);
      if (index < 0)
        tools << tool;
      else
        tools[index] = tool;
      truncated |= tools.size() > 32;
      last["tools"] = tools.mid(qMax(0, tools.size() - 32));
    }
    messages.last() = last;
  }
  if (!stream.isActive())
    stream.start();
}
void Subagents::publish() {
  if (messages.size() > 200) {
    messages = messages.mid(messages.size() - 200);
    truncated = true;
  }
  qsizetype weight = 0;
  for (int i = messages.size() - 1; i >= 0; --i) {
    auto m = messages[i].toMap();
    weight += m["text"].toString().size() + m["thinking"].toString().size();
    for (auto t : m["tools"].toList())
      weight += t.toMap()["result"].toString().size() + 16384;
    if (weight > 1024 * 1024 && i < messages.size() - 1) {
      messages = messages.mid(i + 1);
      truncated = true;
      break;
    }
  }
  model.update(messages);
  emit contextChanged();
}
void Subagents::loadFile(QString path) {
  const auto token = ++generation;
  loading = true;
  emit contextChanged();
  auto watcher = new QFutureWatcher<QVariantMap>(this);
  connect(watcher, &QFutureWatcher<QVariantMap>::finished, this,
          [this, watcher, token] {
            auto result = watcher->result();
            watcher->deleteLater();
            if (token != generation)
              return;
            loading = false;
            error = result["error"].toString();
            truncated = result["truncated"].toBool();
            messages = result["messages"].toList();
            publish();
          });
  watcher->setFuture(QtConcurrent::run([path] {
    if (!QFileInfo(path).isFile())
      return QVariantMap{{"error", "子代理的会话文件已不存在"}};
    auto rows = U::preview(path, 4 * 1024 * 1024).first;
    return QVariantMap{{"messages", rows},
                       {"truncated", QFileInfo(path).size() > 4 * 1024 * 1024}};
  }));
}
void Subagents::close() {
  closeView();
  deadline.stop();
  if (save.isActive()) {
    save.stop();
    b->store.save();
  }
}
