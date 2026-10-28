#include "tray.hpp"
#include "notifications.hpp"
#include "events.hpp"
#include <QAction>
#include <QApplication>
#include <QQuickWindow>
#include <QtTest>

class TestTray : public TrayController {
public:
  using TrayController::TrayController;
  bool available = false;
protected:
  bool trayAvailable() const override { return available; }
};
class TrayTests : public QObject {
  Q_OBJECT
private slots:
  void linuxDesktopAcceptsTheNativeNotification() {
#ifdef Q_OS_LINUX
    if (!qEnvironmentVariableIsSet("UMEKO_TEST_DESKTOP_NOTIFICATIONS")) QSKIP("Opt in on a real Linux desktop");
    QQuickWindow window;
    QPixmap pixel(32,32); pixel.fill(Qt::blue); QSystemTrayIcon tray{QIcon(pixel)};
    TaskNotifications notifications(&window,&tray);
    QSignalSpy opened(&notifications,&TaskNotifications::activated);
    notifications.taskFinished("desktop-fixture","系统通知测试（非真实任务）",true);
    QTRY_VERIFY_WITH_TIMEOUT(!notifications.dbusIds.isEmpty(),3000);
    notifications.actionInvoked(notifications.dbusIds.key(notifications.recent.last()),"default");
    QCOMPARE(opened.count(),1); QCOMPARE(opened.first()[0].toString(),QString("desktop-fixture"));
#else
    QSKIP("Linux desktop transport");
#endif
  }
  void completionNotificationsKeepIndependentSessionTargets() {
    class Notifications : public TaskNotifications {
    public:
      using TaskNotifications::TaskNotifications;
      QStringList ids, titles;
      int nativeSend(QString id, QString title, QString) override {ids << id; titles << title; return 1;}
      bool sendDBus(QString, QString, QString) override {return false;}
    };
    QQuickWindow window; window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QPixmap pixel(32,32); pixel.fill(Qt::blue); QSystemTrayIcon tray{QIcon(pixel)};
    Notifications notifications(&window,&tray);
    QSignalSpy opened(&notifications,&TaskNotifications::activated);
    notifications.taskFinished("visible","Visible task",true); QVERIFY(notifications.ids.isEmpty());
    window.showMinimized(); QTRY_COMPARE(window.visibility(),QWindow::Minimized);
    notifications.taskFinished("A","First task",true);
    window.hide(); notifications.taskFinished("B","Second task",true);
    QCOMPARE(notifications.ids.size(),2); QVERIFY(notifications.titles.first().contains("任务完成"));
    notifyEvent("clicked",notifications.ids[1].toStdString());
    notifyEvent("clicked",notifications.ids[0].toStdString());
    notifyEvent("clicked",notifications.ids[0].toStdString());
    QTRY_COMPARE(opened.size(),2);
    QCOMPARE(opened[0][0].toString(),QString("B")); QCOMPARE(opened[1][0].toString(),QString("A"));
    notifications.setLanguage("en"); notifications.taskFinished("C","Failed task",false);
    QVERIFY(notifications.titles.last().contains("needs attention"));
    window.showNormal(); QTRY_VERIFY(window.isVisible() && window.visibility() != QWindow::Minimized);
    notifications.taskFinished("visible-short","Short task",true,false); QCOMPARE(notifications.ids.size(),3);
    notifications.taskFinished("visible-long","Long task",true,true); QCOMPARE(notifications.ids.size(),4);
    notifyEvent("clicked",notifications.ids.last().toStdString());
    QTRY_COMPARE(opened.size(),3); QCOMPARE(opened.last()[0].toString(),QString("visible-long"));
    notifications.taskFinished("visible-error","Long failed task",false,true); QCOMPARE(notifications.ids.size(),5);
    QVERIFY(notifications.titles.last().contains("needs attention"));
    notifications.close(); notifications.taskFinished("exit","Ignore",true,true); QCOMPARE(notifications.ids.size(),5);
  }
  void closeKeepsBackgroundWorkAndRestores() {
    QQuickWindow window;
    window.resize(320,240); window.show();
    QPixmap pixel(32,32); pixel.fill(Qt::blue);
    TestTray tray(&window,QIcon(pixel)); tray.available = true;
    QSignalSpy notifications(&tray,&TrayController::notificationRequested);
    QSignalSpy quit(&tray,&TrayController::quitRequested);
    QTimer agent; int ticks = 0;
    connect(&agent,&QTimer::timeout,this,[&]{++ticks;}); agent.start(5);
    QVERIFY(!window.close());
    QVERIFY(!window.isVisible());
    QCOMPARE(quit.count(),0);
    QCOMPARE(notifications.count(),1);
    QCOMPARE(notifications.first().first().toString(),QString("我在这里继续执行任务！需要完全退出你可以右键点击任务栏中的图标~"));
    QTRY_VERIFY(ticks > 2);
    auto icon = tray.findChild<QSystemTrayIcon*>("umekoTrayIcon");
    QVERIFY(icon && !icon->icon().isNull());
    auto menu = icon->contextMenu(); QVERIFY(menu);
    auto show = menu->findChild<QAction*>("trayShowAction"); QVERIFY(show);
    show->trigger(); QTRY_VERIFY(window.isVisible());
    auto exit = menu->findChild<QAction*>("trayQuitAction"); QVERIFY(exit);
    exit->trigger(); exit->trigger();
    QCOMPARE(quit.count(),1);
    QVERIFY(window.close());
  }
  void unavailableTrayKeepsWindowReachable() {
    QQuickWindow window; window.show();
    QPixmap pixel(32,32); pixel.fill(Qt::blue);
    TestTray tray(&window,QIcon(pixel));
    QSignalSpy notifications(&tray,&TrayController::notificationRequested);
    QVERIFY(!window.close());
    QCOMPARE(window.visibility(),QWindow::Minimized);
    QCOMPARE(notifications.count(),1);
    tray.restore(); QTRY_VERIFY(window.isVisible());
    QVERIFY(window.visibility()!=QWindow::Minimized);
  }
  void explicitQuitForUpdateIsNotIntercepted() {
    QQuickWindow window; window.show();
    QPixmap pixel(32,32); pixel.fill(Qt::blue);
    TestTray tray(&window,QIcon(pixel)); tray.available=true;
    QSignalSpy aboutToQuit(qApp,&QCoreApplication::aboutToQuit);
    QTimer::singleShot(0,qApp,&QCoreApplication::quit);
    QCoreApplication::exec();
    QCOMPARE(aboutToQuit.count(),1);
    QVERIFY(window.close());
  }
  void missingAvatarNeverHidesTheWindow() {
    QQuickWindow window; window.show();
    TestTray tray(&window,QIcon()); tray.available=true;
    QVERIFY(!window.close());
    QCOMPARE(window.visibility(),QWindow::Minimized);
    tray.restore(); QVERIFY(window.isVisible());
  }
};
QTEST_MAIN(TrayTests)
#include "tray_test.moc"
