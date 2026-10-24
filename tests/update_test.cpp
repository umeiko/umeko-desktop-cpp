#include "update.hpp"
#include "miniz.h"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QtTest>

class UpdateTests : public QObject {
  Q_OBJECT
private slots:
  void installer_data() {
    QTest::addColumn<bool>("badHash");
    QTest::newRow("successful-install") << false;
    QTest::newRow("changed-package") << true;
  }
  void installer() {
    QFETCH(bool,badHash);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto root = QStringLiteral(UMEKO_SOURCE_DIR);
    const auto target = directory.path() + "/installed";
    QVERIFY(QDir().mkpath(target));
#ifdef Q_OS_WIN
    const QString executable = "UmekoCpp.exe", node = "node.exe";
    const QString system = "Windows";
#elif defined(Q_OS_MACOS)
    const QString executable = "UmekoCpp", node = "bin/node";
    const QString system = "Darwin";
#else
    const QString executable = "UmekoCpp", node = "bin/node";
    const QString system = "Linux";
#endif
    QFile old(target+"/"+executable); QVERIFY(old.open(QIODevice::WriteOnly)); old.write("old app"); old.close();
    const auto archive = directory.path()+"/update.zip";
    mz_zip_archive zip{}; QVERIFY(mz_zip_writer_init_file(&zip,archive.toUtf8().constData(),0));
    const auto architecture = QSysInfo::currentCpuArchitecture() == "arm64" ? "arm64" : "x64";
    QMap<QString,QByteArray> files{{executable,"new app"},{"runtime/update-engine.mjs","// updater"},{"runtime/node/"+node,"node"},
      {"build-info.json",QJsonDocument(QJsonObject{{"version","0.3.0"},{"system",system},{"architecture",architecture}}).toJson()}};
    for (auto it=files.begin();it!=files.end();++it) {
      auto name=("UmekoCpp/"+it.key()).toUtf8();
      QVERIFY(mz_zip_writer_add_mem(&zip,name.constData(),it.value().constData(),it.value().size(),MZ_DEFAULT_COMPRESSION));
    }
    QVERIFY(mz_zip_writer_finalize_archive(&zip)); mz_zip_writer_end(&zip);
    QFile package(archive); QVERIFY(package.open(QIODevice::ReadOnly));
    auto digest=QCryptographicHash::hash(package.readAll(),QCryptographicHash::Sha256).toHex();package.close();
    const auto jobFile = directory.path()+"/job.json";
    QVERIFY(U::writeJson(jobFile+".commit",QVariantMap{{"confirmed",true}}));
    QVERIFY(U::writeJson(jobFile,QVariantMap{{"installDir",target},{"executable",target+"/"+executable},{"archive",archive},
      {"sha256",badHash?QString(64,'0'):QString::fromLatin1(digest)},{"version","v0.3.0"},{"payloadRelative","UmekoCpp"},
      {"resourceRelative",""},{"parentPid",2147483647}}));
    UpdateInstaller installer(jobFile,root);
    QQmlApplicationEngine engine;
    QStringList warnings;
    connect(&engine,&QQmlApplicationEngine::warnings,this,[&](const QList<QQmlError>& errors){for(auto e:errors)warnings<<e.toString();});
    engine.rootContext()->setContextProperty("installer",&installer);
    engine.load(QUrl::fromLocalFile(root+"/app/qml/Installer.qml"));
    QVERIFY(!engine.rootObjects().isEmpty());
    auto window=qobject_cast<QQuickWindow*>(engine.rootObjects().first());QVERIFY(window);
    installer.begin();
    QTRY_VERIFY_WITH_TIMEOUT(installer.status()["state"] == "complete" || installer.status()["state"] == "error",10000);
    QCOMPARE(installer.status()["state"].toString(),badHash?QString("error"):QString("complete"));
    QVERIFY(QFileInfo::exists(jobFile+".ready"));
    QFile installed(target+"/"+executable); QVERIFY(installed.open(QIODevice::ReadOnly));
    QCOMPARE(installed.readAll(),badHash?QByteArray("old app"):QByteArray("new app"));
    QTest::qWait(150);
    QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join('\n')));
    if (!badHash) {
      QDir().mkpath(root+"/build/screenshots");
      QVERIFY(window->grabWindow().save(root+"/build/screenshots/installer-complete.png"));
    }
    QVERIFY(window->close());
  }
};
int main(int argc,char**argv) {
  QQuickStyle::setStyle("Basic");QGuiApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);
  UpdateTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "update_test.moc"
