#include "backend.hpp"
#include "miniz.h"
#include "syntaxhighlighter.hpp"
#include <QDesktopServices>
#include <QQuickStyle>
#include <QScopeGuard>
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
          if (upstreamError) {
            const auto raw =
                QJsonDocument(
                    QJsonObject{{"error", "complete upstream error details"}})
                    .toJson(QJsonDocument::Compact);
            socket->write("HTTP/1.1 429 Too Many Requests\r\nContent-Type: "
                          "application/json\r\nContent-Length: " +
                          QByteArray::number(raw.size()) +
                          "\r\nConnection: close\r\n\r\n" + raw);
            socket->disconnectFromHost();
            return;
          }
          QVariantMap message;
          QString finish = "stop";
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
               QJsonObject{{"prompt_tokens", 100}, {"completion_tokens", 10}}}};
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
    QTest::mouseMove(window, point(fileHeader, 50, 17));
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
    click("toggleChatbox_media-delivery_1");
    auto fileMenu = find("revealChatbox_media-delivery_1")->parent();
    QVERIFY(fileMenu->property("visible").toBool());
    click("toggleChatbox_media-delivery_1");
    QVERIFY(!fileMenu->property("visible").toBool());
    QDesktopServices::setUrlHandler("file", this, "recordOpenedUrl");
    click("openChatbox_media-delivery_1");
    QVERIFY(!openedUrls.isEmpty());
    QCOMPARE(U::cleanPath(openedUrls.last().toLocalFile()),
             U::cleanPath(mediaReport));
    QCOMPARE(find("chatboxCard_media-delivery_1")->height(), qreal(34));
    QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                      point(find("openChatbox_media-delivery_1")));
    QTest::qWait(150);
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
    QTest::mouseMove(window, QPoint(40, 40));
    QTest::qWait(80);
    QCOMPARE(actions->property("opacity").toDouble(), 0.0);
    int hoverAttempts = 0;
    auto hoverReply = [this, view, &hoverAttempts] {
      auto bubble = find("messageBubble_1");
      const auto visible =
          bubble->mapRectToScene(bubble->boundingRect())
              .intersected(view->mapRectToScene(view->boundingRect()));
      if (visible.isEmpty())
        return false;
      // Cocoa can suppress repeated synthetic moves at identical coordinates.
      // Move one pixel between retries so geometry changes get a fresh event.
      QTest::mouseMove(window, visible.center().toPoint() +
                                   QPoint(++hoverAttempts % 2, 0));
      return find("messageActions_1")->property("opacity").toDouble() == 1.0;
    };
    QTRY_VERIFY_WITH_TIMEOUT(hoverReply(), 3000);
    QVERIFY(find("messageTime_1")->property("text").toString().contains(':'));
    click("copyMessage_1");
    QCOMPARE(QGuiApplication::clipboard()->text(),
             QString("Got: ") + paragraph + "\n\n" + unicode);
    QTest::mouseMove(window, QPoint(40, 40));
    QTRY_COMPARE_WITH_TIMEOUT(actions->property("opacity").toDouble(), 0.0,
                              1000);
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
  void appearanceAndRequestDetails() {
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
    QCOMPARE(report["response"].toObject()["status"].toInt(), 429);
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
  void cleanupTestCase() {
    backend->close();
    engine.reset();
    backend.reset();
    QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
  }
};
QTEST_MAIN(NativeUiTests)
#include "ui_test.moc"
