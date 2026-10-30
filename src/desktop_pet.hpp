#pragma once
#include "common.hpp"
class DesktopPlugins;
class DesktopPet : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap config READ config NOTIFY changed)
  Q_PROPERTY(bool visible READ visible NOTIFY changed)
  Q_PROPERTY(QVariantList appearances READ appearances NOTIFY changed)
  Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
public:
  explicit DesktopPet(DesktopPlugins *manager);
  QVariantMap config() const { return options; }
  bool visible() const {
    return running && options["visible"].toBool() && !closed;
  }
  QVariantList appearances() const;
  QVariantMap state() const;
  void start();
  void stop();
  void close();
  void sample();
  Q_INVOKABLE QVariantMap ui(QString method, QVariantMap arguments);
  Q_INVOKABLE void interact(QString kind);
  Q_INVOKABLE void savePosition(int x, int y);
  Q_INVOKABLE QVariantMap position() const;
  Q_INVOKABLE QVariantMap screenBounds(int x, int y) const;
signals:
  void changed();
  void reaction(QString text);
  void restoreRequested();
  void settingsRequested();
  void resetPositionRequested();
  void imported(QString token, QVariantMap result);

private:
  DesktopPlugins *m;
  QTimer timer;
  QElapsedTimer clock;
  QVariantMap options, packs, activity;
  QHash<QString, bool> observed;
  QString action = "idle", target = "idle", interaction, terminal, lastKind,
          lastError;
  qint64 since = 0, until = 0, terminalUntil = 0, lastInteraction = -10000;
  bool running = false, closed = false, importing = false;
  QString directory, packDirectory;
  void loadPacks();
  QVariantMap configure(QVariantMap patch, bool persist = true);
  QVariantMap handle(QString method, QVariantMap args);
  QVariantMap status() const;
  bool step();
  void speak(QString kind);
};
