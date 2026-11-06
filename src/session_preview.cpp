#include "backend.hpp"
#include <QtConcurrent>

SessionPreview::SessionPreview(Backend *backend)
    : QObject(backend), b(backend) {}
void SessionPreview::cancel() {
  request = 0;
  pending.clear();
  prepared.clear();
}
void SessionPreview::prepare(QString id, int token) {
  cancel();
  request = token;
  QVariantMap saved;
  for (auto row : b->sessionIndex)
    if (row.toMap()["id"] == id)
      saved = row.toMap();
  if (saved.isEmpty()) {
    emit ready(token, false);
    return;
  }
  QVariantMap job{{"token", token}, {"saved", saved}};
  if (b->findSession(id)) {
    prepared = job;
    emit ready(token, true);
    return;
  }
  pending = job;
  start();
}
void SessionPreview::start() {
  if (reading || pending.isEmpty() || b->closing)
    return;
  reading = true;
  auto job = pending;
  if(job["saved"].toMap()["backend"]=="dsh"){b->flushDsh();}
  auto writer=b->dshWriter;
  pending.clear();
  auto watcher = new QFutureWatcher<QVariantMap>(this);
  connect(watcher, &QFutureWatcher<QVariantMap>::finished, this,
          [this, watcher] {
            const auto result = watcher->result();
            watcher->deleteLater();
            reading = false;
            if (b->closing)
              return;
            if (result["token"].toInt() == request) {
              const bool ok = !result["error"].toBool();
              if (ok)
                prepared = result;
              else
                emit b->toast("聊天记录暂时无法读取，请稍后重试");
              emit ready(request, ok);
            }
            start();
          });
  // The worker owns only value snapshots, never the backend or QML objects.
  // At most one read runs; rapid clicks retain only the latest pending job.
  watcher->setFuture(QtConcurrent::run([job,writer]() mutable {
    auto result = job;
    const auto saved = job["saved"].toMap();
    const auto file = saved["file"].toString();
    if(saved["backend"]=="dsh") {
      writer.waitForFinished();QFile f(file);
      if(!f.open(QIODevice::ReadOnly) || f.size()>128*1024*1024){result["error"]=true;return result;}
      QJsonParseError error;const auto doc=QJsonDocument::fromJson(f.readAll(),&error);
      if(error.error!=QJsonParseError::NoError || !doc.isObject()){result["error"]=true;return result;}
      const auto value=doc.object().toVariantMap();result["dsh"]=value;result["messages"]=value["messages"];result["displaySnapshot"]=ChatModel::project(value["messages"].toList());return result;
    }
    if (!file.isEmpty()) {
      QFile probe(file);
      if (!probe.open(QIODevice::ReadOnly)) {
        result["error"] = true;
        return result;
      }
      probe.close();
      const auto history = U::preview(file);
      result["messages"] = history.first;
      result["displaySnapshot"] = ChatModel::project(history.first);
      result["tree"] = history.second;
    }
    return result;
  }));
}
bool SessionPreview::activate(int token, bool readOnly) {
  if (prepared.isEmpty() || request != token ||
      prepared["token"].toInt() != token)
    return false;
  const auto id = prepared["saved"].toMap()["id"].toString();
  QVariantMap saved;
  for (auto row : b->sessionIndex)
    if (row.toMap()["id"] == id)
      saved = row.toMap();
  if (saved.isEmpty())
    return false;
  if (!b->findSession(id)) {
    auto s = b->fresh(saved["workspace"].toString());
    s->id = id;
    if(saved["backend"]=="dsh") {
      s->backend="dsh";s->acpSessionId=saved["acpSessionId"].toString();s->dshHome=saved["dshHome"].toString();s->dshOwnFixed=true;s->dshOwn=saved["dshOwnConfig"].toBool();
      auto data=prepared["dsh"].toMap();s->dshOptions=data["configOptions"].toList();s->dshInterrupted=data["interruptionPending"].toBool();s->usage=data["contextUsage"].toMap();
    }
    s->extensionModes = b->metadata(id)["extensionModes"].toMap();
    s->blockedTools = b->metadata(id)["blockedTools"].toStringList();
    s->piCapabilities = b->metadata(id)["piCapabilities"].toMap();
    s->skillModes = b->metadata(id)["skillModes"].toMap();
    s->pluginOwner = b->metadata(id)["pluginSession"].toMap();
    s->file = saved["file"].toString();
    s->title = saved["title"].toString();
    s->model = saved["model"].toString();
    s->provider = saved["provider"].toString();
    s->reasoningEffort = b->metadata(id).value("reasoningEffort", s->reasoningEffort).toString();
    s->messages = prepared["messages"].toList();
    s->displaySnapshot = prepared["displaySnapshot"].toList();
    const auto viewport=b->metadata(id)["viewport"].toMap();
    s->scrollIndex=viewport.value("index",-1).toInt();s->scrollY=viewport.value("offset",0).toDouble();s->followTail=viewport.value("follow",true).toBool();
    s->tree = prepared["tree"].toList();
  }
  cancel();
  if (readOnly) b->activate(b->findSession(id));
  else b->openSession(id);
  return true;
}
