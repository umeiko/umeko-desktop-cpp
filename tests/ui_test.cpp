#include "backend.hpp"
#include "miniz.h"
#include "syntaxhighlighter.hpp"
#include "update_network_fixture.hpp"
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QQuickStyle>
#include <QWheelEvent>
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
  bool holdReplies = false;
  bool upstreamError = false;
  bool limitResponse = false;
  int transientFailures = 0;
  QList<std::function<void()>> heldReplies;
  void releaseReplies() {
    holdReplies = false;
    const auto replies = heldReplies;
    heldReplies.clear();
    for (const auto &reply : replies)
      reply();
  }
  int nativeCalls = 0;
  bool mediaTool = false;
  int mediaCalls = 0;
  QString mediaImage, mediaReport;
  QList<QUrl> openedUrls;
  QString nativeHistory;
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
  void movePointer(QPoint position) {
#ifdef Q_OS_MACOS
    QCursor::setPos(window->mapToGlobal(position));
    QTest::qWait(80);
#endif
    // QWindow QtTest moves do not synthesize the preceding enter event,
    // including on the offscreen platform used by Windows/Linux CI.
    QEnterEvent enter(position, position, window->mapToGlobal(position));
    QCoreApplication::sendEvent(window, &enter);
    QTest::mouseMove(window, position);
  }
  void click(QString name) {
    auto item = find(name);
    QVERIFY2(item, qPrintable(name));
    // Backend notifications can arrive before QML has polished a new delegate
    // and moved its bottom-anchored composer. Click its settled geometry.
    QTest::qWait(150);
    QVERIFY(item->isVisible() && item->width() > 0 && item->height() > 0);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point(item));
    QTest::qWait(100);
  }
  QString lastText() {
    auto messages = backend->messages();
    return messages.isEmpty() ? QString()
                              : messages.last().toMap()["text"].toString();
  }
  bool package(QString path, bool malicious = false, bool updated = false,
               QString plugin = "example-echo", QString version = "0.1.0",
               QString prefix = {}) {
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
        if (name == "manifest.json") {
          auto manifest = QJsonDocument::fromJson(raw).object();
          manifest["id"] = plugin;
          manifest["version"] = version;
          if (!prefix.isEmpty()) {
            manifest["entrypoint"] = "./scripts/provider.py:create_provider";
            manifest["config"] = "./config.json";
            manifest["configSchema"] = "./config.schema.json";
          }
          raw = QJsonDocument(manifest).toJson();
        }
        if (updated && name == "scripts/provider.py")
          raw.replace("self.context.config[\"prefix\"] + content",
                      "\"Reinstalled: \" + content");
        ok &= mz_zip_writer_add_mem(&zip, (prefix.toUtf8() + name).constData(),
                                    raw.constData(), raw.size(),
                                    MZ_DEFAULT_COMPRESSION);
      }
    }
    ok &= mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    return ok;
  }
  void configureAPI() {
    for (auto item : backend->config()["providers"].toList()) {
      auto provider = item.toMap();
      if (provider["name"] == "Fixture API") {
        backend->selectModel(provider["id"].toString(), "native-fixture");
        return;
      }
    }
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
          if (transientFailures > 0) {
            --transientFailures;
            const QByteArray raw =
                "{\"error\":{\"message\":\"stream interrupted\"}}";
            socket->write("HTTP/1.1 503 Service Unavailable\r\nContent-Type: "
                          "application/json\r\nContent-Length: " +
                          QByteArray::number(raw.size()) +
                          "\r\nConnection: close\r\n\r\n" + raw);
            socket->disconnectFromHost();
            return;
          }
          if (upstreamError) {
            const auto raw =
                QJsonDocument(
                    QJsonObject{{"error", "complete upstream error details"}})
                    .toJson(QJsonDocument::Compact);
            socket->write("HTTP/1.1 400 Bad Request\r\nContent-Type: "
                          "application/json\r\nContent-Length: " +
                          QByteArray::number(raw.size()) +
                          "\r\nConnection: close\r\n\r\n" + raw);
            socket->disconnectFromHost();
            return;
          }
          QVariantMap message;
          QString finish = limitResponse ? "length" : "stop";
          if (mediaTool && mediaCalls++ == 0) {
            QVariantList items{
                QVariantMap{{"path", mediaImage},
                            {"title", "Preview image"},
                            {"caption", "Delivered through Pi"}},
                QVariantMap{{"path", mediaReport}, {"title", "Report"}}};
            QVariantMap function{
                {"name", "submit_to_chatbox"},
                {"arguments",
                 QString::fromUtf8(
                     QJsonDocument::fromVariant(QVariantMap{{"items", items}})
                         .toJson(QJsonDocument::Compact))}};
            message = {{"role", "assistant"},
                       {"content", QVariant()},
                       {"tool_calls",
                        QVariantList{QVariantMap{{"id", "media-delivery"},
                                                 {"type", "function"},
                                                 {"function", function}}}}};
            finish = "tool_calls";
          } else if (nativeTool && nativeCalls++ == 0) {
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
               QJsonObject{{"prompt_tokens", 100},
                           {"completion_tokens",
                            limitResponse ? body["max_tokens"].toInt() : 10}}}};
          auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
          auto send = [socket = QPointer<QTcpSocket>(socket), bytes] {
            if (!socket || socket->state() != QAbstractSocket::ConnectedState)
              return;
            socket->write(
                "HTTP/1.1 200 OK\r\nContent-Type: "
                "application/json\r\nConnection: close\r\nContent-Length: " +
                QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
            socket->disconnectFromHost();
          };
          if (holdReplies)
            heldReplies << send;
          else
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
    QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() +
                 60);
    QTest::mousePress(
        window, Qt::LeftButton, Qt::NoModifier, point(mascot),
        QGuiApplication::styleHints()->mouseDoubleClickInterval() + 60);
    QTRY_VERIFY_WITH_TIMEOUT(mascot->property("celebrating").toBool(), 2500);
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
  void chatWidthControls() {
    const auto previousSize = window->size();
    const auto cleanup = qScopeGuard([this, previousSize] {
      find("chatWidthControls")->setProperty("expandedLeft", false);
      find("chatWidthControls")->setProperty("expandedRight", false);
      window->resize(previousSize);
      QTest::qWait(250);
    });
    window->setProperty("page", "chat");
    window->resize(1700, 920);
    QTest::qWait(250);
    auto controls = find("chatWidthControls");
    auto composer = find("composer");
    QVERIFY(controls && composer);
    QVERIFY(controls->property("expandable").toBool());
    QCOMPARE(composer->width(), qreal(850));
    movePointer(QPoint(window->width() / 2, 130));
    QTest::qWait(250);
    QCOMPARE(find("expandChatLeft")->opacity(), qreal(0));
    QCOMPARE(find("expandChatRight")->opacity(), qreal(0));
    const auto clickEdge = [this](const QString &name) {
      auto item = find(name);
      movePointer(point(item));
      QTRY_VERIFY_WITH_TIMEOUT(item->opacity() > 0.95, 1000);
      click(name);
    };
    auto leftEdge = find("expandChatLeft");
    leftEdge->forceActiveFocus(Qt::TabFocusReason);
    movePointer(point(leftEdge));
    QTRY_VERIFY_WITH_TIMEOUT(leftEdge->opacity() > 0.95, 1000);
    movePointer(point(find("promptInput")));
    QTRY_VERIFY_WITH_TIMEOUT(leftEdge->opacity() < 0.01, 1000);
    controls->setEnabled(false);
    movePointer(QPoint(120, 120));
    controls->setEnabled(true);
    QTRY_VERIFY_WITH_TIMEOUT(find("expandChatLeft")->opacity() < 0.01 &&
                                find("expandChatRight")->opacity() < 0.01,
                            1000);
    const qreal left = composer->x(), right = left + composer->width();
    clickEdge("expandChatLeft");
    QTest::qWait(150);
    QVERIFY(composer->x() < left - 60);
    QVERIFY(qAbs(composer->x() + composer->width() - right) < 1);
    QCOMPARE(find("expandChatLeft")->property("text").toString(),
             QString(QChar(0x25b6)));
    clickEdge("expandChatRight");
    QTest::qWait(150);
    QVERIFY(composer->x() + composer->width() > right + 60);
    QVERIFY(qAbs(find("conversation")->width() - composer->width() + 20) < 1);
    clickEdge("expandChatLeft");
    QTest::qWait(150);
    QVERIFY(qAbs(composer->x() - left) < 1);
    window->resize(1060, 760);
    QTest::qWait(250);
    QVERIFY(!controls->property("expandable").toBool());
    QVERIFY(!find("expandChatLeft")->isVisible());
    QVERIFY(!find("expandChatRight")->isVisible());
    QVERIFY(controls->property("expandedRight").toBool());
    QVERIFY(composer->x() >= 40);
    window->resize(1700, 920);
    QTest::qWait(250);
    QVERIFY(find("expandChatRight")->isVisible());
    QVERIFY(composer->width() > 850);
    clickEdge("expandChatRight");
    QTest::qWait(150);
    QCOMPARE(composer->width(), qreal(850));
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
  void screenshotsAndPortraits() {
    auto mascot = find("welcomeMascot");
    QVERIFY(mascot);
    for (int i = 0; i < 12; ++i) {
      mascot->setProperty("portraitIndex", i);
      QTRY_VERIFY_WITH_TIMEOUT(mascot->property("loaded").toBool(), 2000);
    }
    mascot->setProperty("portraitIndex", 2);
    QDir().mkpath(root + "/build/screenshots");
    QTest::qWait(300);
    QVERIFY(window->grabWindow().save(root + "/build/screenshots/welcome.png"));
    click("settingsButton");
    window->setProperty("settingsTab", 0);
    QTest::qWait(400);
    QVERIFY(
        window->grabWindow().save(root + "/build/screenshots/providers.png"));
    window->setProperty("page", "chat");
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
    view->setProperty("contentY", view->property("contentHeight").toDouble() -
                                      view->height());
    auto before = view->property("contentY").toDouble();
    QWheelEvent event(QPointF(point(view)), QPointF(point(view)), QPoint(),
                      QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(window, &event);
    QTest::qWait(100);
    QVERIFY(view->property("contentY").toDouble() < before - 1);
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
    nativeHistory = id;
    backend->newSession();
    backend->openSession(id);
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 15000);
    QVERIFY(lastText().contains("Native complete"));
    nativeTool = false;
  }
  void oversizedCodeHighlighting() {
    QQmlComponent component(engine.get());
    component.setData(R"(import QtQuick
import Umeko.Text 1.0
Item {
 TextEdit { id: editor; objectName: "boundedEditor"; text: "const answer = 42" }
 SyntaxHighlighter { objectName: "boundedHighlighter"; document: editor.textDocument; language: "typescript" }
})",
                      QUrl());
    std::unique_ptr<QObject> item(component.create());
    QVERIFY2(item, qPrintable(component.errorString()));
    auto editor = item->findChild<QObject *>("boundedEditor");
    auto highlighter =
        item->findChild<SyntaxHighlighter *>("boundedHighlighter");
    QVERIFY(editor && highlighter);
    QTRY_VERIFY_WITH_TIMEOUT(highlighter->colored(), 2000);
    const QString oversized(100000, 'x');
    editor->setProperty("text", oversized);
    QTRY_VERIFY_WITH_TIMEOUT(!highlighter->colored(), 2000);
    QCOMPARE(editor->property("text").toString(), oversized);
    editor->setProperty("text", QString("line\n").repeated(1001));
    QTest::qWait(160);
    QVERIFY(!highlighter->colored());
    editor->setProperty("text", "const restored = 42");
    QTRY_VERIFY_WITH_TIMEOUT(highlighter->colored(), 2000);
  }
  void coldHistoryWithRetiredModel() {
    auto profile = directory.filePath("cold-profile");
    Store seed(profile);
    seed.data = backend->config();
    auto providers = seed.data["providers"].toList();
    QString id;
    for (auto &v : providers) {
      auto p = v.toMap();
      if (p["name"] == "Fixture API") {
        id = p["id"].toString();
        auto models = p["models"].toList();
        auto model = models.first().toMap();
        model["alias"] = "replacement-model";
        p["models"] = QVariantList{model};
        v = p;
      }
    }
    QVERIFY(!id.isEmpty());
    seed.data["providers"] = providers;
    seed.data["selectedProvider"] = id;
    seed.data["selectedModel"] = "replacement-model";
    QVERIFY(seed.save());
    Store original(directory.path());
    QVERIFY(seed.saveSessions(original.sessions()));
    Backend cold(root, profile);
    QTRY_VERIFY_WITH_TIMEOUT(!cold.kernelBusy(), 15000);
    auto loader = qobject_cast<SessionPreview *>(cold.sessionPreview());
    QVERIFY(loader);
    QSignalSpy ready(loader, &SessionPreview::ready);
    loader->prepare(nativeHistory, 10);
    loader->cancel();
    loader->prepare(nativeHistory, 11);
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 10000);
    QCOMPARE(ready.first().first().toInt(), 11);
    QVERIFY(ready.first().last().toBool());
    QVERIFY(!loader->activate(10));
    QVERIFY(loader->activate(11));
    QTRY_VERIFY_WITH_TIMEOUT(!cold.busy(), 15000);
    QVERIFY(!cold.messages().isEmpty());
    cold.send("continue retired-model history");
    QTRY_VERIFY_WITH_TIMEOUT(!cold.busy(), 30000);
    auto text = cold.messages().last().toMap()["text"].toString();
    QVERIFY2(text.contains("continue retired-model history"), qPrintable(text));
    cold.close();
  }
  void chatLoadingCover() {
    backend->newSession();
    auto cover = find("chatLoadingOverlay");
    QVERIFY(cover);
    auto select = [this](QString id) {
      const QVariant row = QVariantMap{{"id", id},
                                       {"title", "Loading fixture"},
                                       {"kind", "session"},
                                       {"selected", false}};
      return QMetaObject::invokeMethod(window, "selectSidebar",
                                       Q_ARG(QVariant, row));
    };
    QVERIFY(select(nativeHistory));
    QVERIFY(cover->isVisible());
    QCOMPARE(cover->opacity(), 1.0);
    QVERIFY(backend->activeSession().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(cover->property("artworkLoaded").toBool(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(backend->activeSession(), nativeHistory, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!cover->isVisible(), 5000);
    QVERIFY(!backend->messages().isEmpty());
    // A subsequent settings navigation cancels the cover and its request.
    backend->newSession();
    QVERIFY(select(nativeHistory));
    QVERIFY(cover->isVisible());
    window->setProperty("page", "settings");
    QVERIFY(!cover->property("loading").toBool());
    QTest::qWait(200);
    QVERIFY(backend->activeSession().isEmpty());
    window->setProperty("page", "chat");
  }
  void independentBackgroundChats() {
    backend->newSession();
    slow = true;
    auto start = requests.size();
    backend->send("background-first");
    QTRY_VERIFY_WITH_TIMEOUT(requests.size() > start, 15000);
    auto first = backend->activeSession();
    backend->newSession();
    backend->send("background-second");
    QTRY_VERIFY_WITH_TIMEOUT(requests.size() > start + 1, 15000);
    auto second = backend->activeSession();
    QVERIFY(first != second);
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 15000);
    QVERIFY(lastText().contains("background-second"));
    backend->openSession(first);
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 15000);
    QVERIFY(lastText().contains("background-first"));
    slow = false;
  }
  void followUpSteering() {
    backend->newSession();
    holdReplies = true;
    const auto cleanup = qScopeGuard([this] { releaseReplies(); });
    auto start = requests.size();
    backend->send("first queued fixture");
    QTRY_VERIFY_WITH_TIMEOUT(backend->running(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(requests.size() > start, 15000);
    backend->send("second queued fixture");
    QTRY_COMPARE_WITH_TIMEOUT(backend->queuedMessages().size(), 1, 5000);
    backend->send("third queued fixture");
    QTRY_COMPARE_WITH_TIMEOUT(backend->queuedMessages().size(), 2, 5000);
    QTest::qWait(200);
    window->grabWindow().save(root +
                              "/build/screenshots/queue-before-edit.png");
    click("editQueued_0");
    QTRY_COMPARE_WITH_TIMEOUT(backend->queuedMessages().size(), 1, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(find("promptInput")
                                 ->property("text")
                                 .toString()
                                 .contains("second queued fixture"),
                             5000);
    click("removeQueued_0");
    QTRY_VERIFY_WITH_TIMEOUT(backend->queuedMessages().isEmpty(), 5000);
    backend->send("second queued fixture edited");
    QTRY_COMPARE_WITH_TIMEOUT(backend->queuedMessages().size(), 1, 5000);
    click("steerQueued_0");
    QTRY_VERIFY_WITH_TIMEOUT(backend->queuedMessages().isEmpty(), 5000);
    auto inserted = [this] {
      for (auto value : backend->messages()) {
        auto message = value.toMap();
        if (message["role"] == "user" &&
            message["text"] == "second queued fixture edited")
          return true;
      }
      return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(inserted(), 5000);
    releaseReplies();
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 30000);
    QVERIFY(backend->queuedMessages().isEmpty());
    QVERIFY2(lastText().contains("second queued fixture"),
             qPrintable(lastText()));
    slow = false;
  }
  void recordOpenedUrl(QUrl url) { openedUrls << url; }
  void chatboxDelivery() {
    configureAPI();
    backend->newSession();
    mediaImage = directory.filePath("preview # image.png");
    mediaReport = directory.filePath("report.txt");
    QImage image(640, 280, QImage::Format_RGB32);
    image.fill(QColor("#d8e4ff"));
    QVERIFY(image.save(mediaImage));
    QFile report(mediaReport);
    QVERIFY(report.open(QIODevice::WriteOnly));
    report.write("synthetic report");
    report.close();
    mediaTool = true;
    mediaCalls = 0;
    const auto cleanup = qScopeGuard([this] {
      mediaTool = false;
      QDesktopServices::unsetUrlHandler("file");
    });
    backend->send("Show the image and deliver the report");
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        find("chatboxImage_media-delivery_0") &&
            find("chatboxImage_media-delivery_0")->property("loaded").toBool(),
        10000);
    QVERIFY(find("chatboxCard_media-delivery_1"));
    QTest::qWait(350); // Allow the tail-follow viewport to finish positioning.
    auto fileHeader = find("openChatbox_media-delivery_1");
    movePointer(point(fileHeader, 50, 17));
    QTRY_VERIFY_WITH_TIMEOUT(
        fileHeader->property("interactionHovered").toBool(), 1000);
    QCOMPARE(find("chatPointerArea")->property("cursorShape").toInt(),
             int(Qt::PointingHandCursor));
    QTRY_VERIFY_WITH_TIMEOUT(
        find("hoverTipContent_chatboxTip_media-delivery_1") &&
            find("hoverTipContent_chatboxTip_media-delivery_1")->isVisible(),
        2000);
    QVERIFY(find("hoverTipContent_chatboxTip_media-delivery_1")->width() <=
            336);
    QVERIFY(!find("toggleChatbox_media-delivery_1"));
    click("previewChatbox_media-delivery_0");
    auto viewer = window->findChild<QObject *>("imagePreview");
    QVERIFY(viewer && viewer->property("visible").toBool());
    const QPoint center(window->width() / 2, window->height() / 2);
    QWheelEvent wheel(QPointF(center), QPointF(window->mapToGlobal(center)),
                      QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(window, &wheel);
    QVERIFY(viewer->property("zoom").toDouble() > 1);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, center);
    QTest::mouseMove(window, center + QPoint(70, 45), 30);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                        center + QPoint(70, 45));
    QVERIFY(viewer->property("visible").toBool());
    QVERIFY(viewer->property("panX").toDouble() > 40);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center);
    QTRY_VERIFY(!viewer->property("visible").toBool());
    QDesktopServices::setUrlHandler("file", this, "recordOpenedUrl");
    click("openChatbox_media-delivery_1");
    QVERIFY(!openedUrls.isEmpty());
    QCOMPARE(U::cleanPath(openedUrls.last().toLocalFile()),
             U::cleanPath(mediaReport));
    QCOMPARE(find("chatboxCard_media-delivery_1")->height(), qreal(30));
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                      point(find("openChatbox_media-delivery_1")));
    QTest::qWait(150);
    for (const auto &prefix : {"attachChatbox_", "copyChatboxFile_",
                               "copyChatbox_", "revealChatbox_"}) {
      const auto name = QString(prefix) + "media-delivery_1";
      auto action = find(name), label = find(name + "Label");
      QVERIFY(action && label);
      QVERIFY(!action->property("glyph").toString().isEmpty());
      QVERIFY(label->width() >= label->implicitWidth() - 1);
    }
    click("copyChatboxFile_media-delivery_1");
    QVERIFY(QGuiApplication::clipboard()->mimeData()->hasUrls());
    QCOMPARE(U::cleanPath(QGuiApplication::clipboard()
                              ->mimeData()
                              ->urls()
                              .first()
                              .toLocalFile()),
             U::cleanPath(mediaReport));
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                      point(find("openChatbox_media-delivery_1")));
    QTest::qWait(150);
    click("copyChatbox_media-delivery_1");
    QCOMPARE(U::cleanPath(QGuiApplication::clipboard()->text()),
             U::cleanPath(mediaReport));
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                      point(find("openChatbox_media-delivery_1")));
    QTest::qWait(150);
    click("attachChatbox_media-delivery_1");
    QTest::qWait(120);
    QVERIFY(!backend->attachments().isEmpty());
    QVERIFY(find("promptInput")->hasActiveFocus());
    for (const auto &item : backend->attachments())
      backend->removeAttachment(item.toMap()["id"].toString());
    QMimeData dropMime;
    dropMime.setUrls({QUrl::fromLocalFile(mediaReport)});
    const QPoint dropPoint(window->width() - 90, 165);
    QDragEnterEvent enter(dropPoint, Qt::CopyAction, &dropMime, Qt::LeftButton,
                          Qt::NoModifier);
    QDragMoveEvent move(dropPoint, Qt::CopyAction, &dropMime, Qt::LeftButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(window, &enter);
    QCoreApplication::sendEvent(window, &move);
    QVERIFY(find("chatFileDropArea")->property("containsDrag").toBool());
    QDropEvent dropped(QPointF(dropPoint), Qt::CopyAction, &dropMime,
                       Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &dropped);
    QTest::qWait(120);
    QVERIFY(dropped.isAccepted());
    QVERIFY(find("promptInput")->hasActiveFocus());
    for (const auto &item : backend->attachments())
      backend->removeAttachment(item.toMap()["id"].toString());
    QVERIFY(window->grabWindow().save(root +
                                      "/build/screenshots/chatbox-media.png"));
    const auto id = backend->activeSession();
    backend->newSession();
    backend->openSession(id);
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(
        find("chatboxImage_media-delivery_0") &&
            find("chatboxImage_media-delivery_0")->property("loaded").toBool(),
        10000);
    int cards = 0;
    for (auto row : backend->displayMessages())
      for (auto step : row.toMap()["activity"].toList())
        if (step.toMap()["kind"] == "media")
          cards += step.toMap()["items"].toList().size();
    QCOMPARE(cards, 2);
  }
  void wordAndParagraphSelection() {
    configureAPI();
    backend->newSession();
    const QString paragraph = "alpha beta gamma " +
                              QString("a wrapped sentence ").repeated(14) +
                              "end";
    const QString unicode =
        QString::fromUtf8("第二段包含中文𠮷字符，三击应选中整个段落。");
    backend->send(paragraph + "\n\n" + unicode);
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 30000);
    auto view = find("conversation");
    QMetaObject::invokeMethod(view, "beginBrowsing");
    QMetaObject::invokeMethod(view, "positionViewAtBeginning");
    QTest::qWait(200);
    QQuickItem *editor = nullptr;
    QList<QQuickItem *> items{window->contentItem()};
    while (!items.isEmpty()) {
      auto item = items.takeLast();
      if (item->objectName().startsWith("selectText_") && item->isVisible() &&
          item->property("text").toString().contains("Got: alpha")) {
        editor = item;
        break;
      }
      items.append(item->childItems());
    }
    QVERIFY(editor);
    QString plain;
    QVERIFY(QMetaObject::invokeMethod(
        editor, "getText", Qt::DirectConnection, Q_RETURN_ARG(QString, plain),
        Q_ARG(int, 0), Q_ARG(int, editor->property("length").toInt())));
    auto selection = find("chatSelection");
    auto copy = [selection] {
      QMetaObject::invokeMethod(selection, "copy");
      return QGuiApplication::clipboard()->text();
    };
    auto at = [editor](int offset) {
      QRectF rect;
      QMetaObject::invokeMethod(editor, "positionToRectangle",
                                Qt::DirectConnection,
                                Q_RETURN_ARG(QRectF, rect), Q_ARG(int, offset));
      return editor
          ->mapToScene(QPointF(rect.x() + 2, rect.y() + rect.height() / 2))
          .toPoint();
    };
    auto pos = at(plain.indexOf("beta") + 1);
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, pos);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, pos);
    QCOMPARE(copy(), QString("beta"));
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, pos);
    QTest::qWait(150);
    QCOMPARE(copy(), QString("beta"));
    QVERIFY(find("copySelectedText"));
    click("copySelectedText");
    QCOMPARE(QGuiApplication::clipboard()->text(), QString("beta"));
    QVERIFY(selection->property("hasSelection").toBool());
    QMetaObject::invokeMethod(selection, "clear");
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, pos);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, pos);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, pos);
    QCOMPARE(copy(), QString("Got: ") + paragraph);
    QMetaObject::invokeMethod(selection, "clear");
    pos = at(plain.indexOf(unicode) + 8);
    QTest::mouseDClick(window, Qt::LeftButton, Qt::NoModifier, pos);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, pos);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, pos);
    QCOMPARE(copy(), unicode);
    QMetaObject::invokeMethod(selection, "clear");
    // Font metrics differ across runners. The earlier selection starts at the
    // top; explicitly expose the reply and its footer before testing hover.
    QMetaObject::invokeMethod(view, "positionViewAtEnd");
    QTest::qWait(200);
    auto actions = find("messageActions_1");
    QVERIFY(actions);
    movePointer(QPoint(40, 40));
    QTest::qWait(80);
    QCOMPARE(actions->property("opacity").toDouble(), 0.0);
    auto bubble = find("messageBubble_1");
    const auto visible =
        bubble->mapRectToScene(bubble->boundingRect())
            .intersected(view->mapRectToScene(view->boundingRect()));
    QVERIFY(!visible.isEmpty());
    movePointer(visible.center().toPoint());
    // Poll only the result: moving out/in on every retry resets the hover state
    // between QTRY's consecutive evaluations on different event backends.
    QTRY_COMPARE_WITH_TIMEOUT(actions->property("opacity").toDouble(), 1.0,
                              3000);
    QVERIFY(find("messageTime_1")->property("text").toString().contains(':'));
    click("copyMessage_1");
    QCOMPARE(QGuiApplication::clipboard()->text(),
             QString("Got: ") + paragraph + "\n\n" + unicode);
    movePointer(QPoint(40, 40));
    QTRY_COMPARE_WITH_TIMEOUT(actions->property("opacity").toDouble(), 0.0,
                              1000);
  }
  void applicationUpdates() {
    auto about = qobject_cast<AppAbout *>(backend->appAbout());
    QVERIFY(about);
    UpdateNetworkFixture network;
    auto original = about->network;
    const auto oldRelease = about->m_release, oldDownload = about->m_download;
    const auto oldDirectory = about->m_downloadDirectory;
    const bool wasMinimal = about->m_minimal;
    auto restore = qScopeGuard([&] {
      about->network = original;
      about->m_minimal = wasMinimal;
      about->m_downloadDirectory = oldDirectory;
      about->m_release = oldRelease;
      about->m_download = oldDownload;
      emit about->changed();
    });
    about->network = &network;
    about->m_minimal = false;
    about->m_downloadDirectory = directory.filePath("updates");
    QString family;
#ifdef Q_OS_WIN
    family = "windows";
#elif defined(Q_OS_MACOS)
    family = "macos";
#else
    family = "linux";
#endif
    const auto target =
        family +
        (QSysInfo::buildCpuArchitecture() == "arm64" ? "-arm64" : "-x64");
    const auto extension = family == "linux" ? ".tar.gz" : ".zip";
    const QByteArray payload(600000, 'x');
    const auto project = QString("https://github.com/umeiko/umeko-desktop-cpp");
    const auto metadata = [&](bool minimal) {
      const auto tag = QString("v99.0.0") + (minimal ? "-minimal" : "");
      const auto name = "UmekoCpp-" + tag + "-" + target + extension;
      return QJsonDocument(
                 QJsonObject{
                     {"tag_name", tag},
                     {"draft", false},
                     {"prerelease", minimal},
                     {"html_url", project + "/releases/tag/" + tag},
                     {"assets",
                      QJsonArray{QJsonObject{
                          {"name", name},
                          {"size", payload.size()},
                          {"browser_download_url",
                           project + "/releases/download/" + tag + "/" + name},
                          {"digest",
                           "sha256:" +
                               QString::fromLatin1(
                                   QCryptographicHash::hash(
                                       payload, QCryptographicHash::Sha256)
                                       .toHex())}}}}})
          .toJson();
    };
    network.add(metadata(false));
    about->checkUpdates();
    QTRY_VERIFY_WITH_TIMEOUT(!about->busy(), 2000);
    QCOMPARE(about->release()["state"].toString(), QString("available"));
    QVERIFY(find("sidebarUpdateButton")->isVisible());
    network.add(payload);
    click("sidebarUpdateButton");
    QTRY_COMPARE_WITH_TIMEOUT(about->download()["state"].toString(),
                              QString("ready"), 2000);
    QFile downloaded(about->download()["path"].toString());
    QVERIFY(downloaded.open(QIODevice::ReadOnly));
    QCOMPARE(downloaded.readAll(), payload);
    downloaded.close();
    QCOMPARE(about->download()["version"].toString(), QString("v99.0.0"));
    QCOMPARE(about->download()["sha256"].toString(),
             QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex()));
    click("sidebarUpdateButton");
    QTRY_VERIFY(find("confirmAppInstallButton"));
    QVERIFY(!about->busy());
    QVERIFY(about->installation().isEmpty());
    click("cancelAppInstallButton");
    QTRY_VERIFY(!find("confirmAppInstallButton") || !find("confirmAppInstallButton")->isVisible());
    QCOMPARE(about->download()["state"].toString(), QString("ready"));
    network.add(QByteArray(payload.size(), 'z'));
    about->downloadUpdate();
    QTRY_COMPARE_WITH_TIMEOUT(about->download()["state"].toString(),
                              QString("error"), 2000);
    QCOMPARE(QDir(directory.filePath("updates/Umeko"))
                 .entryList(QDir::Files | QDir::Hidden)
                 .size(),
             1);
    about->m_minimal = true;
    network.add(metadata(false));
    network.add(metadata(true));
    about->checkUpdates();
    QTRY_VERIFY_WITH_TIMEOUT(!about->busy(), 2000);
    QVERIFY(about->release()["asset"].toMap()["name"].toString().contains(
        "-minimal-"));
    QVERIFY(network.urls.last().path().endsWith("/v99.0.0-minimal"));
  }
  void taskCompletionEventsAreNotDuplicatedOrSentForCancellation() {
    const auto metadata = backend->store.data["sessionMeta"];
    auto restore = qScopeGuard([&] {backend->store.data["sessionMeta"] = metadata; backend->store.save();});
    QSignalSpy finished(backend.get(), &Backend::taskFinished);
    auto task = std::make_shared<Session>(); task->id = "notification-fixture"; task->title = "Local task";
    task->action = "prompt"; task->phase = "running"; task->ready = true;
    backend->event(task, {{"type","done"}}); backend->event(task, {{"type","done"}});
    QCOMPARE(finished.count(),1); QVERIFY(finished.first()[2].toBool());
    task->phase = "running"; task->cancelled = true; backend->event(task, {{"type","done"}}); QCOMPARE(finished.count(),1);
    task->cancelled = false; task->failed = true; task->phase = "running"; backend->event(task, {{"type","done"}});
    QCOMPARE(finished.count(),2); QVERIFY(!finished.last()[2].toBool());
    task->action = "reload"; task->phase = "working"; backend->event(task, {{"type","done"}}); QCOMPARE(finished.count(),2);
  }
  void commonProviderAccounts() {
    const auto original = backend->store.data;
    const auto previousPage = window->property("page");
    const auto previousTab = window->property("settingsTab");
    QStringList added;
    QDesktopServices::setUrlHandler("https", this, "recordOpenedUrl");
    auto restore = qScopeGuard([&] {
      QDesktopServices::unsetUrlHandler("https");
      for (const auto &id : added) backend->store.setSecret("provider:" + id, {});
      backend->store.data = original;
      backend->notify();
      backend->selectModel(original["selectedProvider"].toString(), original["selectedModel"].toString());
      window->setProperty("page", previousPage); window->setProperty("settingsTab", previousTab);
    });
    QCOMPARE(backend->commonProviders().size(), 6);
    window->setProperty("page", "settings"); window->setProperty("settingsTab", 0);
    QTest::qWait(100);
    find("settingsScroll")->property("contentItem").value<QObject *>()->setProperty("contentY", 0);
    const auto inside = [](QQuickItem *item, QQuickItem *section) {
      while (item) {if (item == section) return true; item = item->parentItem();}
      return false;
    };
    QVERIFY(find("commonProvidersSection")); QVERIFY(find("apiProvidersSection"));
    QCOMPARE(find("commonProvidersSection")->parentItem(), find("apiProvidersSection")->parentItem());
    QVERIFY(inside(find("addCommonProviderButton"), find("commonProvidersSection")));
    QVERIFY(!inside(find("addCommonProviderButton"), find("apiProvidersSection")));
    click("addCommonProviderButton");
    QTRY_VERIFY(find("commonProvider_kimi"));
    for (const auto &value : backend->commonProviders())
      QVERIFY(find("commonProvider_" + value.toMap()["id"].toString())->isVisible());
    for (const auto &value : backend->commonProviders()) {
      const auto preset = value.toMap();
      click("commonProvider_" + preset["id"].toString());
      QCOMPARE(find("commonProviderRegion")->isVisible(), preset.contains("international"));
      QCOMPARE(find("commonProviderRegion")->property("currentIndex").toInt(), 0);
      if (preset["id"] == "glm")
        QCOMPARE(find("commonProviderRegion")->property("currentText").toString(), QString("zcode.z.ai/cn（中国大陆）"));
      QCOMPARE(find("commonProviderKeyLink")->property("targetUrl").toString(), preset["keyUrl"].toString());
      const auto count = openedUrls.size();
      click("commonProviderKeyLink");
      QCOMPARE(openedUrls.size(), count + 1);
      QCOMPARE(openedUrls.last(), QUrl(preset["keyUrl"].toString()));
      if (preset.contains("international")) {
        click("commonProviderRegion");
        QCOMPARE(find("commonProviderRegion")->property("count").toInt(), 2);
        QTest::keyClick(window, Qt::Key_End); QTest::keyClick(window, Qt::Key_Return);
        QTest::qWait(100);
        QCOMPARE(find("commonProviderRegion")->property("currentIndex").toInt(), 1);
        if (preset["id"] == "glm")
          QCOMPARE(find("commonProviderRegion")->property("currentText").toString(), QString("z.ai（国际）"));
        const auto url = preset["international"].toMap()["keyUrl"].toString();
        QCOMPARE(find("commonProviderKeyLink")->property("targetUrl").toString(), url);
        click("commonProviderKeyLink");
        QCOMPARE(openedUrls.last(), QUrl(url));
      }
      click("commonProviderBack");
    }
    QDir().mkpath(root + "/build/screenshots");
    QVERIFY(window->grabWindow().save(root + "/build/screenshots/common-provider-menu.png"));
    click("commonProvider_kimi");
    QVERIFY(!find("confirmCommonProvider")->property("enabled").toBool());
    find("commonProviderKey")->setProperty("text", "fixture-native-key");
    find("commonProviderAlias")->setProperty("text", "Work");
    QCOMPARE(find("commonProviderNamePreview")->property("text").toString(), QString("Kimi(Work)"));
    click("confirmCommonProvider");
    auto account = backend->store.data["providers"].toList().last().toMap();
    added << account["id"].toString();
    QCOMPARE(account["name"].toString(), QString("Kimi(Work)"));
    QCOMPARE(backend->store.secret("provider:" + added.last()), QByteArray("fixture-native-key"));
    QTRY_VERIFY(find("editProvider_" + added.last()));
    QVERIFY(inside(find("editProvider_" + added.last()), find("commonProvidersSection")));
    click("editProvider_" + added.last());
    QTRY_VERIFY(inside(find("providerEditor"), find("commonProvidersSection")));
    QCOMPARE(find("providerName")->property("text").toString(), QString("Kimi(Work)"));
    click("editProvider_" + added.last());
    QVERIFY(backend->addCommonProvider("kimi", "fixture-native-key-2", ""));
    account = backend->store.data["providers"].toList().last().toMap(); added << account["id"].toString();
    QCOMPARE(account["name"].toString(), QString("Kimi(2)"));
    const auto count = backend->store.data["providers"].toList().size();
    QVERIFY(!backend->addCommonProvider("kimi", "fixture", "Work"));
    QVERIFY(!backend->addCommonProvider("kimi", "", ""));
    QCOMPARE(backend->store.data["providers"].toList().size(), count);
    QVERIFY(backend->addCommonProvider("codex", "fixture-native-codex", ""));
    account = backend->store.data["providers"].toList().last().toMap(); added << account["id"].toString();
    QCOMPARE(account["wireApi"].toString(), QString("responses"));
    QCOMPARE(backend->store.data["selectedProvider"].toString(), added.last());
    for (const auto &value : backend->commonProviders()) {
      const auto preset = value.toMap();
      if (!preset["subscription"].toBool()) continue;
      const auto id = preset["id"].toString();
      for (const auto &region : {QString("cn"), QString("international")}) {
        if (region == "international" && !preset.contains("international")) continue;
        const auto effective = region == "international" ? preset["international"].toMap() : preset;
        const auto name = effective["name"].toString(), alias = region == "international" ? QString("Global") : QString("Work");
        click("addCommonProviderButton"); click("commonProvider_" + id);
        QCOMPARE(find("commonProviderRegion")->property("currentIndex").toInt(), 0);
        if (region == "international") {
          find("commonProviderKey")->setProperty("text", "key-for-other-region");
          find("commonProviderAlias")->setProperty("text", alias);
          click("commonProviderRegion");
          QCOMPARE(find("commonProviderRegion")->property("count").toInt(), 2);
          QTest::keyClick(window, Qt::Key_End); QTest::keyClick(window, Qt::Key_Return);
          QTest::qWait(100);
          QCOMPARE(find("commonProviderRegion")->property("currentIndex").toInt(), 1);
          QCOMPARE(find("commonProviderKey")->property("text").toString(), QString());
          QCOMPARE(find("commonProviderAlias")->property("text").toString(), alias);
          QVERIFY(!find("confirmCommonProvider")->isEnabled());
        }
        QCOMPARE(find("commonProviderKeyHint")->property("text").toString(), region == "international" ? QString("填写国际站订阅 Key，即可添加使用。") : QString("填写国内站订阅 Key，即可添加使用。"));
        find("commonProviderKey")->setProperty("text", "fixture-subscription-key");
        find("commonProviderAlias")->setProperty("text", alias);
        click("confirmCommonProvider");
        account = backend->store.data["providers"].toList().last().toMap(); added << account["id"].toString();
        QCOMPARE(account["presetId"].toString(), id);
        QCOMPARE(account["presetRegion"].toString(), region);
        QCOMPARE(account["name"].toString(), name + "(" + alias + ")");
        QCOMPARE(account["baseUrl"].toString(), effective["baseUrl"].toString());
        QCOMPARE(backend->store.secret("provider:" + added.last()), QByteArray("fixture-subscription-key"));
        QVERIFY(backend->saveProvider(added.last(), account["name"].toString(), account["api"].toString(), account["baseUrl"].toString(), "", QString::fromUtf8(QJsonDocument::fromVariant(account["models"]).toJson())));
        QCOMPARE(backend->provider(added.last())["presetRegion"].toString(), region);
        QVERIFY(backend->addCommonProvider(id, "fixture-subscription-key-2", "", region));
        account = backend->store.data["providers"].toList().last().toMap(); added << account["id"].toString();
        QCOMPARE(account["name"].toString(), name + (id == "minimax" && region == "international" ? "(3)" : "(2)"));
        QCOMPARE(account["models"].toList().first().toMap()["upstream"].toString(), effective["models"].toList().first().toMap()["upstream"].toString());
      }
    }
    QVERIFY(!backend->addCommonProvider("kimi", "fixture", "", "international"));
    QVERIFY(!backend->addCommonProvider("glm", "fixture", "", "invalid-region"));
    QSet<QString> aliases;
    for (const auto &value : backend->store.data["providers"].toList())
      for (const auto &row : value.toMap()["models"].toList()) {
        const auto alias = row.toMap()["alias"].toString();
        QVERIFY(!aliases.contains(alias)); aliases.insert(alias);
      }
  }
  void outputLimitNotice() {
    configureAPI();
    window->setProperty("page", "chat");
    backend->newSession();
    limitResponse = true;
    backend->send("Long drawing fixture");
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 15000);
    QCOMPARE(requests.last().toMap()["max_tokens"].toInt(),
             backend->config()["agent"].toMap()["maxTokens"].toInt());
    QCOMPARE(backend->messages().last().toMap()["stopReason"].toString(),
             QString("length"));
    QTest::qWait(200);
    auto notice = find("outputLimitNotice_1");
    QVERIFY(notice && notice->isVisible());
    const auto before = requests.size();
    limitResponse = false;
    click("continueOutputButton");
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 15000);
    QCOMPARE(requests.size(), before + 1);
    QCOMPARE(backend->messages().size(), 4);
    QCOMPARE(backend->messages().last().toMap()["stopReason"].toString(),
             QString("stop"));
    QTest::qWait(150);
    QVERIFY(!find("outputLimitNotice_3")->isVisible());
  }
  void questionNavigator() {
    if (backend->config()["selectedModel"] != "native-fixture")
      configureAPI();
    window->setProperty("page", "chat");
    backend->newSession();
    backend->send("navigation fixture");
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 15000);
    auto model = qobject_cast<ChatModel *>(backend->messageModel());
    QVERIFY(model);
    QVariantList messages;
    for (int i = 0; i < 30; ++i) {
      messages << QVariantMap{{"role", "user"},
                              {"error", false},
                              {"text",
                               QString("Question %1 navigation marker").arg(i)}}
               << QVariantMap{
                      {"role", "assistant"},
                      {"error", false},
                      {"text", QString("Answer paragraph.\n\n").repeated(10)}};
    }
    model->update(messages);
    QTest::qWait(300);
    auto nav = find("questionNavigator"), view = find("conversation");
    QVERIFY(nav && view);
    QQmlComponent avatarComponent(
        engine.get(),
        QUrl::fromLocalFile(root + "/app/qml/AssistantAvatar.qml"));
    QVERIFY2(avatarComponent.isReady(),
             qPrintable(avatarComponent.errorString()));
    std::unique_ptr<QObject> avatar(
        avatarComponent.create(engine->rootContext()));
    QVERIFY(avatar);
    QSet<int> variants;
    for (int i = 0; i < 60; ++i) {
      avatar->setProperty("messageKey", QString("fixture/%1").arg(i));
      variants.insert(avatar->property("variant").toInt());
      QTRY_VERIFY_WITH_TIMEOUT(avatar->property("loaded").toBool(), 1000);
    }
    QCOMPARE(variants, QSet<int>({0, 1, 2}));
    movePointer(point(view, 40, 40));
    QTest::qWait(300);
    QVERIFY(!nav->property("expanded").toBool());
    QVERIFY(!find("conversationScrollBar")->isVisible());
    movePointer(point(nav));
    QTRY_VERIFY_WITH_TIMEOUT(nav->property("expanded").toBool(), 1000);
    auto listing = find("questionNavigationList");
    QVERIFY(listing);
    QMetaObject::invokeMethod(listing, "positionViewAtBeginning");
    QTest::qWait(150);
    click("questionNavigationItem_0");
    QTest::qWait(200);
    QCOMPARE(nav->property("activeQuestion").toInt(), 0);
    QVERIFY(!view->property("followTail").toBool());
    auto bubble = find("messageBubble_0");
    QVERIFY(bubble);
    QVERIFY(qAbs(bubble->mapToItem(view, QPointF()).y()) < 4);
    auto header = find("assistantAvatar_1");
    QVERIFY(header && header->property("loaded").toBool());
    const auto source = header->property("source");
    auto last = messages.last().toMap();
    last["text"] = last["text"].toString() + " Streamed";
    messages.last() = last;
    model->update(messages);
    QCOMPARE(header->property("source"), source);
    QMetaObject::invokeMethod(view, "positionViewAtEnd");
    QTest::qWait(150);
    QMetaObject::invokeMethod(listing, "positionViewAtBeginning");
    QTest::qWait(100);
    click("questionNavigationItem_0");
    QTest::qWait(200);
    header = find("assistantAvatar_1");
    QVERIFY(header && header->property("loaded").toBool());
    QCOMPARE(header->property("source"), source);
    QTest::mouseMove(window, point(view, 40, 40));
    QTest::qWait(350);
    QVERIFY(!nav->property("expanded").toBool());
    backend->newSession();
    QVERIFY(model->questions().isEmpty());
  }
  void customThemesAndReplyMascot() {
    const auto original = backend->config()["theme"].toString();
    const auto file = directory.filePath("moon.umeko-theme.json");
    QVariantMap theme{
        {"format", "umeko-theme"},
        {"version", 1},
        {"id", "moon"},
        {"name", "月夜"},
        {"base", "dark"},
        {"colors", QVariantMap{{"bg", "#252033"}, {"buttonText", "#FFFFFF"}}}};
    QVERIFY(U::writeJson(file, theme));
    QVERIFY(backend->importTheme(QUrl::fromLocalFile(file).toString()));
    QTest::qWait(200);
    QCOMPARE(backend->config()["theme"].toString(), QString("custom:moon"));
    QVERIFY(backend->config()["dark"].toBool());
    QCOMPARE(window->color(), QColor("#252033"));
    QCOMPARE(backend->customThemes().size(), 1);
    QVERIFY(backend->importTheme(file));
    QCOMPARE(backend->customThemes().size(), 1);
    Store saved(directory.path());
    QCOMPARE(saved.data["theme"].toString(), QString("custom:moon"));
    theme["colors"] = QVariantMap{{"bg", "not a color"}};
    QVERIFY(U::writeJson(file, theme));
    QVERIFY(!backend->importTheme(file));
    QCOMPARE(backend->config()["theme"].toString(), QString("custom:moon"));
    backend->copyThemePrompt();
    QVERIFY(QGuiApplication::clipboard()->text().contains("buttonText"));
    QVERIFY(QGuiApplication::clipboard()->text().contains("umeko-theme"));
    backend->removeTheme("custom:moon");
    QVERIFY(backend->customThemes().isEmpty());
    QCOMPARE(backend->config()["theme"].toString(), QString("dark"));
    backend->setTheme("yanami");
    backend->newSession();
    backend->send("reply mascot fixture");
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 30000);
    QTest::qWait(300);
    auto mascot = find("replyEndMascot_1");
    QVERIFY(mascot && mascot->property("loaded").toBool());
    for (int i = 0; i < 3; ++i)
      click("replyEndMascot_1");
    QVERIFY(mascot->property("animating").toBool());
    QVERIFY(mascot->property("reaction").toString().contains("饼干"));
    backend->setTheme(original);
  }
  void networkRetryStatus() {
    if (backend->config()["selectedModel"] != "native-fixture")
      configureAPI();
    window->setProperty("page", "chat");
    backend->newSession();
    auto before = requests.size();
    transientFailures = 1;
    backend->send("retry fixture");
    QTRY_VERIFY_WITH_TIMEOUT(backend->networkRetry()["waiting"].toBool(),
                             15000);
    auto label = find("networkRetryStatus");
    QVERIFY(label && label->isVisible());
    QVERIFY(label->property("text").toString().contains("第一次"));
    QSignalSpy changed(backend.get(), &Backend::chatChanged);
    QTest::qWait(1400);
    QCOMPARE(changed.size(), 0);
    QVERIFY(label->property("text").toString().contains("1s/2s"));
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 15000);
    QCOMPARE(requests.size() - before, 2);
    QVERIFY(backend->networkRetry().isEmpty());
    backend->newSession();
    transientFailures = 1;
    backend->send("cancel retry fixture");
    QTRY_VERIFY_WITH_TIMEOUT(backend->networkRetry()["waiting"].toBool(),
                             15000);
    before = requests.size();
    backend->stop();
    QVERIFY(backend->networkRetry().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 1500);
    QCOMPARE(requests.size(), before);
  }
  void reasoningEffortRequests() {
    if (backend->config()["selectedModel"] != "native-fixture")
      configureAPI();
    window->setProperty("page", "chat");
    for (const auto &level : QStringList{"off", "low", "high", "max", "off"}) {
      QVERIFY(backend->setReasoningEffort(level));
      backend->newSession();
      QCOMPARE(backend->reasoningEffort(), level);
      const auto before = requests.size();
      backend->send("reasoning fixture " + level);
      QTRY_VERIFY_WITH_TIMEOUT(requests.size() > before, 15000);
      QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 15000);
      const auto request = requests.last().toMap();
      if (level == "off")
        QVERIFY(!request.contains("reasoning_effort"));
      else
        QCOMPARE(request["reasoning_effort"].toString(), level);
    }
    QVERIFY(!backend->setReasoningEffort("invalid"));
    QCOMPARE(backend->reasoningEffort(), QString("off"));
    click("modelSelector");
    QTest::qWait(800);
    auto hint = window->findChild<QObject *>("reasoningEffortHint");
    QVERIFY(hint);
    QTest::mouseMove(window, point(find("modelSearch")));
    QTest::qWait(200);
    QVERIFY(!hint->property("visible").toBool());
    QTest::mouseMove(window, point(find("reasoningControls")));
    QTRY_VERIFY_WITH_TIMEOUT(hint->property("visible").toBool(), 2000);
    QTest::mouseMove(window, point(find("modelSearch")));
    QTRY_VERIFY_WITH_TIMEOUT(!hint->property("visible").toBool(), 2000);
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::qWait(850);
  }
  void appearanceAndRequestDetails() {
    window->setProperty("page", "settings");
    window->setProperty("settingsTab", 0);
    QTest::qWait(250);
    const auto editorId = backend->config()["selectedProvider"].toString();
    click("editProvider_" + editorId);
    QVERIFY(find("settingsPage")->property("editingProvider").toBool());
    QCOMPARE(find("editProvider_" + editorId)->property("glyph").toString(),
             QString("chevron-up"));
    click("editProvider_" + editorId);
    QVERIFY(!find("settingsPage")->property("editingProvider").toBool());
    click("providerProxy_" + editorId);
    QCOMPARE(find("providerNetworkTitle")->property("text").toString(),
             QString("网络设置"));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY_WITH_TIMEOUT(!find("providerNetworkTitle") ||
                                 !find("providerNetworkTitle")->isVisible(),
                             3000);
    window->setProperty("page", "settings");
    window->setProperty("settingsTab", 5);
    backend->setLanguage("en");
    backend->setFontScale(2.0);
    QTest::qWait(250);
    QCOMPARE(find("addProviderButton")->property("text").toString(),
             QString("Add provider"));
    QCOMPARE(find("promptInput")->property("font").value<QFont>().pixelSize(),
             26);
    QCOMPARE(find("sendGlyph")->width(), 34.0);
    QCOMPARE(find("attachFilesButton")->width(), 62.0);
    backend->setFontScale(.5);
    QTest::qWait(200);
    QCOMPARE(find("promptInput")->property("font").value<QFont>().pixelSize(),
             7);
    backend->setFontScale(1);
    backend->setLanguage("zh");
    window->setProperty("page", "chat");
    const auto provider = backend->config()["selectedProvider"].toString();
    QVERIFY(
        backend->setProviderProxy(provider, "custom", "http://127.0.0.1:7890"));
    QVERIFY(backend->setProviderProxy(provider, "direct", ""));
    upstreamError = true;
    backend->newSession();
    backend->send("request failure fixture");
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 30000);
    upstreamError = false;
    QVERIFY(find("requestDetails_1"));
    click("requestDetails_1");
    QVERIFY(find("requestDetailsText")->isVisible());
    click("copyRequestDetails");
    auto report =
        QJsonDocument::fromJson(QGuiApplication::clipboard()->text().toUtf8())
            .object();
    QVERIFY(report["request"].toObject()["messages"].isArray());
    QCOMPARE(report["response"].toObject()["status"].toInt(), 400);
    QVERIFY(report["response"].toObject()["body"].toString().contains(
        "complete upstream error details"));
    QTest::keyClick(window, Qt::Key_Escape);
    QVERIFY(backend->setProviderProxy(provider, "system", ""));
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
    QVERIFY(!manager->pythonExecutable().isEmpty());
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
  void providerPackageReinstallation() {
    auto manager = qobject_cast<Providers *>(backend->providerPlugins());
    const auto archive = directory.filePath("reinstall.zip");
    const auto updated = directory.filePath("reinstall-updated.zip");
    const auto newer = directory.filePath("reinstall-newer.zip");
    QVERIFY(package(archive, false, false, "example-reinstall"));
    QVERIFY(package(updated, false, true, "example-reinstall"));
    QVERIFY(package(newer, false, false, "example-reinstall", "2.0.0"));
    auto install = [&](QString file) {
      if (!manager->importZip(QUrl::fromLocalFile(file).toString()))
        return QString();
      return manager->rows().last().toMap()["id"].toString();
    };
    const auto first = install(archive), second = install(archive),
               otherVersion = install(newer);
    QVERIFY(!first.isEmpty() && !second.isEmpty() && !otherVersion.isEmpty());
    const auto folder = backend->store.directory +
                        "/provider-plugins/packages/example-reinstall/";
    QVERIFY(!manager->importZip(QUrl::fromLocalFile(updated).toString()));
    manager->remove(first);
    QVERIFY(QFileInfo::exists(folder + "0.1.0/manifest.json"));
    QVERIFY(!manager->importZip(QUrl::fromLocalFile(updated).toString()));
    manager->remove(second);
    QVERIFY(QFileInfo::exists(folder + "0.1.0/manifest.json"));
    QVERIFY(!manager->importZip(QUrl::fromLocalFile(updated).toString()));
    QVERIFY(manager->addAccount("example-reinstall", "0.1.0"));
    QVERIFY(manager->uninstall("example-reinstall", "0.1.0"));
    QVERIFY(!QFileInfo::exists(folder + "0.1.0"));
    QVERIFY(QFileInfo::exists(folder + "2.0.0/manifest.json"));
    const auto replacement = install(updated);
    QVERIFY(!replacement.isEmpty());
    manager->login(replacement, "{\"username\":\"reinstall-fixture\"}");
    QTRY_VERIFY_WITH_TIMEOUT(!manager->rows().last().toMap()["busy"].toBool(),
                             15000);
    const auto models = manager->rows().last().toMap()["models"].toList();
    QVERIFY(!models.isEmpty());
    backend->newSession();
    backend->selectModel(replacement,
                         models.first().toMap()["alias"].toString());
    backend->send("reinstall fixture");
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 30000);
    QVERIFY2(lastText().contains("Reinstalled: reinstall fixture"),
             qPrintable(lastText()));

    // Simulate an old release that removed the account but retained its
    // package.
    auto rows = backend->store.data["providers"].toList();
    for (int i = rows.size() - 1; i >= 0; --i)
      if (rows[i].toMap()["id"] == replacement)
        rows.removeAt(i);
    backend->store.data["providers"] = rows;
    QVERIFY(backend->store.save());
    bool listed = false;
    for (const auto &value : manager->packages()) {
      const auto p = value.toMap();
      if (p["id"] == "example-reinstall" && p["version"] == "0.1.0") {
        listed = true;
        QCOMPARE(p["accountCount"].toInt(), 0);
      }
    }
    QVERIFY(listed);
    QVERIFY(!manager->importZip(QUrl::fromLocalFile(archive).toString()));
    QVERIFY(manager->uninstall("example-reinstall", "0.1.0"));
    const auto recovered = install(archive);
    QVERIFY(!recovered.isEmpty());
    QFile code(folder + "0.1.0/scripts/provider.py");
    QVERIFY(code.open(QIODevice::ReadOnly));
    QVERIFY(!code.readAll().contains("Reinstalled:"));
    code.close();
    manager->remove(recovered);
    manager->remove(otherVersion);
    QVERIFY(manager->uninstall("example-reinstall", "0.1.0"));
    QVERIFY(manager->uninstall("example-reinstall", "2.0.0"));
    QVERIFY(!QFileInfo::exists(folder + "0.1.0"));
    QVERIFY(!QFileInfo::exists(folder + "2.0.0"));
  }
  void providerZipDiagnostics() {
    auto manager = qobject_cast<Providers *>(backend->providerPlugins());
    const auto archive = directory.filePath("dot-paths.zip");
    QVERIFY(
        package(archive, false, false, "example-dots", "0.1.0", "./provider/"));
    QVERIFY(manager->importZip(QUrl::fromLocalFile(archive).toString()));
    QVERIFY(manager->importError().isEmpty());
    const auto id = manager->rows().last().toMap()["id"].toString();
    manager->login(id, "{\"username\":\"dot-fixture\"}");
    QTRY_VERIFY_WITH_TIMEOUT(!manager->rows().last().toMap()["busy"].toBool(),
                             15000);
    QVERIFY2(!manager->rows().last().toMap()["models"].toList().isEmpty(),
             qPrintable(manager->rows().last().toMap()["status"].toString()));
    QVERIFY(manager->uninstall("example-dots", "0.1.0"));
    const QList<QPair<QStringList, QString>> cases{
        {{"./scripts/../bad.py"}, "PARENT_PATH"},
        {{"/bad.py"}, "ABSOLUTE_PATH"},
        {{"C:/bad.py"}, "COLON"},
        {{"scripts\\bad.py"}, "BACKSLASH"},
        {{"scripts//bad.py"}, "EMPTY_SEGMENT"},
        {{"CON.txt"}, "RESERVED_NAME"},
        {{"scripts/a.py", "./scripts/a.py"}, "DUPLICATE_PATH"},
        {{"scripts", "./scripts/a.py"}, "PATH_TYPE_CONFLICT"},
        {{"./scripts/link.py"}, "SYMLINK"}};
    for (const auto &fixture : cases) {
      mz_zip_archive zip{};
      QVERIFY(mz_zip_writer_init_file(&zip, archive.toUtf8().constData(), 0));
      QHash<QByteArray, QByteArray> names;
      for (const auto &name : fixture.first) {
        const auto original = name.toUtf8();
        const auto safe = QByteArray::number(names.size())
                              .rightJustified(original.size(), 'x');
        names[safe] = original;
        // The writer refuses some unsafe paths. Patch same-length names in
        // the headers afterward to exercise the importer's validation.
        QVERIFY(mz_zip_writer_add_mem(&zip, safe.constData(), "fixture", 7,
                                      MZ_DEFAULT_COMPRESSION));
      }
      QVERIFY(mz_zip_writer_finalize_archive(&zip));
      mz_zip_writer_end(&zip);
      QFile file(archive);
      QVERIFY(file.open(QIODevice::ReadWrite));
      auto raw = file.readAll();
      for (auto it = names.cbegin(); it != names.cend(); ++it)
        raw.replace(it.key(), it.value());
      if (fixture.second == "SYMLINK") {
        const auto central = raw.indexOf(QByteArray("PK\x01\x02", 4));
        QVERIFY(central >= 0);
        const quint32 mode = 0120777u << 16;
        for (int i = 0; i < 4; ++i)
          raw[central + 38 + i] = char(mode >> (8 * i));
      }
      QVERIFY(file.seek(0));
      QCOMPARE(file.write(raw), raw.size());
      file.close();
      QVERIFY(!manager->importZip(QUrl::fromLocalFile(archive).toString()));
      QCOMPARE(manager->importError()["entry"].toString(),
               fixture.first.last());
      QCOMPARE(manager->importError()["rule"].toString(), fixture.second);
      if (fixture.first.size() > 1)
        QCOMPARE(manager->importError()["conflict"].toString(),
                 fixture.first.first());
    }
  }
  void extensionAndMcpManagement() {
    auto manager = qobject_cast<PiManagement *>(backend->piManagement());
    QVERIFY(manager);
    manager->setRecoveryMode(false);
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), 15000);
    QString mcpId;
    for (auto v : manager->extensions())
      if (v.toMap()["path"] == "builtin:mcp")
        mcpId = v.toMap()["id"].toString();
    QVERIFY2(!mcpId.isEmpty(), qPrintable(manager->status()));
    manager->setExtensionEnabled(mcpId, false);
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), 15000);
    bool disabled = false;
    for (auto v : manager->extensions())
      if (v.toMap()["id"] == mcpId)
        disabled = !v.toMap()["enabled"].toBool();
    QVERIFY(disabled);
    manager->setExtensionEnabled(mcpId, true);
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), 15000);
    backend->saveMcp("global", "{\"mcpServers\":{}}");
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 15000);
    QCOMPARE(QJsonDocument::fromJson(backend->readMcp("global").toUtf8())
                 .object()["mcpServers"]
                 .toObject()
                 .size(),
             0);
    auto before = backend->readMcp("global");
    backend->saveMcp("global", "{\"mcpServers\":[]}");
    QCOMPARE(backend->readMcp("global"), before);
  }
  void proxyProviderSubmenusAndAutomaticAddresses() {
    auto original = backend->store.data;
    const auto previousPage = window->property("page"), previousTab = window->property("settingsTab");
    auto restore = qScopeGuard([&] {
      backend->store.data = original; backend->invalidate(); backend->notify();
      window->setProperty("page",previousPage); window->setProperty("settingsTab",previousTab);
    });
    auto account = [](QString id, QString name, QString model) {
      return QVariantMap{{"id",id},{"name",name},{"api","openai"},{"baseUrl","http://127.0.0.1:18181/v1"},
        {"models",QVariantList{U::normalizeModel({{"upstream",model},{"alias",model}})}}};
    };
    backend->store.data["providers"] = QVariantList{account("group-a","Provider A","model-a"),account("group-b","Provider B","model-b")};
    auto server = backend->store.data["server"].toMap(); server["lan"] = false; backend->store.data["server"] = server;
    backend->notify(); window->setProperty("page","settings"); window->setProperty("settingsTab",1); QTest::qWait(200);
    auto revealAndClick = [&](QString name) {
      auto item = find(name); QVERIFY(item);
      auto flick = qobject_cast<QQuickItem*>(find("settingsScroll")->property("contentItem").value<QObject*>()); QVERIFY(flick);
      const auto y = flick->property("contentY").toReal() + item->mapToItem(flick,QPointF()).y() - flick->height()/2;
      flick->setProperty("contentY",qBound(0.,y,qMax(0.,flick->property("contentHeight").toReal()-flick->height())));
      QTest::qWait(100); click(name);
    };
    QVERIFY(!find("proxyAddressPicker"));
    QCOMPARE(find("proxyOpenaiBaseUrl")->property("text").toString(),QString("http://127.0.0.1:%1/v1").arg(server["port"].toInt()));
    QVERIFY(!find("mappingModelId_0")); QVERIFY(!find("mappingModelId_1"));
    revealAndClick("mappingProvider_group-a");
    QVERIFY(find("mappingModelId_0")->isVisible()); QVERIFY(!find("mappingModelId_1"));
    auto alias = find("mappingAlias_0"); alias->setProperty("text","renamed-a"); QMetaObject::invokeMethod(alias,"textEdited");
    revealAndClick("backToMappingProviders"); revealAndClick("mappingProvider_group-b");
    QVERIFY(!find("mappingModelId_0")); QVERIFY(find("mappingModelId_1")->isVisible());
    revealAndClick("backToMappingProviders"); revealAndClick("mappingProvider_group-a");
    QCOMPARE(find("mappingAlias_0")->property("text").toString(),QString("renamed-a"));
    revealAndClick("saveMappingsButton");
    const auto saved = backend->store.data["providers"].toList();
    QCOMPARE(saved[0].toMap()["models"].toList()[0].toMap()["alias"].toString(),QString("renamed-a"));
    QCOMPARE(saved[1].toMap()["models"].toList()[0].toMap()["alias"].toString(),QString("model-b"));
    server["lan"] = true; backend->store.data["server"] = server; emit backend->changed(); QTest::qWait(100);
    const auto addresses = backend->proxyAddresses();
    const auto base = addresses.size() > 1 ? QString("http://%1:%2").arg(addresses[1]).arg(server["port"].toInt()) : QString();
    QCOMPARE(find("proxyAnthropicBaseUrl")->property("text").toString(),base);
    if (!base.isEmpty()) {
      revealAndClick("copyProxyOpenaiBaseUrl"); QCOMPARE(QGuiApplication::clipboard()->text(),base+"/v1");
    }
  }
  void proxyLifecycleAndRollback() {
    QTcpServer freePort, blocked;
    QVERIFY(freePort.listen(QHostAddress::LocalHost, 0));
    auto port = freePort.serverPort();
    freePort.close();
    QVERIFY(backend->saveProxyKey("", "native-key-fixture", ""));
    auto issued = backend->takeCreatedProxyKey();
    QVERIFY(issued["key"].toString().size() >= 16);
    QVERIFY(backend->takeCreatedProxyKey().isEmpty());
    QVERIFY(backend->saveServer(port, true, true, false));
    backend->toggleServer();
    QTRY_VERIFY_WITH_TIMEOUT(backend->serverRunning(), 10000);
    QVERIFY(blocked.listen(QHostAddress::LocalHost, 0));
    QVERIFY(backend->saveServer(blocked.serverPort(), true, false, false));
    QTRY_COMPARE_WITH_TIMEOUT(
        backend->config()["server"].toMap()["port"].toInt(), int(port), 10000);
    QVERIFY(backend->serverRunning());
    backend->toggleServer();
    QTRY_VERIFY_WITH_TIMEOUT(!backend->serverRunning(), 10000);
    backend->revokeProxyKey(issued["id"].toString());
    QCOMPARE(backend->proxyKeys().size(), 0);
  }
  void npmMirrorAndKernelCheck() {
    QTcpServer registry;
    QVERIFY(registry.listen(QHostAddress::LocalHost, 0));
    int pingCalls = 0, metadataCalls = 0;
    connect(&registry, &QTcpServer::newConnection, this, [&] {
      while (registry.hasPendingConnections()) {
        auto socket = registry.nextPendingConnection();
        auto request = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, socket, [&, socket, request] {
          *request += socket->readAll();
          if (!request->contains("\r\n\r\n"))
            return;
          QByteArray body = "{}";
          if (request->startsWith("GET /-/ping"))
            ++pingCalls;
          else {
            ++metadataCalls;
            body =
                R"({"name":"@earendil-works/pi-coding-agent","dist-tags":{"latest":"1.3.0"},"versions":{"1.3.0":{"name":"@earendil-works/pi-coding-agent","version":"1.3.0"}}})";
          }
          socket->write(
              "HTTP/1.1 200 OK\r\nContent-Type: "
              "application/json\r\nConnection: close\r\nContent-Length: " +
              QByteArray::number(body.size()) + "\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
      }
    });
    auto manager = qobject_cast<PiManagement *>(backend->piManagement());
    QVERIFY(manager);
    QFile npmrc(directory.filePath("test.npmrc"));
    QVERIFY(npmrc.open(QIODevice::WriteOnly));
    npmrc.close();
    QVariantMap values{
        {"npmRegistry",
         QString("http://127.0.0.1:%1/").arg(registry.serverPort())},
        {"npmUserconfig", npmrc.fileName()}};
    auto raw = QString::fromUtf8(QJsonDocument::fromVariant(values).toJson());
    QVERIFY(manager->saveNpmConfig(raw));
    QCOMPARE(manager->npmConfig()["npmRegistry"], values["npmRegistry"]);
    manager->testNpmConfig();
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), 30000);
    QVERIFY2(manager->npmStatus().contains("连接正常"),
             qPrintable(manager->npmStatus()));
    QVERIFY(pingCalls > 0);
    backend->checkKernel();
    QTRY_VERIFY_WITH_TIMEOUT(!backend->kernelBusy(), 30000);
    QVERIFY2(backend->kernelStatus().contains("最新"),
             qPrintable(backend->kernelStatus()));
    QVERIFY(metadataCalls > 0);
    QVERIFY(!manager->saveNpmConfig(
        "{\"npmRegistry\":\"https://user:secret@example.invalid/\"}"));
    QVERIFY(manager->saveNpmConfig("{}"));
  }
  void cleanup() {
    if (QTest::currentTestFailed() && window) {
      auto selection = find("chatSelection");
      qInfo() << "Window active:" << window->isActive()
              << "window geometry:" << window->geometry()
              << "system cursor:" << QCursor::pos();
      if (selection)
        qInfo() << "Chat pointer:" << selection->property("pointer")
                << "hovering:" << selection->property("hovering")
                << "dragging:" << selection->property("dragging");
      const auto screenshots = root + "/build/screenshots";
      QDir().mkpath(screenshots);
      window->grabWindow().save(screenshots + "/failed-" +
                                QTest::currentTestFunction() + ".png");
    }
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
