#include "webview_support.hpp"
#include "common.hpp"
#include <QLibraryInfo>

static QVariantMap configuration;
static QString components() {
  auto path = qEnvironmentVariable("UMEKO_WEBVIEW_COMPONENT_DIR");
  if (!path.isEmpty()) return path;
  auto base = qEnvironmentVariable("XDG_DATA_HOME", QDir::homePath() + "/.local/share");
  return base + "/umeko/components";
}
QVariantMap webviewConfiguration() {
  auto result = configuration;
  result["directory"] = components();
  result["qt"] = QString::fromLatin1(qVersion());
  if (!result.contains("available")) result["available"] = false;
  return result;
}
void configureWebView(const QString &resourceRoot) {
  configuration = {{"available", false}};
#ifdef Q_OS_LINUX
  const auto spec = U::readJson(resourceRoot + "/runtime/webview-component.json").toMap();
  const auto active = U::readJson(components() + "/active.json").toMap();
  const auto directory = active["directory"].toString();
  if (spec["sha256"].toString().size() != 64 || active["id"] != spec["id"] || active["sha256"] != spec["sha256"] ||
      directory.isEmpty() || directory != QFileInfo(directory).fileName() || directory.contains("..")) return;
  const auto root = components() + "/" + directory;
  const auto manifest = U::readJson(root + "/manifest.json").toMap();
  if (manifest["id"] != spec["id"] || manifest["qt"] != QString::fromLatin1(qVersion())) return;
  for (const auto &file : {"lib/libQt6WebEngineCore.so.6", "libexec/QtWebEngineProcess", "plugins/webview/libqtwebview_webengine.so"})
    if (!QFileInfo::exists(root + "/" + file)) return;
  QCoreApplication::addLibraryPath(root + "/plugins");
  qputenv("QTWEBENGINEPROCESS_PATH", QFile::encodeName(root + "/libexec/QtWebEngineProcess"));
  qputenv("QTWEBENGINE_RESOURCES_PATH", QFile::encodeName(root + "/resources"));
  qputenv("QTWEBENGINE_LOCALES_PATH", QFile::encodeName(root + "/translations/qtwebengine_locales"));
  configuration["qmlPath"] = root + "/qml";
  const auto libraryPath = root + "/lib:" + QLibraryInfo::path(QLibraryInfo::LibrariesPath);
  qputenv("LD_LIBRARY_PATH", QFile::encodeName(libraryPath) + ':' + qgetenv("LD_LIBRARY_PATH"));
  // The shipped frontend has no WebEngine dependency. The optional plugin resolves
  // its libraries within the component through $ORIGIN RUNPATHs.
#endif
  QString library;
#ifdef Q_OS_WIN
  library = QLibraryInfo::path(QLibraryInfo::BinariesPath) + "/Qt6WebView.dll";
#elif defined(Q_OS_MACOS)
  library = QLibraryInfo::path(QLibraryInfo::LibrariesPath) + "/QtWebView.framework/QtWebView";
#else
  library = QLibraryInfo::path(QLibraryInfo::LibrariesPath) + "/libQt6WebView.so.6";
#endif
  QLibrary frontend(library);
  frontend.setLoadHints(QLibrary::PreventUnloadHint | QLibrary::ExportExternalSymbolsHint);
  bool ready = frontend.load();
  auto quickPath = library;
  quickPath.replace("WebView.so", "WebViewQuick.so").replace("WebView.dll", "WebViewQuick.dll");
#ifdef Q_OS_LINUX
  QLibrary quick(quickPath);
  quick.setLoadHints(QLibrary::PreventUnloadHint | QLibrary::ExportExternalSymbolsHint);
  ready = ready && quick.load();
  if (!ready) configuration["error"] = frontend.errorString() + "; " + quick.errorString();
  QPluginLoader backend(components() + "/" + U::readJson(components() + "/active.json").toMap()["directory"].toString() + "/plugins/webview/libqtwebview_webengine.so");
  ready = ready && backend.load();
  if (!ready && !configuration.contains("error")) configuration["error"] = backend.errorString();
#endif
  configuration["available"] = ready;
}
