#include "terminal_view.hpp"
#include <QFileDialog>
#include "backend.hpp"
#include "webview_support.hpp"
#include "syntaxhighlighter.hpp"
#include "text_spacing.hpp"
#include "input_method.hpp"
#include "update.hpp"
#include "tray.hpp"
#include "notifications.hpp"
#include <QApplication>
#include <QFontDatabase>
#include <QLibraryInfo>
#include <QPluginLoader>
#include <QQuickStyle>

static void inputMethod(const char *executable) {
#ifdef Q_OS_LINUX
  auto path = QFileInfo(QString::fromLocal8Bit(executable)).absolutePath();
  const auto plugins = path + "/plugins";
  QCoreApplication::addLibraryPath(plugins);
  if (qEnvironmentVariableIsSet("UMEKO_QT_IM_MODULE")) {
    qputenv("QT_IM_MODULE", qgetenv("UMEKO_QT_IM_MODULE"));
    return;
  }
  auto chosen = qgetenv("QT_IM_MODULE");
  bool fcitx = chosen == "fcitx" || chosen == "fcitx5" ||
               qgetenv("XMODIFIERS").contains("fcitx");
  if (!fcitx || (!chosen.isEmpty() && chosen != "fcitx" && chosen != "fcitx5"))
    return;
  auto contexts = plugins + "/platforminputcontexts";
  if (!QDir(contexts).entryList({"*fcitx*"}, QDir::Files).isEmpty())
    qputenv("QT_IM_MODULE", "fcitx");
#else
  Q_UNUSED(executable);
#endif
}
int main(int argc, char **argv) {
  inputMethod(argv[0]);
  auto earlyRoot = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
#ifdef Q_OS_MACOS
  earlyRoot += "/../Resources";
#endif
  for (int i = 1; i + 1 < argc; ++i)
    if (QString::fromLocal8Bit(argv[i]) == "--resource-root") earlyRoot = QString::fromLocal8Bit(argv[i + 1]);
  configureWebView(U::cleanPath(earlyRoot));
  QQuickStyle::setStyle("Basic");
  QApplication app(argc, argv);
  InputMethodUpdates inputUpdates(&app);
  app.setApplicationName("Umeko-Desktop");
  app.setApplicationDisplayName("Umeko-Desktop");
  app.setApplicationVersion(UMEKO_VERSION);
  app.setOrganizationName("Umeko");
#ifdef Q_OS_WIN
  if (QFontDatabase::families().isEmpty())
    for (auto file : {"msyh.ttc", "segoeui.ttf", "consola.ttf"})
      QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR") +
                                        "/Fonts/" + file);
  app.setFont(QFont("Microsoft YaHei UI", 10));
#endif
  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addOptions(
      {{"resource-root", "Runtime resource directory", "path"},
       {"login-startup", "Launch at user login"},
       {"login-umeko", "Start the desktop in its tray"},
       {"login-proxy", "Start the configured API proxy"},
       {"data-dir", "Isolated local data directory", "path"},
       {"smoke", "Render the actual application and exit"},
       {"snapshot", "Save the real window to a PNG file", "path"},
       {"demo", "Populate isolated screenshot demonstration data"}});
  parser.addOption({"install-update", "Run the detached update installer", "job"});
  parser.process(app);
  auto root = parser.value("resource-root");
  if (root.isEmpty()) {
#ifdef Q_OS_MACOS
    root =
        QDir(QCoreApplication::applicationDirPath()).filePath("../Resources");
#else
    root = QCoreApplication::applicationDirPath();
#endif
  }
  root = U::cleanPath(root);
  app.setWindowIcon(QIcon(":/qt/qml/Umeko/App/app/icons/logo.svg"));
  if (parser.isSet("install-update")) {
    UpdateInstaller installer(parser.value("install-update"), root);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("installer", &installer);
    engine.load(QUrl("qrc:/qt/qml/Umeko/App/app/qml/Installer.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    QTimer::singleShot(0, &installer, &UpdateInstaller::begin);
    return app.exec();
  }
  qmlRegisterType<TerminalView>("Umeko.Terminal",1,0,"TerminalView");
  qmlRegisterType<SyntaxHighlighter>("Umeko.Text", 1, 0, "SyntaxHighlighter");
  qmlRegisterType<TextSpacing>("Umeko.Text",1,0,"TextSpacing");
  Backend backend(root, parser.value("data-dir"));
  QLockFile instance(backend.store.directory+"/.instance.lock");instance.setStaleLockTime(0);
  const bool ownsInstance=instance.tryLock(0);
  if(parser.isSet("login-startup") && !ownsInstance){backend.close();return 0;}

  QObject::connect(qobject_cast<DesktopPlugins*>(backend.desktopPlugins()), &DesktopPlugins::openWithRequested,
                   &app, [&backend](const QString &path) {
    const auto application=QFileDialog::getOpenFileName(nullptr,
        backend.language()=="en" ? "Choose an application" : "选择打开此文件的应用",
#ifdef Q_OS_MACOS
        "/Applications");
    const bool launched=application.isEmpty() || QProcess::startDetached("/usr/bin/open",{"-a",application,path});
#else
        "/usr/bin");
    const bool launched=application.isEmpty() || QProcess::startDetached(application,{path});
#endif
    if(!launched)emit backend.toast(backend.language()=="en" ? "Could not start the selected application" : "无法启动所选应用");
  });
  QQmlApplicationEngine engine;
  const auto browserImports = webviewConfiguration()["qmlPath"].toString();
  if (!browserImports.isEmpty()) engine.addImportPath(browserImports);
  engine.rootContext()->setContextProperty("backend", &backend);
  engine.rootContext()->setContextProperty("proxyOnlyLogin",parser.isSet("login-startup")&&!parser.isSet("login-umeko"));
  auto diagnostic = [](QString line) {
    auto path = qEnvironmentVariable("UMEKO_DIAGNOSTICS");
    if (!path.isEmpty()) {
      QFile out(path);
      QDir().mkpath(QFileInfo(path).absolutePath());
      if (out.open(QIODevice::Append))
        out.write(line.toUtf8() + '\n');
    } else
      qInfo().noquote() << line;
  };
  QObject::connect(&engine, &QQmlEngine::warnings, &app,
                   [diagnostic](const QList<QQmlError> &errors) {
                     for (auto error : errors)
                       diagnostic(error.toString());
                   });
  engine.load(QUrl("qrc:/qt/qml/Umeko/App/app/qml/Main.qml"));
  if (engine.rootObjects().isEmpty()) {
    diagnostic("QML_FAILED");
    return 1;
  }
  auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
  window->setTitle(app.applicationDisplayName());
  const QIcon trayIcon(":/qt/qml/Umeko/App/app/icons/yanami-pixel.webp");
  TrayController tray(window, trayIcon);
  TaskNotifications notifications(window, tray.systemTray());
  notifications.setLanguage(backend.language());
  QObject::connect(&backend, &Backend::appearanceChanged, &notifications, [&] {notifications.setLanguage(backend.language());});
  QObject::connect(&backend, &Backend::taskFinished, &notifications, &TaskNotifications::taskFinished);
  QObject::connect(&notifications, &TaskNotifications::activated, &backend, [&](const QString &session) {
    window->setProperty("page", "chat"); backend.openSession(session); tray.restore();
  });
  const auto refreshTray=[&] {tray.systemTray()->setIcon(backend.characterStyle()["id"]=="system" ? QIcon(":/qt/qml/Umeko/App/app/icons/characters/system/robot.png") : trayIcon);};
  QObject::connect(&backend,&Backend::changed,&app,refreshTray);refreshTray();
  tray.setLanguage(backend.language());
  QObject::connect(&backend, &Backend::appearanceChanged, &tray, [&] {tray.setLanguage(backend.language());});
  QObject::connect(&tray, &TrayController::quitRequested, &app, &QCoreApplication::quit);
  QObject::connect(&tray, &TrayController::notificationRequested, &backend, &Backend::toast);
  diagnostic("QML_READY");
  if(parser.isSet("login-startup")) {
    QTimer::singleShot(0,window,&QWindow::hide);
    if(parser.isSet("login-proxy")) {
      auto timer=new QTimer(&app);auto attempts=std::make_shared<int>(0);
      QObject::connect(timer,&QTimer::timeout,&app,[&,timer,attempts]{
        if(!backend.kernelBusy()){timer->stop();if(!backend.serverRunning())backend.toggleServer();timer->deleteLater();}
        else if(++*attempts>=200){timer->stop();timer->deleteLater();}
      });timer->start(100);
    }
  }

  if (!parser.isSet("smoke"))
    qobject_cast<AppAbout *>(backend.appAbout())->startAutomaticChecks();
  if (parser.isSet("smoke")) {
    // Validate the installed filesystem imports as well as the main QRC. A
    // source-tree UI test cannot catch a missing public app/qml directory.
    const QDir widgets(root + "/umeko-plugins");
    for (const auto &id : widgets.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
      const auto file = widgets.filePath(id + "/qml/Main.qml");
      if (!QFileInfo::exists(file)) continue;
      QQmlComponent component(&engine, QUrl::fromLocalFile(file));
      if (component.isLoading()) {
        QEventLoop loop;
        QObject::connect(&component, &QQmlComponent::statusChanged, &loop,
                         &QEventLoop::quit);
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
      }
      if (!component.isReady()) {
        diagnostic("WIDGET_QML_FAILED:" + id + ":" + component.errorString());
        backend.close();
        return 1;
      }
      diagnostic("WIDGET_QML_READY:" + id);
    }
    // Exercise the packaged QRC, which file-based QML tests cannot validate.
    for(const auto &path:{QString("app/qml/skill.svg"),QString("app/icons/pi-widget.svg"),QString("app/icons/widget-manager.svg"),QString("app/icons/yanami-desktop-settings-v2.png")}) {
      if(QIcon(":/qt/qml/Umeko/App/"+path).pixmap(24,24).isNull()){diagnostic("UI_ART_MISSING:"+path);backend.close();return 1;}
    }
    diagnostic("UI_ART_READY:4");
#ifdef Q_OS_LINUX
    QPluginLoader ime(QCoreApplication::applicationDirPath() + "/plugins/platforminputcontexts/libfcitx5platforminputcontextplugin.so");
    diagnostic("IME_PLUGIN_READY:" + QString::number(ime.load()));
#else
    diagnostic("NATIVE_NOTIFICATIONS_READY:" + QString::number(notifications.isNativeReady()));
#endif
    diagnostic("TRAY_ICON_READY:" + QString::number(!trayIcon.pixmap(32, 32).isNull()));
    diagnostic("WINDOW_TITLE:" + window->title());
    diagnostic("COMPILER:" + QStringLiteral(UMEKO_COMPILER));
    diagnostic("NODE_EXECUTABLE:" + U::node());
    diagnostic(
        "PYTHON_EXECUTABLE:" +
        backend.providerPlugins()->property("pythonExecutable").toString());
  }
  QObject::connect(&app, &QCoreApplication::aboutToQuit, &backend,
                   &Backend::close);
  QTimer::singleShot(200, &backend, &Backend::loadNativeIndex);
  if (parser.isSet("demo")) {
    if (parser.isSet("snapshot")) window->resize(1480, 960);
    QTimer::singleShot(600, &backend, &Backend::injectDemo);

  }
  if (parser.isSet("snapshot"))
    QTimer::singleShot(qBound(1200,qEnvironmentVariableIntValue("UMEKO_SNAPSHOT_DELAY_MS") ? qEnvironmentVariableIntValue("UMEKO_SNAPSHOT_DELAY_MS") : qEnvironmentVariableIsSet("UMEKO_WEB_PREVIEW_URL") ? 4000 : 1200,30000), &app,
                       [window, path = parser.value("snapshot"), diagnostic] {
                         if (!window->grabWindow().save(path))
                           diagnostic("SNAPSHOT_FAILED");
                       });
  if (parser.isSet("smoke"))
    QTimer::singleShot(qEnvironmentVariableIsSet("UMEKO_WEB_PREVIEW_URL") ? 5500 : 2000, &app, &QCoreApplication::quit);
  int result = app.exec();
  backend.close();
  diagnostic("EXIT:" + QString::number(result));
  return result;
}
