#pragma once
#include "common.hpp"
#include <memory>
class Backend;
class DesktopPlugins;
struct Session;
// Official application Widget service. Model callers never receive its UI API.
class GlobalAgents : public QObject {
  Q_OBJECT
public:
  explicit GlobalAgents(DesktopPlugins *manager);
  void start();
  void stop();
  void close();
  QVariantMap ui(QString method, QVariantMap args, bool agent = false);
  QVariantMap execute(QString operation, QVariantMap args, QString callerKey);
  void observe(QString key, QVariantMap event);
  QVariantMap runtimeConfig(std::shared_ptr<Session> record);
private:
  friend class NativeUiTests;
  DesktopPlugins *m;
  Backend *b;
  QString root, registry, error;
  QVariantMap agents, bridges;
  QVariantList requests;
  QHash<QString, std::shared_ptr<Session>> records;
  QVariantMap outbox;
  bool active = false;
  int revision = 0;
  QTimer timer, saveTimer;
  QList<QPointer<QProcess>> retiring;
  void changed(bool persist = true);
  void save();
  void tick();
  void halt(QString id);
  bool allowed(std::shared_ptr<Session> record) const;
  QString identity(std::shared_ptr<Session> record) const;
  std::shared_ptr<Session> ensure(QString id);
  std::shared_ptr<Session> record(QString target);
  QVariantMap overview() const;
  QVariantMap snapshot(QString selected) const;
  QVariantMap handle(QString method, QVariantMap args, bool agent);
  void finish(QString id, QString status, QString result);
  void enqueue(QString target, QVariantMap request, QString message, bool wake);
  int requestIndex(QString id) const;
};
