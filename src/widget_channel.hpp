#pragma once
#include "common.hpp"
class DesktopPlugins;
struct Session;
class WidgetChannel : public QObject {
  Q_OBJECT
public:
  explicit WidgetChannel(DesktopPlugins *manager);
  QVariantMap execute(QString operation, QVariantMap args, QString callerKey);
  void notify(QString id, QVariantMap args);
  void observe(QString key, QVariantMap event);
  void stopNotifications(QString id);

private:
  friend class DesktopPlugins;
  struct State {
    QString token, key;
    QVariantMap snapshot;
    QVariantList messages;
    qint64 sequence = 0;
  };
  DesktopPlugins *m;
  QHash<QString, std::shared_ptr<State>> states;
  QMap<QString, QVariantMap> calls;
  QStringList callOrder;
  QHash<QString, QString> management;
  QHash<QString, QList<qint64>> rates;
  std::shared_ptr<State> current;
  QVariantMap snapshot(std::shared_ptr<Session> record) const;
  std::shared_ptr<State> remember(std::shared_ptr<Session> record);
  QString logPath(std::shared_ptr<State> state) const;
  void save(std::shared_ptr<State> state);
  void bind();
  void flush();
  void receipt(std::shared_ptr<State> state, int index, QString status,
               QString error = {});
  void reject(QString widget, QString id, QString request, QString error);
  QVariantList list(bool active, std::shared_ptr<Session> caller) const;
  QString document(QString id) const;
  QVariantMap require(QString id, bool active = false) const;
};
