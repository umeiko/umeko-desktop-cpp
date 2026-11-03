#pragma once
#include "common.hpp"

// Local one-shot reminders. Persist a claim before requesting a bubble, so
// restarts, visibility changes and failed writes never replay an occurrence.
class PetReminders : public QObject {
  Q_OBJECT
public:
  explicit PetReminders(QString directory, QObject *parent = nullptr,
      std::function<QDateTime()> clock = [] { return QDateTime::currentDateTimeUtc(); });
  std::function<bool()> visible = [] { return false; };
  std::function<bool()> notifications = [] { return true; };
  QVariantMap handle(QString method, QVariantMap args);
  QVariantMap summary() const;
  QVariantMap timeInfo() const;
  void start();
  void stop();
  void check();
signals:
  void changed();
  void due(QVariantList items);
private:
  QString directory, error;
  QVariantList items;
  QTimer timer;
  std::function<QDateTime()> clock;
  bool running = false, invalid = false, paused = false, checking = false;
  void load();
  void commit(QVariantList next);
  QVariantList ordered() const;
  QVariantMap view(QVariantMap item) const;
};
