#include "notifications.hpp"
#include "notify.h"
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QStandardPaths>
#include <QUuid>
#include <QPixmap>
#ifdef QT_DBUS_LIB
#include <QtDBus>
#endif

TaskNotifications::TaskNotifications(QWindow *mainWindow, QSystemTrayIcon *systemTray, QObject *parent)
    : QObject(parent), window(mainWindow), tray(systemTray), timer(this) {
  const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/notifications";
  if (QDir().mkpath(cache)) {
    icon = cache + "/yanami.png";
    if (!tray->icon().pixmap(96, 96).save(icon)) icon.clear();
  }
  nativeReady = umeko_notify_init("io.github.umeiko.umeko-desktop-cpp", "Umeko-Desktop", icon.toUtf8().constData());
  timer.setInterval(150); connect(&timer, &QTimer::timeout, this, &TaskNotifications::poll); timer.start();
  connect(tray, &QSystemTrayIcon::messageClicked, this, [this] {
    const auto id = fallback; fallback.clear(); activate(id);
  });
  window->installEventFilter(this);
  connect(window, &QObject::destroyed, this, &TaskNotifications::close);
  connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &TaskNotifications::close);
}
bool TaskNotifications::eventFilter(QObject *watched, QEvent *event) {
  if (watched == window && event->type() == QEvent::Hide) fallback.clear();
  return false;
}
void TaskNotifications::close() { closing = true; timer.stop(); pending.clear(); recent.clear(); dbusIds.clear(); }
void TaskNotifications::taskFinished(QString session, QString title, bool success) {
  if (closing || !window || session.isEmpty()) return;
  if (window->isVisible() && window->visibility() != QWindow::Minimized) return;
  const auto heading = QString("Umeko · ") + (english ? (success ? "Task complete" : "Task needs attention") : (success ? "任务完成" : "任务未完成"));
  auto label = title.simplified().left(160);
  if (label.isEmpty()) label = english ? "Conversation" : "会话";
  const auto body = label + "\n" + (english ? "Click to view the result." : "点击查看结果。");
  const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  pending.insert(id, session);
  recent.enqueue(id);
  while (recent.size() > 128) pending.remove(recent.dequeue());
  const auto result = nativeSend(id, heading, body);
  if (result == 0 && !sendDBus(id, heading, body)) {
    fallback = id; tray->showMessage(heading, body, tray->icon(), 8000);
  } else if (result < 0) {pending.remove(id); return;}
  emit posted(session, heading, body);
}
int TaskNotifications::nativeSend(QString id, QString title, QString body) {
  return nativeReady ? umeko_notify_show(id.toUtf8().constData(), title.toUtf8().constData(), body.toUtf8().constData()) : 0;
}
bool TaskNotifications::sendDBus(QString id, QString title, QString body) {
#ifdef QT_DBUS_LIB
  const QString service = "org.freedesktop.Notifications", path = "/org/freedesktop/Notifications", interface = service;
  auto bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) return false;
  QDBusInterface remote(service, path, interface, bus);
  if (!remote.isValid()) return false;
  if (!dbusConnected) {
    if (!bus.connect(service, path, interface, "ActionInvoked", this, SLOT(actionInvoked(uint,QString)))) return false;
    bus.connect(service, path, interface, "NotificationClosed", this, SLOT(notificationClosed(uint,uint)));
    dbusConnected = true;
  }
  const QVariantMap hints{{"sound-name", "complete"}, {"suppress-sound", false}, {"transient", false}};
  const auto actions = QStringList{"default", english ? "Open" : "查看"};
  auto watcher = new QDBusPendingCallWatcher(remote.asyncCall("Notify", QStringLiteral("Umeko"), uint(0), icon, title, body, actions, hints, -1), this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this, [this,watcher,id] {
    QDBusPendingReply<uint> reply = *watcher;
    if (reply.isError()) pending.remove(id);
    else if (!closing && pending.contains(id)) {
      for (auto it = dbusIds.begin(); it != dbusIds.end();)
        if (!pending.contains(it.value())) it = dbusIds.erase(it); else ++it;
      dbusIds[reply.value()] = id;
    }
    watcher->deleteLater();
  });
  return true;
#else
  Q_UNUSED(id); Q_UNUSED(title); Q_UNUSED(body); return false;
#endif
}
void TaskNotifications::actionInvoked(uint id, QString action) {
  const auto token = dbusIds.take(id);
  if (action == "default") activate(token);
}
void TaskNotifications::notificationClosed(uint id, uint) { pending.remove(dbusIds.take(id)); }
void TaskNotifications::activate(QString id) {
  const auto session = pending.take(id);
  if (!session.isEmpty() && !closing) emit activated(session);
}
void TaskNotifications::poll() {
  for (int i = 0; i < 128; ++i) {
    const char *raw = umeko_notify_poll(); if (!raw) return;
    const auto value = QString::fromUtf8(raw); const auto at = value.indexOf(':');
    const auto id = value.mid(at + 1);
    if (value.left(at) == "clicked") activate(id); else pending.remove(id);
  }
}
