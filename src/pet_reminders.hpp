#pragma once
#include "common.hpp"
#include <optional>

// Persist occurrences, agent deliveries and unread user alerts independently.
class PetReminders : public QObject {
  Q_OBJECT
public:
  explicit PetReminders(QString directory, QObject *parent = nullptr,
      std::function<QDateTime()> clock = [] { return QDateTime::currentDateTimeUtc(); });
  std::function<bool()> visible = [] { return false; };
  std::function<bool()> notifications = [] { return true; };
  std::function<QString()> assistant = [] { return QString(); };
  QVariantList pendingNotifications(int limit=16, QString agent={}) const {
    QVariantList result;
    for(const auto &value:outbox) {
      const auto target=value.toMap()["agentId"].toString();
      if(agent.isEmpty()||target.isEmpty()||target==agent)result<<value;
      if(result.size()>=limit)break;
    }
    return result;
  }
  QVariantList unreadAlerts() const { return alerts; }
  void acknowledgeNotifications(QStringList ids);
  void markViewed(QString id = {});
  QVariantMap handle(QString method, QVariantMap args);
  QVariantMap summary() const;
  QVariantMap timeInfo() const;
  QVariantMap resolveLocalTime(QVariantMap args) const;
  void start();
  void stop();
  void check();
signals:
  void changed();
  void due(QVariantList items);
private:
  QString directory, error;
  QVariantList items, outbox, alerts;
  QTimer timer;
  std::function<QDateTime()> clock;
  bool running = false, invalid = false, paused = false, checking = false;
  void load();
  void commit(QVariantList next, std::optional<QVariantList> notifications=std::nullopt, std::optional<QVariantList> unread=std::nullopt);
  QVariantList ordered() const;
  QVariantMap view(QVariantMap item) const;
};
