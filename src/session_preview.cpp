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
  const auto job = pending;
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
  watcher->setFuture(QtConcurrent::run([job] {
    auto result = job;
    const auto saved = job["saved"].toMap();
    const auto file = saved["file"].toString();
    if (!file.isEmpty()) {
      QFile probe(file);
      if (!probe.open(QIODevice::ReadOnly)) {
        result["error"] = true;
        return result;
      }
      probe.close();
      const auto history = U::preview(file);
      result["messages"] = history.first;
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
    s->extensionModes = b->metadata(id)["extensionModes"].toMap();
    s->blockedTools = b->metadata(id)["blockedTools"].toStringList();
    s->file = saved["file"].toString();
    s->title = saved["title"].toString();
    s->model = saved["model"].toString();
    s->provider = saved["provider"].toString();
    s->messages = prepared["messages"].toList();
    s->tree = prepared["tree"].toList();
  }
  cancel();
  if (readOnly) b->activate(b->findSession(id));
  else b->openSession(id);
  return true;
}
