#include "backend.hpp"
#include "syntaxhighlighter.hpp"
#include <QFontDatabase>
#include <QLibraryInfo>
#include <QQuickStyle>

static void inputMethod() {
#ifdef Q_OS_LINUX
  if (qEnvironmentVariableIsSet("UMEKO_QT_IM_MODULE")) {
    qputenv("QT_IM_MODULE", qgetenv("UMEKO_QT_IM_MODULE"));
    return;
  }
  auto chosen = qgetenv("QT_IM_MODULE");
  bool fcitx = chosen == "fcitx" || chosen == "fcitx5" ||
               qgetenv("XMODIFIERS").contains("fcitx");
  if (!fcitx || (!chosen.isEmpty() && chosen != "fcitx" && chosen != "fcitx5"))
    return;
  auto plugins =
      QLibraryInfo::path(QLibraryInfo::PluginsPath) + "/platforminputcontexts";
  if (!QDir(plugins).entryList({"*fcitx*"}, QDir::Files).isEmpty())
    qputenv("QT_IM_MODULE", "fcitx");
  else if (!QDir(plugins).entryList({"*ibus*"}, QDir::Files).isEmpty())
    qputenv("QT_IM_MODULE", "ibus");
#endif
}
int main(int argc, char **argv) {
  inputMethod();
  QQuickStyle::setStyle("Basic");
  QGuiApplication app(argc, argv);
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
       {"data-dir", "Isolated local data directory", "path"},
       {"smoke", "Render the actual application and exit"},
       {"snapshot", "Save the real window to a PNG file", "path"},
       {"demo", "Populate isolated screenshot demonstration data"}});
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
  app.setWindowIcon(QIcon("qrc:/qt/qml/Umeko/App/app/icons/app.webp"));
  qmlRegisterType<SyntaxHighlighter>("Umeko.Text", 1, 0, "SyntaxHighlighter");
  Backend backend(root, parser.value("data-dir"));
  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("backend", &backend);
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
  diagnostic("QML_READY");
  if (parser.isSet("smoke")) {
    diagnostic("WINDOW_TITLE:" + window->title());
    diagnostic("COMPILER:" + QStringLiteral(UMEKO_COMPILER));
    diagnostic(
        "PYTHON_EXECUTABLE:" +
        backend.providerPlugins()->property("pythonExecutable").toString());
  }
  QObject::connect(&app, &QCoreApplication::aboutToQuit, &backend,
                   &Backend::close);
  QTimer::singleShot(200, &backend, &Backend::loadNativeIndex);
  if (parser.isSet("demo"))
    QTimer::singleShot(600, &backend, &Backend::injectDemo);
  if (parser.isSet("snapshot"))
    QTimer::singleShot(1200, &app,
                       [window, path = parser.value("snapshot"), diagnostic] {
                         if (!window->grabWindow().save(path))
                           diagnostic("SNAPSHOT_FAILED");
                       });
  if (parser.isSet("smoke"))
    QTimer::singleShot(2000, &app, &QCoreApplication::quit);
  int result = app.exec();
  backend.close();
  diagnostic("EXIT:" + QString::number(result));
  return result;
}
