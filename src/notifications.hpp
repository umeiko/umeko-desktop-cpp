#pragma once
#include <QObject>
#include <QPointer>
#include <QHash>
#include <QQueue>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWindow>

class TaskNotifications : public QObject {
  Q_OBJECT
  friend class TrayTests;
public:
  bool isNativeReady() const { return nativeReady; }
  TaskNotifications(QWindow *window, QSystemTrayIcon *tray, QObject *parent = nullptr);
  void setLanguage(QString language) { english = language == "en"; }
public slots:
  void taskFinished(QString session, QString title, bool success, bool longRunning = false);
  void close();
signals:
  void activated(QString session);
  void posted(QString session, QString title, QString body);
protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  virtual int nativeSend(QString id, QString title, QString body);
  virtual bool sendDBus(QString id, QString title, QString body);
private slots:
  void poll();
  void actionInvoked(uint id, QString action);
  void notificationClosed(uint id, uint reason);
private:
  void activate(QString id);
  QPointer<QWindow> window;
  QSystemTrayIcon *tray;
  QTimer timer;
  QHash<QString, QString> pending;
  QQueue<QString> recent;
  QHash<uint, QString> dbusIds;
  QString icon, fallback;
  bool english = false, closing = false, nativeReady = false, dbusConnected = false;
};
