#pragma once
#include "common.hpp"
#include <memory>
class Backend;
struct Session;

class SessionFiles : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QVariantMap failure READ failure NOTIFY failureChanged)
public:
  explicit SessionFiles(Backend *backend);
  bool busy() const { return m_busy; }
  QVariantMap failure() const { return m_failure; }
  QSet<QString> pending, deleted;
  Q_INVOKABLE QVariantMap projectSummary(QString id) const;
  Q_INVOKABLE void importFiles(QString urls, QString project = {});
  Q_INVOKABLE void deleteSession(QString id);
  Q_INVOKABLE void deleteProject(QString id);
  Q_INVOKABLE void dismissFailure();
signals:
  void changed();
  void failureChanged();
private:
  Backend *b;
  bool m_busy = false;
  QVariantMap m_failure;
  QString workspace(QString path) const;
  QVariantList projectSessions(QString path) const;
  void fail(QString title, QVariantList errors);
  void execute(QVariantMap payload, QVariantMap context, QList<std::shared_ptr<Session>> records = {});
  void remove(QVariantList sessions, QString project);
};
