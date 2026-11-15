#pragma once
#include "common.hpp"
#include <memory>
class DesktopPet;
class DesktopPlugins;
struct Session;

// Application identity and speech limits live outside model-controlled settings.
class PetAssistant : public QObject {
  Q_OBJECT
public:
  explicit PetAssistant(DesktopPet *pet, DesktopPlugins *manager);
  void scheduleSync();
  void reconcile();
  bool enabled() const;
  bool owns(std::shared_ptr<Session> record) const;
  QString persona() const;
  QString bindingFor(std::shared_ptr<Session> record) const;
  QString prompt(std::shared_ptr<Session> record) const;
  QVariantMap status() const;
  QVariantMap send(std::shared_ptr<Session> record, QVariantMap args);
  void completed(std::shared_ptr<Session> record, double elapsed, QString state);
  void flush();
  void close();
private:
  friend class NativeUiTests;
  DesktopPet *pet;
  DesktopPlugins *m;
  QString identifier, binding, signature, error;
  QElapsedTimer clock;
  QTimer sync, timer;
  QStringList seen;
  QVariantList pending;
  QList<qint64> spoken;
  qint64 nextBrief = 0;
  int overflow = 0;
};
