#pragma once
#include "desktop_plugins.hpp"

class WidgetHost : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap capabilities READ capabilities CONSTANT)
  Q_PROPERTY(QVariantMap pendingApproval READ pendingApproval NOTIFY changed)
public:
  explicit WidgetHost(DesktopPlugins *manager);
  QVariantMap capabilities() const;
  QVariantMap pendingApproval() const { return approval; }
  QString requestFor(QString id, QVariantMap value, bool page = false);
  void stop(QString id);
  void observe(QString key, QVariantMap event);
  bool prepareSession(std::shared_ptr<struct Session> record);
  bool captureSessionApproval(QString key, QVariantMap event);
  bool workspaceLocked(QString workspace) const;
  bool releaseCheckpoint(QString id, QString token, std::shared_ptr<DesktopPlugins::Worker> worker);
  Q_INVOKABLE void respondApproval(bool allowed);
  Q_INVOKABLE void respondPiDialog(QString value);
signals:
  void changed();

private:
  struct Request {
    QString widget, id, operation;
    std::shared_ptr<DesktopPlugins::Worker> worker;
    QVariantMap session;
    QVariant arguments;
    bool page = false;
    int backgroundPending = 0;
    int attempts = 0;
    QString backgroundError;
  };
  using Task = std::shared_ptr<Request>;
  DesktopPlugins *m;
  Backend *b;
  QVariantMap approval;
  Task pending;
  QHash<QString, Task> requests, jobs;
  QHash<QString, QPair<Task, QString>> catalogRequests;
  QHash<QString, QPair<Task, QString>> promptRequests;
  void systemPrompt(Task request);
  QStringList jobOrder;
  struct Activity {
    std::shared_ptr<DesktopPlugins::Worker> worker;
    QVariantList events;
    int dropped = 0;
  };
  QHash<QString, Activity> activity;
  struct CheckpointLock {
    QString widget;
    std::shared_ptr<DesktopPlugins::Worker> worker;
    QString workspace;
  };
  QHash<QString, CheckpointLock> checkpointLocks;
  QTimer activityTimer;
  void flushActivity();
  void owner(Task request) const;
  void reply(Task request, QVariantMap value);
  void begin(Task request);
  void execute(Task request);
  void queryCheckpoint(Task request);
  void finishCheckpoint(Task request);
  void syncCheckpointLocks();
  void market(Task request);
  struct PreparedMarket {
    Task owner;
    QVariantMap package;
  };
  QHash<QString, PreparedMarket> marketPrepared;
  QHash<QString, PreparedMarket> marketImports;
  QHash<QString, Task> marketImportRequests;
  QVariantList marketRows;
  bool marketBusy = false;
  void discardMarket(QString token);
  struct SessionLease {
    QString plugin;
    std::shared_ptr<DesktopPlugins::Worker> worker;
    std::shared_ptr<struct Session> record;
    bool subscribed=false, handler=false;
  };
  struct PluginRun {
    std::shared_ptr<struct Session> record;
    QString id, requestId, content, disposition, status="queued", error;
    QStringList messages;
    QList<Task> waiters;
    bool cancelling=false, truncated=false;
  };
  struct SessionApproval {
    std::shared_ptr<struct Session> record;
    QVariantMap event;
    QSet<QString> handlers;
    qint64 expires=0, handlerExpires=0;
    bool desktop=false;
  };
  QHash<QString,SessionLease> sessionLeases;
  QHash<QString,PluginRun> sessionRuns;
  QStringList sessionRunOrder;
  QHash<QString,QStringList> sessionQueues;
  QHash<QString,QString> sessionActive, sessionAcquiring;
  QHash<QString,QList<Task>> sessionAcquires;
  QHash<QString,SessionApproval> sessionApprovals;
  QString visibleSessionApproval;
  void sessionService(Task request);
  void sessionObserve(QString key,QVariantMap event);
  void stopSessions(QString plugin);
  void sessionPolicy(std::shared_ptr<struct Session> record);
  void sessionLease(Task request,std::shared_ptr<struct Session> record,bool created);
  void nextSessionRun(std::shared_ptr<struct Session> record);
  void finishSessionRun(QString run,QString status,QString error={});
  void emitSessionEvent(std::shared_ptr<struct Session> record,QString type,QString run={},QVariantMap values={});
  void fallbackSessionApproval(QString id);
  void showSessionApproval();
  bool answerSessionApproval(QString id,QVariant value);
};
