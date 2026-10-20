#include "backend.hpp"
#include "miniz.h"
#include "syntaxhighlighter.hpp"
#include <QQuickStyle>
#include <QtTest>

class NativeUiTests : public QObject {
  Q_OBJECT
  QTemporaryDir directory;
  std::unique_ptr<Backend> backend;
  std::unique_ptr<QQmlApplicationEngine> engine;
  QQuickWindow *window = nullptr;
  QString root;
  QStringList warnings;
  QTcpServer upstream;
  QVariantList requests;
  bool nativeTool = false, slow = false;
  int nativeCalls = 0;
  QQuickItem *find(QString name) {
    QList<QQuickItem *> items{window->contentItem()};
    while (!items.isEmpty()) {
      auto item = items.takeLast();
      if (item->objectName() == name)
        return item;
      items.append(item->childItems());
    }
    return nullptr;
  }
  QPoint point(QQuickItem *item, qreal x = -1, qreal y = -1) {
    return item
        ->mapToScene(QPointF(x < 0 ? item->width() / 2 : x,
                             y < 0 ? item->height() / 2 : y))
        .toPoint();
  }
  void click(QString name) {
    auto item = find(name);
    QVERIFY2(item, qPrintable(name));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point(item));
    QTest::qWait(100);
  }
  QString lastText() {
    auto messages = backend->messages();
    return messages.isEmpty() ? QString()
                              : messages.last().toMap()["text"].toString();
  }
  bool package(QString path, bool malicious = false) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, path.toUtf8().constData(), 0))
      return false;
    bool ok = true;
    if (malicious) {
      const char *text = "bad";
      ok = mz_zip_writer_add_mem(&zip, "../outside.txt", text, 3,
                                 MZ_DEFAULT_COMPRESSION);
    } else {
      auto base = root + "/provider_dev/examples/echo-provider";
      QDirIterator it(base, QDir::Files, QDirIterator::Subdirectories);
      while (it.hasNext()) {
        QFile file(it.next());
        auto name = QDir(base).relativeFilePath(file.fileName()).toUtf8();
        if (name.contains("__pycache__"))
          continue;
        if (!file.open(QIODevice::ReadOnly)) {
          ok = false;
          break;
        }
        auto raw = file.readAll();
        ok &= mz_zip_writer_add_mem(&zip, name.constData(), raw.constData(),
                                    raw.size(), MZ_DEFAULT_COMPRESSION);
      }
    }
    ok &= mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    return ok;
  }
  void configureAPI() {
    auto port = upstream.serverPort();
    auto models = QString::fromUtf8(
        QJsonDocument(QJsonArray{QJsonObject{{"upstream", "fixture-model"},
                                             {"alias", "native-fixture"},
                                             {"contextWindow", 128000},
                                             {"contextWindowAuto", false},
                                             {"vision", false},
                                             {"visionAuto", false}}})
            .toJson());
    QVERIFY(backend->saveProvider("", "Fixture API", "openai",
                                  QString("http://127.0.0.1:%1/v1").arg(port),
                                  "", models));
    QString id;
    for (auto v : backend->config()["providers"].toList())
      if (v.toMap()["name"] == "Fixture API")
        id = v.toMap()["id"].toString();
    backend->selectModel(id, "native-fixture");
  }
private slots:
  void initTestCase() {
#ifdef Q_OS_WIN
    for (auto file : {"msyh.ttc", "segoeui.ttf", "consola.ttf"})
      QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR") +
                                        "/Fonts/" + file);
    QGuiApplication::setFont(QFont("Microsoft YaHei UI", 10));
#endif
    root = qEnvironmentVariable("UMEKO_TEST_ROOT", QDir::currentPath());
    QVERIFY(directory.isValid());
    QQuickStyle::setStyle("Basic");
    qmlRegisterType<SyntaxHighlighter>("Umeko.Text", 1, 0, "SyntaxHighlighter");
    Store seed(directory.path());
    auto agent = seed.data["agent"].toMap();
    agent["agentDir"] = directory.filePath("agent");
    agent["safeMode"] = true;
    agent["autoCompaction"] = false;
    seed.data["agent"] = agent;
    QVERIFY(seed.save());
    backend = std::make_unique<Backend>(root, directory.path());
    engine = std::make_unique<QQmlApplicationEngine>();
    engine->rootContext()->setContextProperty("backend", backend.get());
    connect(engine.get(), &QQmlEngine::warnings, this,
            [this](const QList<QQmlError> &errors) {
              for (auto e : errors)
                warnings << e.toString();
            });
    engine->load(QUrl::fromLocalFile(root + "/app/qml/Main.qml"));
    QVERIFY(!engine->rootObjects().isEmpty());
    window = qobject_cast<QQuickWindow *>(engine->rootObjects().first());
    QVERIFY(window);
    window->show();
    window->requestActivate();
    QTest::qWait(500);
    QTRY_VERIFY_WITH_TIMEOUT(!backend->kernelBusy(), 15000);
    QVERIFY(upstream.listen(QHostAddress::LocalHost, 0));
    connect(&upstream, &QTcpServer::newConnection, this, [this] {
      while (upstream.hasPendingConnections()) {
        auto socket = upstream.nextPendingConnection();
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
          *buffer += socket->readAll();
          int split = buffer->indexOf("\r\n\r\n");
          if (split < 0)
            return;
          auto headers = buffer->left(split);
          auto match =
              QRegularExpression("Content-Length: (\\d+)",
                                 QRegularExpression::CaseInsensitiveOption)
                  .match(QString::fromLatin1(headers));
          if (!match.hasMatch())
            return;
          int size = match.captured(1).toInt();
          if (buffer->size() < split + 4 + size)
            return;
          auto body = QJsonDocument::fromJson(buffer->mid(split + 4, size))
                          .object()
                          .toVariantMap();
          requests << body;
          buffer->clear();
          QVariantMap message;
          QString finish = "stop";
          if (nativeTool && nativeCalls++ == 0) {
            QVariantMap function{{"name", "write"},
                                 {"arguments",
                                  "{\"path\":\"native-proof.txt\",\"content\":"
                                  "\"native tool result\"}"}};
            QVariantMap call{{"id", "native-write"},
                             {"type", "function"},
                             {"function", function}};
            message = {{"role", "assistant"},
                       {"content", QVariant()},
                       {"tool_calls", QVariantList{call}}};
            finish = "tool_calls";
          } else {
            auto msgs = body["messages"].toList();
            QString text;
            for (auto it = msgs.crbegin(); it != msgs.crend(); ++it)
              if (it->toMap()["role"] == "user") {
                auto content = it->toMap()["content"];
                if (content.metaType().id() == QMetaType::QString)
                  text = content.toString();
                else
                  for (auto block : content.toList())
                    if (block.toMap()["type"] == "text")
                      text += block.toMap()["text"].toString();
                break;
              }
            message = {
                {"role", "assistant"},
                {"content", nativeTool ? "Native complete" : "Got: " + text}};
          }
          QJsonObject response{
              {"choices", QJsonArray{QJsonObject{
                              {"message", QJsonObject::fromVariantMap(message)},
                              {"finish_reason", finish}}}},
              {"usage",
               QJsonObject{{"prompt_tokens", 100}, {"completion_tokens", 10}}}};
          auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
          auto send = [socket, bytes] {
            if (socket->state() != QAbstractSocket::ConnectedState)
              return;
            socket->write(
                "HTTP/1.1 200 OK\r\nContent-Type: "
                "application/json\r\nConnection: close\r\nContent-Length: " +
                QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
            socket->disconnectFromHost();
          };
          QTimer::singleShot(slow ? 1000 : 0, socket, send);
        });
      }
    });
  }
  void welcomeAndEggs() {
    auto mascot = find("welcomeMascot");
    QVERIFY(mascot);
    QVERIFY(mascot->property("loaded").toBool());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point(mascot));
    QTest::qWait(60);
    QVERIFY(!mascot->property("reactionText").toString().isEmpty());
    QVERIFY(mascot->property("animating").toBool());
    QMetaObject::invokeMethod(mascot, "reset");
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, point(mascot));
    QTest::qWait(700);
    QVERIFY(mascot->property("celebrating").toBool());
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, point(mascot));
    QMetaObject::invokeMethod(mascot, "reset");
    auto start = point(mascot);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(window, start + QPoint(65, 25), 80);
    QVERIFY(mascot->property("dragging").toBool());
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                        start + QPoint(65, 25));
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(mascot->property("offsetX").toDouble()) < 0.5,
                             4000);
  }
  void sidebarAndPid() {
    backend->saveSidebarLayout(310, false);
    QCOMPARE(backend->sidebarWidth(), 310);
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTest::qWait(100);
    QVERIFY(backend->sidebarCollapsed());
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTest::qWait(100);
    QVERIFY(!backend->sidebarCollapsed());
    click("settingsButton");
    auto highlight = find("settingsTabHighlight");
    QVERIFY(highlight);
    qreal before = highlight->x();
    click("settingsTab_5");
    QVERIFY(highlight->property("targetX").toDouble() > before);
    QVERIFY(highlight->x() != before);
    click("settingsTab_1");
    QTRY_VERIFY_WITH_TIMEOUT(!highlight->property("moving").toBool(), 4000);
    QVERIFY(qAbs(highlight->x() - highlight->property("targetX").toDouble()) <
            0.2);
    window->setProperty("page", "chat");
    backend->saveSidebarLayout(244, false);
  }
  void historyFoldsAndSelection() {
    backend->injectDemo();
    QTest::qWait(400);
    auto view = find("conversation");
    QVERIFY(view);
    QCOMPARE(view->property("count").toInt(), 2);
    click("toggleThinking_1_0");
    QVERIFY(find("thinkingText_1_0")->isVisible());
    click("toggleTools_1_20000");
    auto path = find("toolPath_write-site");
    QVERIFY(path);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      point(path, 30, 7));
    QCOMPARE(QGuiApplication::clipboard()->text(),
             QString("/workspace/umeko-notes/site.config.ts"));
    auto selection = find("chatSelection");
    QVERIFY(selection);
    QMetaObject::invokeMethod(selection, "selectAll");
    QTest::qWait(80);
    QMetaObject::invokeMethod(selection, "copy");
    auto copied = QGuiApplication::clipboard()->text();
    QVERIFY(copied.contains("保留 Markdown"));
    QVERIFY(copied.contains("export default"));
    QVERIFY(copied.contains("npm run dev"));
    QMetaObject::invokeMethod(selection, "clear");
    window->resize(1060, 700);
    QTest::qWait(150);
    auto before = view->property("contentY").toDouble();
    QWheelEvent event(QPointF(point(view)), QPointF(point(view)), QPoint(),
                      QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(window, &event);
    QTest::qWait(100);
    QVERIFY(view->property("contentY").toDouble() <= before);
    window->resize(1320, 860);
    backend->newSession();
  }
  void nativeToolsAndHistory() {
    configureAPI();
    backend->setWorkspace(directory.path());
    nativeTool = true;
    nativeCalls = 0;
    backend->send("Write a native proof");
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 30000);
    QVERIFY2(lastText().contains("Native complete"), qPrintable(lastText()));
    QFile proof(directory.filePath("native-proof.txt"));
    QVERIFY(proof.open(QIODevice::ReadOnly));
    QCOMPARE(proof.readAll(), QByteArray("native tool result"));
    QVERIFY(!requests.isEmpty());
    QSet<QString> tools;
    for (auto v : requests.last().toMap()["tools"].toList())
      tools.insert(v.toMap()["function"].toMap()["name"].toString());
    for (auto name : {"read", "write", "edit", "bash", "powershell", "find",
                      "grep", "ls", "codemode", "tool_search"})
      QVERIFY2(tools.contains(name), name);
    auto id = backend->activeSession();
    backend->newSession();
    backend->openSession(id);
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 15000);
    QVERIFY(lastText().contains("Native complete"));
    nativeTool = false;
  }
  void followUpSteering() {
    backend->newSession();
    slow = true;
    auto start = requests.size();
    backend->send("first queued fixture");
    QTRY_VERIFY_WITH_TIMEOUT(backend->running(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(requests.size() > start, 15000);
    backend->send("second queued fixture");
    QTRY_COMPARE_WITH_TIMEOUT(backend->queuedMessages().size(), 1, 5000);
    backend->steer("");
    QTRY_COMPARE_WITH_TIMEOUT(
        backend->queuedMessages().first().toMap()["mode"].toString(),
        QString("steering"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 30000);
    QVERIFY2(lastText().contains("second queued fixture"),
             qPrintable(lastText()));
    slow = false;
  }
  void providerPackages() {
    auto bad = directory.filePath("bad.zip");
    QVERIFY(package(bad, true));
    auto manager = qobject_cast<Providers *>(backend->providerPlugins());
    QVERIFY(manager);
    auto before = manager->rows().size();
    QVERIFY(!manager->importZip(QUrl::fromLocalFile(bad).toString()));
    QCOMPARE(manager->rows().size(), before);
    auto archive = directory.filePath("echo.zip");
    QVERIFY(package(archive));
    QVERIFY(manager->importZip(QUrl::fromLocalFile(archive).toString()));
    QCOMPARE(manager->rows().size(), before + 1);
    auto id = manager->rows().last().toMap()["id"].toString();
    auto python = qEnvironmentVariable("UMEKO_TEST_PYTHON", U::python());
    QVERIFY(!python.isEmpty());
    QVERIFY(manager->setPythonPath(python));
    manager->testPython(python);
    QTRY_VERIFY_WITH_TIMEOUT(manager->pythonStatus().startsWith("Python 3."),
                             15000);
    manager->login(id, "{\"username\":\"fixture-user\"}");
    QTRY_VERIFY_WITH_TIMEOUT(!manager->rows().last().toMap()["busy"].toBool(),
                             15000);
    QVERIFY2(!manager->rows().last().toMap()["models"].toList().isEmpty(),
             qPrintable(manager->rows().last().toMap()["status"].toString()));
    backend->newSession();
    backend->selectModel(id, "local-echo");
    backend->send("provider compatibility fixture");
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 30000);
    QVERIFY2(lastText().contains("Echo: provider compatibility fixture"),
             qPrintable(lastText()));
  }
  void cleanupTestCase() {
    backend->close();
    engine.reset();
    backend.reset();
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
  }
};
QTEST_MAIN(NativeUiTests)
#include "ui_test.moc"
