#pragma once
#include <QMenu>
#include <QObject>
#include <QPointer>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWindow>

// Only the main window closes into the tray. Explicit quit/update still exits.
class TrayController : public QObject {
  Q_OBJECT
public:
  explicit TrayController(QWindow *window, const QIcon &icon, QObject *parent = nullptr);
  ~TrayController() override;
  Q_INVOKABLE void restore();
  Q_INVOKABLE void requestQuit();
  void setLanguage(const QString &language);
  QSystemTrayIcon *systemTray() { return &tray; }
signals:
  void quitRequested();
  void notificationRequested(QString message);
protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  virtual bool trayAvailable() const;
private:
  void minimizeToTray();
  void markQuitting();
  QPointer<QWindow> window;
  QSystemTrayIcon tray;
  QMenu menu;
  QTimer availability;
  QWindow::Visibility previousVisibility = QWindow::Windowed;
  bool quitting = false;
  bool english = false;
};
