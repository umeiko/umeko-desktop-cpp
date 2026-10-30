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
  QString requestFor(QString id, QVariantMap value);
  void stop(QString id);
  void observe(QString key, QVariantMap event);
  Q_INVOKABLE void respondApproval(bool allowed);
signals:
  void changed();

private:
  struct Request {
    QString widget, id, operation;
    std::shared_ptr<DesktopPlugins::Worker> worker;
    QVariantMap session;
    QVariant arguments;
  };
  using Task = std::shared_ptr<Request>;
  DesktopPlugins *m;
  Backend *b;
  QVariantMap approval;
  Task pending;
  QHash<QString, Task> requests, jobs;
  QStringList jobOrder;
  struct Activity {
    std::shared_ptr<DesktopPlugins::Worker> worker;
    QVariantList events;
    int dropped = 0;
  };
  QHash<QString, Activity> activity;
  QTimer activityTimer;
  void flushActivity();
  void owner(Task request) const;
  void reply(Task request, QVariantMap value);
  void begin(Task request);
  void execute(Task request);
};
