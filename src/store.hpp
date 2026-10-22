#pragma once
#include "common.hpp"

class Store {
public:
  explicit Store(QString directory = {});
  QString directory;
  QVariantMap data;
  QString error;
  bool save();
  QByteArray secret(QString key);
  bool setSecret(QString key, QByteArray value);
  QVariantList sessions() const;
  bool saveSessions(QVariantList rows);
  static QVariantMap defaults();
  static bool migrateProfile(QString previous, QString target);

private:
  QHash<QString, QByteArray> cached;
  QString keyPrefix;
  bool writable = true;
};
