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
  QVariantList requests, creationRequests;
  QHash<QString, std::shared_ptr<Session>> records;
  QVariantMap outbox;
  mutable QHash<QString,qint64> busySince;
  bool active = false;
  bool allowAutomaticCreation = false;
  int revision = 0;
  QTimer timer, saveTimer;
  QList<QPointer<QProcess>> retiring;
  void changed(bool persist = true);
  void save();
  void tick();
  void halt(QString id);
  QVariantMap bridgeAccess(QStringList keys) const;
  bool allowed(std::shared_ptr<Session> record, QString source = {}) const;
  void connectCreationSource(QVariantMap row);
  bool connected(std::shared_ptr<Session> source, std::shared_ptr<Session> target) const;
  bool canRoute(QString source, QString target);
  QVariantMap activity(QString id) const;
  QVariantList pendingQueue(QString id) const;
  QString identity(std::shared_ptr<Session> record) const;
  std::shared_ptr<Session> ensure(QString id);
  std::shared_ptr<Session> record(QString target);
  QVariantMap overview(std::shared_ptr<Session> caller = {}) const;
  QVariantMap selfInfo(std::shared_ptr<Session> caller) const;
  QPair<QString,int> automaticName(QString character, QString id);
  QVariantMap requestCreation(QVariantMap args, std::shared_ptr<Session> caller);
  QVariantMap creationResult(QVariantMap row) const;
  bool ownsCreation(QVariantMap row, std::shared_ptr<Session> caller) const;
  int creationIndex(QString id) const;
  void notifyCreation(QVariantMap row);
  QVariantMap snapshot(QString selected) const;
  QVariantMap handle(QString method, QVariantMap args, bool agent);
  void finish(QString id, QString status, QString result);
  void enqueue(QString target, QVariantMap request, QString message, bool wake);
  int requestIndex(QString id) const;
};
