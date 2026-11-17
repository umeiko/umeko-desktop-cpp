#pragma once
#include "common.hpp"
#include <memory>
class DesktopPet;
class DesktopPlugins;
struct Session;

// Application identity and speech validation live outside model-controlled settings.
class PetAssistant : public QObject {
  Q_OBJECT
public:
  explicit PetAssistant(DesktopPet *pet, DesktopPlugins *manager);
  void scheduleSync();
  void reconcile();
  bool enabled() const;
  bool owns(std::shared_ptr<Session> record) const;
  QString persona() const;
  QString reminderAgentId() const;
  bool managesReminders(std::shared_ptr<Session> record) const;
  QString reminderBindingFor(std::shared_ptr<Session> record) const;
  QVariantMap reminder(std::shared_ptr<Session> record,QVariantMap args);
  void scheduleReminders();
  void flushReminders();
  void observeReminders(QString key,QVariantMap event);
  QString bindingFor(std::shared_ptr<Session> record) const;
  QString prompt(std::shared_ptr<Session> record) const;
  QVariantMap status() const;
  bool allowsTimer(std::shared_ptr<Session> record, QString operation, QVariantMap args) const;
  QVariantMap send(std::shared_ptr<Session> record, QVariantMap args);
  void completed(std::shared_ptr<Session> record, double elapsed, QString state);
  void flush();
  void close();
private:
  friend class NativeUiTests;
  DesktopPet *pet;
  DesktopPlugins *m;
  QString identifier, binding, signature, error, reminderBinding;
  QElapsedTimer clock;
  QTimer sync, timer, reminderTimer;
  QStringList seen;
  QVariantList pending;
  qint64 nextBrief = 0;
  QVariantMap reminderFlight;
  qint64 reminderRetryAt = 0;
  int overflow = 0;
};
