#include "tray.hpp"
#include <QAction>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QGuiApplication>

TrayController::TrayController(QWindow *mainWindow, const QIcon &icon, QObject *parent)
    : QObject(parent), window(mainWindow), tray(icon, this), availability(this) {
  tray.setObjectName("umekoTrayIcon");
  tray.setToolTip("Umeko");
  menu.setObjectName("umekoTrayMenu");
  auto open = menu.addAction("打开 Umeko");
  open->setObjectName("trayShowAction");
  menu.addSeparator();
  auto quit = menu.addAction("退出");
  quit->setObjectName("trayQuitAction");
  quit->setMenuRole(QAction::QuitRole);
  tray.setContextMenu(&menu);
  connect(open, &QAction::triggered, this, &TrayController::restore);
  connect(quit, &QAction::triggered, this, &TrayController::requestQuit);
  connect(&tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) restore();
  });
  connect(&tray, &QSystemTrayIcon::messageClicked, this, &TrayController::restore);
  connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &TrayController::markQuitting);
  QGuiApplication::setQuitOnLastWindowClosed(false);
  window->installEventFilter(this);
  QCoreApplication::instance()->installEventFilter(this);
  // A desktop can start its tray service after the application has launched.
  connect(&availability, &QTimer::timeout, this, [this] {
    if (trayAvailable() && !tray.icon().isNull() && !quitting) {tray.show(); availability.stop();}
  });
  if (trayAvailable() && !tray.icon().isNull()) tray.show();
  else availability.start(2000);
}
TrayController::~TrayController() {
  if (window) window->removeEventFilter(this);
  if (QCoreApplication::instance()) QCoreApplication::instance()->removeEventFilter(this);
  tray.hide();
}
bool TrayController::trayAvailable() const { return QSystemTrayIcon::isSystemTrayAvailable(); }
void TrayController::setLanguage(const QString &language) {
  english = language == "en";
  menu.findChild<QAction *>("trayShowAction")->setText(english ? "Open Umeko" : "打开 Umeko");
  menu.findChild<QAction *>("trayQuitAction")->setText(english ? "Quit" : "退出");
}
bool TrayController::eventFilter(QObject *watched, QEvent *event) {
  if (watched == QCoreApplication::instance() && event->type() == QEvent::Quit) markQuitting();
  if (watched == window && event->type() == QEvent::Close && !quitting) {
    static_cast<QCloseEvent *>(event)->ignore();
    minimizeToTray();
    return true;
  }
  return QObject::eventFilter(watched, event);
}
void TrayController::minimizeToTray() {
  if (!window) return;
  if (window->visibility() != QWindow::Minimized && window->visibility() != QWindow::Hidden)
    previousVisibility = window->visibility();
  if (trayAvailable() && !tray.icon().isNull()) {
    tray.show();
    window->hide();
    const QString message = english ? "Minimized to the system tray. Agents keep running. Right-click the Yanami icon to quit." : "已最小化到系统托盘。Agent 继续运行，右键八奈见头像可退出。";
    emit notificationRequested(message);
    tray.showMessage("Umeko", message, tray.icon(), 4000);
  } else {
    // Keep the app reachable on desktops without a notification area.
    window->showMinimized();
    emit notificationRequested(english ? "System tray unavailable. Minimized to the taskbar; agents keep running." : "系统托盘不可用，已最小化到任务栏。Agent 继续运行。");
  }
}
void TrayController::restore() {
  if (!window || quitting) return;
  if (!window->isVisible() || window->visibility() == QWindow::Minimized) {
    if (previousVisibility == QWindow::Maximized) window->showMaximized();
    else if (previousVisibility == QWindow::FullScreen) window->showFullScreen();
    else window->showNormal();
  }
  window->raise(); window->requestActivate();
}
void TrayController::markQuitting() {
  quitting = true; availability.stop(); tray.hide();
}
void TrayController::requestQuit() {
  if (quitting) return;
  markQuitting();
  emit quitRequested();
}
