#include "terminal_view.hpp"
#include "widget_channel.hpp"
#include "desktop_pet.hpp"
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
  bool desktopTool = false;
  int desktopCalls = 0;
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
    // Cocoa needs the real cursor, while offscreen tests must stay in their
    // synthetic window coordinates (the runner's screen geometry can differ).
    if (QGuiApplication::platformName() != "offscreen") {
      QCursor::setPos(window->mapToGlobal(position));
      QTest::qWait(80);
    }
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
    movePointer(point(item));
    QTest::qWait(40);
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
               QString prefix = {}, QByteArray implementation = {},
               QVariant description = {}) {
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
          if (description.isValid())
            manifest["description"] = QJsonValue::fromVariant(description);
          else
            manifest.remove("description");
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
        if (!implementation.isEmpty() && name == "scripts/provider.py")
          raw = implementation;
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
    qmlRegisterType<TerminalView>("Umeko.Terminal",1,0,"TerminalView");
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
          if (desktopTool && desktopCalls++ == 0) {
            QVariantMap function{{"name", "umeko_desktop"}, {"arguments", "{\"operation\":\"appearance.fontScale\",\"arguments\":{\"scale\":1.1}}"}};
            message = {{"role", "assistant"}, {"content", QVariant()},
                       {"tool_calls", QVariantList{QVariantMap{{"id", "desktop-control"}, {"type", "function"}, {"function", function}}}}};
            finish = "tool_calls";
          } else if (mediaTool && mediaCalls++ == 0) {
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
  void subagentNativeConversations() {
    const auto oldSize = window->size();
    backend->newSession();
    auto record = backend->view;
    QProcess pipe;
    pipe.setProgram(U::node());
    pipe.setArguments(
        {"-e", "require('node:readline').createInterface({input:process.stdin})"
               ".on('line',line=>process.stdout.write(line+'\\n'))"});
    pipe.start();
    QVERIFY(pipe.waitForStarted());
    record->process = &pipe;
    record->ready = true;
    record->phase = "running";
    record->action = "prompt";
    auto reset = qScopeGuard([&] {
      backend->children->closeView();
      record->process = nullptr;
      record->phase = "idle";
      record->subagentsActive = false;
      pipe.kill();
      pipe.waitForFinished(2000);
      backend->newSession();
      window->resize(oldSize);
    });
    window->setProperty("page", "chat");
    window->resize(1700, 1050);
    QVariantList tools;
    for (int i = 0; i < 7; ++i) {
      QVariantMap child{
          {"id", "native-child-" + QString::number(i)},
          {"status", "running"},
          {"task", "A long multiline task\nthat must remain on one line"},
          {"avatarSlot", i % 4}};
      backend->children->merge(record, child);
      tools << QVariantMap{{"id", "subagent:" + child["id"].toString()},
                           {"name", "subagent"},
                           {"subagent", child},
                           {"state", "running"}};
    }
    record->messages = {QVariantMap{
        {"role", "assistant"}, {"text", "Delegated work"}, {"tools", tools}}};
    emit backend->chatChanged();
    QTRY_VERIFY(find("subagentGroupCard"));
    QVERIFY(!find("subagentCard_native-child-0"));
    auto matrix = window->findChild<QObject *>("subagentMatrix");
    QVERIFY(matrix);
    click("subagentGroupCard");
    QTRY_VERIFY(matrix->property("desiredOpen").toBool());
    click("subagentGroupCard");
    QTRY_VERIFY(!matrix->property("desiredOpen").toBool());
    QCOMPARE(backend->children->activeCount(), 7);
    QVERIFY(backend->children->agents()[0].toMap()["name"].toString().endsWith(
        " · 1"));
    backend->children->open("native-child-0");
    backend->event(
        record,
        {{"type", "subagent_context"},
         {"id", "native-child-0"},
         {"messages",
          QVariantList{
              QVariantMap{{"role", "user"}, {"text", "Check this task"}},
              QVariantMap{{"role", "assistant"}, {"text", "Working"}}}}});
    QTRY_VERIFY(find("subagentPromptInput"));
    QTRY_VERIFY(!find("webPane")->property("moving").toBool());
    QTest::qWait(100);
    auto prompt = find("subagentPromptInput");
    prompt->setProperty("text", "queued child request");
    prompt->forceActiveFocus();
    pipe.readAllStandardOutput();
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(pipe.bytesAvailable() > 0);
    auto lines = pipe.readAllStandardOutput().trimmed().split('\n');
    auto sent = QJsonDocument::fromJson(lines.last()).object().toVariantMap();
    QCOMPARE(sent["id"].toString(), QString("native-child-0"));
    QCOMPARE(sent["mode"].toString(), QString("followUp"));
    backend->event(record, {{"type", "subagent_command"},
                            {"requestId", sent["requestId"]},
                            {"ok", true}});
    QVariantMap queued{{"id", sent["messageId"]},
                       {"text", sent["text"]},
                       {"mode", "followUp"}};
    backend->event(record, {{"type", "subagent_queue"},
                            {"id", "native-child-0"},
                            {"items", QVariantList{queued}}});
    QTRY_VERIFY(find("subagentQueuedMessages")->height() > 0);
    auto band = find("subagentQueuedMessages"),
         composer = find("subagentComposer");
    QVERIFY(qAbs(band->y() + band->height() - composer->y()) < 1);
    prompt->setProperty("text", "priority child request");
    prompt->forceActiveFocus();
    QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
    QTRY_VERIFY(pipe.bytesAvailable() > 0);
    sent = QJsonDocument::fromJson(
               pipe.readAllStandardOutput().trimmed().split('\n').last())
               .object()
               .toVariantMap();
    QCOMPARE(sent["mode"].toString(), QString("interrupt"));
    QCOMPARE(sent["id"].toString(), QString("native-child-0"));
    QVERIFY(record->queue.isEmpty());
    backend->event(record, {{"type", "subagent_command"},
                            {"requestId", sent["requestId"]},
                            {"ok", true}});
    const auto file = directory.filePath("child-attachment.txt");
    QFile fixture(file);
    QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write("attachment");
    fixture.close();
    QVERIFY(backend->children->addAttachments(QString::fromUtf8(
        QJsonDocument(QJsonArray{QUrl::fromLocalFile(file).toString()})
            .toJson())));
    backend->children->setDraft("private child draft");
    backend->children->open("native-child-1");
    QVERIFY(backend->children->context()["attachments"].toList().isEmpty());
    QVERIFY(backend->children->queuedMessages().isEmpty());
    backend->children->open("native-child-0");
    QCOMPARE(backend->children->context()["draft"].toString(),
             QString("private child draft"));
    QCOMPARE(backend->children->context()["attachments"].toList().size(), 1);
    backend->event(record, {{"type", "subagent_queue_returned"},
                            {"id", "native-child-0"},
                            {"items", QVariantList{queued}}});
    QVERIFY(backend->children->context()["draft"].toString().contains(
        "queued child request"));
    backend->children->merge(
        record, {{"id", "native-child-0"}, {"status", "completed"}});
    backend->event(record,
                   {{"type", "subagent_context"},
                    {"id", "native-child-0"},
                    {"messages", QVariantList{QVariantMap{{"role", "assistant"},
                                                          {"text", "Done"}}}}});
    QTRY_VERIFY(find("subagentReplyMascot_0"));
    QCOMPARE(find("subagentReplyMascot_0")->width(), qreal(56));
    click("subagentReplyMascot_0");
    QVERIFY(!find("subagentReplyMascot_0")
                 ->property("reaction")
                 .toString()
                 .isEmpty());
    backend->children->processExit(record);
    QCOMPARE(backend->children->activeCount(),0);
    QVERIFY(backend->children->queuedMessages().isEmpty());
    const auto out = root + "/build/screenshots";
    QDir().mkpath(out);
    window->grabWindow().save(out + "/subagent-native-queue.png");
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
    auto motion = find("sidebarWidthMotion"), sidebar = find("sidebar");
    QVERIFY(motion && sidebar);
    QTRY_VERIFY_WITH_TIMEOUT(!motion->property("moving").toBool(), 4000);
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTest::qWait(100);
    QVERIFY(backend->sidebarCollapsed());
    QVERIFY(motion->property("moving").toBool());
    QVERIFY(sidebar->width() > 0 && sidebar->width() < 310);
    QTRY_VERIFY_WITH_TIMEOUT(!motion->property("moving").toBool(), 4000);
    QCOMPARE(sidebar->width(), qreal(0));
    QVERIFY(!sidebar->isVisible());
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTest::qWait(100);
    QVERIFY(!backend->sidebarCollapsed());
    QVERIFY(motion->property("moving").toBool());
    QVERIFY(sidebar->width() > 0 && sidebar->width() < 310);
    // Reverse twice while moving; the latest target must win without snapping.
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTest::qWait(25);
    QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
    QTRY_VERIFY_WITH_TIMEOUT(!motion->property("moving").toBool(), 4000);
    QCOMPARE(sidebar->width(), qreal(310));
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
    QTRY_VERIFY_WITH_TIMEOUT(!motion->property("moving").toBool(), 4000);
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
    const auto fixture=directory.filePath("site.config.ts");
    QFile source(fixture);QVERIFY(source.open(QIODevice::WriteOnly));source.write("export default { title: 'Preview fixture' };\n");source.close();
    auto message=backend->view->messages[1].toMap();auto tools=message["tools"].toList();auto tool=tools[1].toMap();tool["path"]=fixture;tools[1]=tool;message["tools"]=tools;backend->view->messages[1]=message;emit backend->chatChanged();
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
    QTRY_COMPARE(find("webPane")->property("mode").toString(),QString("document"));
    QCOMPARE(find("webPane")->property("documentPath").toString(),fixture);
    QMetaObject::invokeMethod(find("webPane"),"dismiss");
    QTRY_VERIFY(!find("webPane")->property("moving").toBool());
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
  void migratedProjectHistoryStaysUniqueAndClickable() {
    QTemporaryDir temporary;
    const auto workspace = temporary.filePath("workspace");
    QDir().mkpath(workspace);
    const auto history = temporary.filePath("history.jsonl");
    QFile log(history);
    QVERIFY(log.open(QIODevice::WriteOnly));
    const auto header = QJsonObject{{"type", "session"}, {"version", 3}, {"id", "moved-chat"}, {"cwd", workspace}};
    const auto entry = QJsonObject{{"type", "message"}, {"id", "user-entry"},
      {"message", QJsonObject{{"role", "user"}, {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "Preserved migrated question"}}}}}}};
    log.write(QJsonDocument(header).toJson(QJsonDocument::Compact) + '\n');
    log.write(QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n');
    log.close();
    Store seed(temporary.path());
    seed.data["workspace"] = workspace;
    seed.data["activeProject"] = "old-id";
    seed.data["projects"] = QVariantList{
      QVariantMap{{"id", "old-id"}, {"path", workspace}, {"name", "My workspace"}},
      QVariantMap{{"id", "duplicate-id"}, {"path", workspace}, {"name", "workspace"}}};
    QVERIFY(seed.save());
    QVERIFY(seed.saveSessions(QVariantList{QVariantMap{{"id", "moved-chat"}, {"file", history}, {"workspace", workspace}, {"title", "Preserved migrated question"}}}));
    Backend restored(root, temporary.path());
    QQmlApplicationEngine restoredEngine;
    restoredEngine.rootContext()->setContextProperty("backend", &restored);
    restoredEngine.load(QUrl::fromLocalFile(root + "/app/qml/Main.qml"));
    QVERIFY(!restoredEngine.rootObjects().isEmpty());
    auto restoredWindow = qobject_cast<QQuickWindow*>(restoredEngine.rootObjects().first());
    QVERIFY(restoredWindow);
    restoredWindow->requestActivate();
    QTest::qWait(400);
    QCOMPARE(restored.projectRows().size(), 2);
    auto item = [&](QString name) -> QQuickItem* {
      QList<QQuickItem*> pending{restoredWindow->contentItem()};
      while (!pending.isEmpty()) {
        auto next = pending.takeLast();
        if (next->objectName() == name) return next;
        pending.append(next->childItems());
      }
      return nullptr;
    };
    auto clickRestored = [&](QString name) -> bool {
      auto target = item(name);
      if (!target || !target->isVisible()) return false;
      QTest::mouseClick(restoredWindow, Qt::LeftButton, Qt::NoModifier,
          target->mapToScene(QPointF(target->width()/2, target->height()/2)).toPoint());
      return true;
    };
    for (int attempt = 0; attempt < 2; ++attempt) {
      QVERIFY(clickRestored("sidebarRow_moved-chat"));
      QTRY_COMPARE_WITH_TIMEOUT(restored.activeSession(), QString("moved-chat"), 5000);
      QVERIFY(!restored.messages().isEmpty());
      QCOMPARE(restored.messages().first().toMap()["text"].toString(), QString("Preserved migrated question"));
      QTRY_VERIFY(!restoredWindow->property("sidebarSwitching").toBool());
      QVERIFY(clickRestored("newChatButton"));
      QTRY_VERIFY(restored.activeSession().isEmpty());
    }
    restored.close();
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
    mediaReport = directory.filePath("report.pdf");
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
  void previewLayoutAndModelFocus() {
    backend->newSession();
    const auto previous = window->size();
    auto pane = find("webPane");
    const auto cleanup = qScopeGuard([&] { pane->setProperty("opened", false); window->resize(previous); });
    backend->saveSidebarLayout(244, false);
    window->resize(1060, 700);
    pane->setProperty("mode", "document"); pane->setProperty("opened", true);
    QTRY_VERIFY_WITH_TIMEOUT(!pane->property("moving").toBool(), 4000);
    QTRY_VERIFY_WITH_TIMEOUT(find("chatPage")->width() >= 639, 4000);
    QVERIFY(pane->width() >= 319);
    QVERIFY(window->property("previewSidebarCollapsed").toBool());
    QVERIFY(!backend->sidebarCollapsed());
    auto surface = find("webSurfaceLoader");
    QVERIFY(surface->x() >= 6);
    QVERIFY(pane->width() - surface->x() - surface->width() >= 11);
    pane->setProperty("opened", false);
    QTRY_VERIFY_WITH_TIMEOUT(!pane->property("moving").toBool(), 4000);
    QVERIFY(!window->property("previewSidebarCollapsed").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(find("mainArea")->x() > 100, 4000);
    window->resize(1320, 860);
    click("modelSelector");
    QTRY_VERIFY_WITH_TIMEOUT(find("modelSearch") && find("modelSearch")->hasActiveFocus(), 3000);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY_WITH_TIMEOUT(find("promptInput")->hasActiveFocus(), 400);
    QTest::keyClick(window, Qt::Key_B);
    QVERIFY(find("promptInput")->property("text").toString().endsWith('b', Qt::CaseInsensitive));
    QTest::qWait(1100);
    QVERIFY(find("promptInput")->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_C);
    QVERIFY(find("promptInput")->property("text").toString().endsWith("bc", Qt::CaseInsensitive));
    find("promptInput")->setProperty("text", "");
  }
  void managementJobsAndAliasCapabilities() {
    backend->newSession(); configureAPI();
    auto report = [](QVariant value) { return QString::fromUtf8(QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact)); };
    auto call = [this](QString operation, QVariantMap args = {}) { return backend->umekoRequest({{"operation", operation}, {"arguments", args}}); };
    auto job = [&](QString id) { return call("jobs.get", {{"id", id}})["jobs"].toList().first().toMap(); };
    auto scan = call("extensions.scan");
    QVERIFY2(scan["ok"].toBool(), qPrintable(report(scan)));
    const auto scanId = scan["jobId"].toString(); QVERIFY(!scanId.isEmpty());
    QCOMPARE(job(scanId)["status"].toString(), QString("running"));
    QTRY_COMPARE_WITH_TIMEOUT(job(scanId)["status"].toString(), QString("done"), 15000);
    QVERIFY(job(scanId)["result"].toMap().contains("extensions"));
    auto bad = call("extensions.enable", {{"id", "missing-extension"}, {"enabled", true}});
    const auto badId = bad["jobId"].toString(); QVERIFY(!badId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(job(badId)["status"].toString(), QString("failed"), 15000);
    QVERIFY(!job(badId)["result"].toMap()["error"].toString().isEmpty());
    auto rejected = call("mcp.add", {{"text", ""}});
    QVERIFY(!rejected["ok"].toBool());
    QCOMPARE(job(rejected["jobId"].toString())["status"].toString(), QString("failed"));
    auto refresh = call("agent.refresh");
    const auto refreshId = refresh["jobId"].toString(); QVERIFY(!refreshId.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(job(refreshId)["status"].toString(), QString("done"), 15000);
    QVERIFY(job(refreshId)["result"].toMap().contains("resources"));
    backend->kernelUpdating = true; backend->send("busy probe");
    QVERIFY(!backend->lastSendError().isEmpty());
    QCOMPARE(call("status.get")["lastSendError"].toString(), backend->lastSendError());
    backend->kernelUpdating = false;
    const QVariantList models{QVariantMap{{"upstream", "shared"}, {"alias", "quick"}, {"vision", false}, {"visionAuto", false}, {"contextWindow", 32000}, {"contextWindowAuto", false}}, QVariantMap{{"upstream", "shared"}, {"alias", "visual"}, {"vision", true}, {"visionAuto", false}, {"contextWindow", 64000}, {"contextWindowAuto", false}}};
    QVERIFY(backend->saveProvider({}, "Alias fixture", "openai", "https://api.example.com/v1", "fixture", report(models)));
    const auto providerId = backend->config()["providers"].toList().last().toMap()["id"].toString();
    const QVariantList minimal{QVariantMap{{"upstream", "shared"}, {"alias", "quick"}}, QVariantMap{{"upstream", "shared"}, {"alias", "visual"}}};
    QVERIFY(backend->saveProvider(providerId, "Alias fixture", "openai", "https://api.example.com/v1", {}, report(minimal)));
    auto saved = backend->provider(providerId)["models"].toList();
    QCOMPARE(saved[0].toMap()["contextWindow"].toInt(), 32000); QCOMPARE(saved[1].toMap()["contextWindow"].toInt(), 64000);
    QVERIFY(!saved[0].toMap()["vision"].toBool()); QVERIFY(saved[1].toMap()["vision"].toBool());
    QVariantList mappings;
    for (auto value : backend->config()["providers"].toList()) for (auto model : value.toMap()["models"].toList()) { auto m = model.toMap(); m["provider"] = value.toMap()["id"]; mappings << m; }
    QVERIFY(backend->saveMappings(report(mappings)));
    saved = backend->provider(providerId)["models"].toList();
    QCOMPARE(saved[1].toMap()["contextWindow"].toInt(), 64000);
    auto duplicate = mappings; auto row = duplicate.last().toMap(); row["alias"] = "quick"; duplicate.last() = row;
    auto invalid = call("models.mappings", {{"models", duplicate}});
    QVERIFY(!invalid["ok"].toBool()); QVERIFY(invalid["error"].toString().contains("别名"));
    QCOMPARE(backend->provider(providerId)["models"].toList(), saved);
    backend->removeProvider(providerId);
    const auto previousAgent = backend->store.data["agent"];
    auto agent = previousAgent.toMap();
    agent["fields"] = QVariantList{QVariantMap{{"key", "username"}, {"secret", false}, {"value", "alice"}}, QVariantMap{{"key", "password"}, {"secret", true}, {"value", "private-fixture"}}};
    backend->store.data["agent"] = agent;
    const auto fields = call("settings.get")["config"].toMap()["agent"].toMap()["fields"].toList();
    backend->store.data["agent"] = previousAgent;
    QCOMPARE(fields[0].toMap()["key"].toString(), QString("username")); QCOMPARE(fields[0].toMap()["value"].toString(), QString("alice"));
    QCOMPARE(fields[1].toMap()["key"].toString(), QString("password")); QCOMPARE(fields[1].toMap()["value"].toString(), QString("[redacted]"));
    const auto archive = directory.filePath("job-provider.zip");
    QVERIFY(package(archive, false, false, "example-job"));
    auto imported = call("plugins.import", {{"path", archive}});
    QVERIFY(imported["ok"].toBool());
    const auto account = imported["createdProviderIds"].toStringList().first();
    auto login = call("plugins.login", {{"id", account}, {"fields", QVariantMap{{"username", "fixture"}, {"password", "fixture-password"}}}});
    QVERIFY2(login["ok"].toBool(), qPrintable(report(login)));
    QTRY_COMPARE_WITH_TIMEOUT(job(login["jobId"].toString())["status"].toString(), QString("done"), 15000);
    QVERIFY(!backend->provider(account)["models"].toList().isEmpty());
    auto modelsJob = call("plugins.refreshModels", {{"id", account}});
    QTRY_COMPARE_WITH_TIMEOUT(job(modelsJob["jobId"].toString())["status"].toString(), QString("done"), 15000);
    QVERIFY(!call("plugins.uninstall", {{"plugin", account}, {"version", "0.1.0"}})["ok"].toBool());
    QVERIFY(call("plugins.uninstall", {{"plugin", "example-job"}, {"version", "0.1.0"}})["ok"].toBool());
  }
  void documentDeliveryAndResize() {
    const auto previousSize = window->size();
    const auto restoreSize = qScopeGuard([&] { window->resize(previousSize); });
    window->resize(1560, 900);
    backend->newSession();
    const auto previousUrl = qgetenv("UMEKO_WEB_PREVIEW_URL"), previousDocument = qgetenv("UMEKO_DOCUMENT_PREVIEW_PATH");
    const auto cleanup = qScopeGuard([&] {
      if (previousUrl.isNull()) qunsetenv("UMEKO_WEB_PREVIEW_URL"); else qputenv("UMEKO_WEB_PREVIEW_URL", previousUrl);
      if (previousDocument.isNull()) qunsetenv("UMEKO_DOCUMENT_PREVIEW_PATH"); else qputenv("UMEKO_DOCUMENT_PREVIEW_PATH", previousDocument);
      backend->newSession();
    });
    qunsetenv("UMEKO_WEB_PREVIEW_URL");
    const auto markdown = directory.filePath("artifact.md"), code = directory.filePath("artifact.py");
    { QFile file(markdown); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("# Preview\n\n**Markdown**\n"); }
    { QFile file(code); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("def hello():\n    return 42\n"); }
    auto reader = qobject_cast<DocumentPreview *>(backend->documentPreview());
    auto pane = find("webPane");
    qputenv("UMEKO_DOCUMENT_PREVIEW_PATH", markdown.toUtf8()); backend->injectDemo();
    QTRY_VERIFY_WITH_TIMEOUT(!reader->state()["html"].toString().isEmpty(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!pane->property("moving").toBool(), 4000);
    QCOMPARE(pane->property("mode").toString(), QString("document"));
    QVERIFY(find("documentPreviewText"));
    qputenv("UMEKO_DOCUMENT_PREVIEW_PATH", code.toUtf8()); backend->injectDemo();
    QTRY_COMPARE(reader->state()["kind"].toString(), QString("code"));
    auto editor = find("documentPreviewText");
    auto colors = editor->property("syntaxColors").value<QObject *>();
    QVERIFY(colors); QTRY_VERIFY(colors->property("colored").toBool());
    click("copyDocumentPreviewButton");
    QCOMPARE(QGuiApplication::clipboard()->text(), QString("def hello():\n    return 42\n"));
    auto handle = find("webPaneResizeHandle");
    const auto start = point(handle, handle->width() / 2, 100);
    const auto width = pane->width();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start);
    for (int delta : {30, 60, 100}) { QTest::mouseMove(window, start - QPoint(delta, 0)); QTest::qWait(40); }
    QVERIFY(handle->property("pressed").toBool()); QVERIFY(pane->width() > width + 90);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, start - QPoint(100, 0));
    const auto enlarged = pane->width();
    click("closeWebPaneButton"); QTRY_VERIFY(!pane->property("moving").toBool());
    QVERIFY(reader->state().isEmpty());
    click("openChatbox_web-preview_0"); QTRY_VERIFY(!reader->state()["text"].toString().isEmpty());
    QTRY_VERIFY(!pane->property("moving").toBool()); QCOMPARE(pane->width(), enlarged);
    backend->newSession(); QVERIFY(!pane->property("opened").toBool()); QVERIFY(reader->state().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!pane->property("moving").toBool(), 4000);
  }
  void settingsDismissesArtifactPreview() {
    backend->newSession();
    const auto previousUrl = qgetenv("UMEKO_WEB_PREVIEW_URL"), previousDocument = qgetenv("UMEKO_DOCUMENT_PREVIEW_PATH");
    const auto cleanup = qScopeGuard([&] {
      window->setProperty("page", "chat");
      if (previousUrl.isNull()) qunsetenv("UMEKO_WEB_PREVIEW_URL"); else qputenv("UMEKO_WEB_PREVIEW_URL", previousUrl);
      if (previousDocument.isNull()) qunsetenv("UMEKO_DOCUMENT_PREVIEW_PATH"); else qputenv("UMEKO_DOCUMENT_PREVIEW_PATH", previousDocument);
      backend->newSession();
    });
    qunsetenv("UMEKO_WEB_PREVIEW_URL");
    qputenv("UMEKO_DOCUMENT_PREVIEW_PATH", (root + "/tests/fixtures/preview/readme.md").toUtf8());
    backend->injectDemo();
    auto reader = qobject_cast<DocumentPreview *>(backend->documentPreview());
    auto pane = find("webPane");
    auto handle = find("webPaneResizeHandle");
    window->setProperty("webPreferredWidth", 420);
    for (int entry = 0; entry < 3; ++entry) {
      QTRY_VERIFY_WITH_TIMEOUT(!reader->state()["html"].toString().isEmpty(), 5000);
      QTRY_VERIFY_WITH_TIMEOUT(!pane->property("moving").toBool(), 4000);
      QVERIFY(pane->isVisible());
      const auto width = pane->width();
      if (entry == 0) click("settingsButton");
      else if (entry == 1) QTest::keyClick(window, Qt::Key_Comma, Qt::ControlModifier);
      else window->setProperty("page", "settings");
      QCOMPARE(window->property("page").toString(), QString("settings"));
      QVERIFY(!pane->property("opened").toBool());
      QVERIFY(!pane->isVisible());
      QVERIFY(!handle->isVisible());
      QVERIFY(reader->state().isEmpty());
      QTRY_VERIFY(!find("documentSurfaceLoader")->property("item").value<QObject *>());
      QTRY_VERIFY_WITH_TIMEOUT(!pane->property("moving").toBool(), 4000);
      QCOMPARE(pane->width(), 0.0);
      QCOMPARE(find("settingsPage")->width(), find("mainArea")->width());
      if (entry == 0)
        QVERIFY(window->grabWindow().save(root + "/build/screenshots/settings-preview-dismissed.png"));
      QTest::keyClick(window, Qt::Key_Escape);
      QCOMPARE(window->property("page").toString(), QString("chat"));
      QVERIFY(!pane->property("opened").toBool());
      QVERIFY(!pane->isVisible());
      click("openChatbox_web-preview_0");
      QTRY_VERIFY_WITH_TIMEOUT(!pane->property("moving").toBool(), 4000);
      QCOMPARE(pane->width(), width);
    }
  }
  void deliveryHoverClearsAfterLeavingAndClickingElsewhere() {
    backend->newSession();
    const auto previousUrl = qgetenv("UMEKO_WEB_PREVIEW_URL"), previousDocument = qgetenv("UMEKO_DOCUMENT_PREVIEW_PATH");
    const auto cleanup = qScopeGuard([&] {
      if (previousUrl.isNull()) qunsetenv("UMEKO_WEB_PREVIEW_URL"); else qputenv("UMEKO_WEB_PREVIEW_URL", previousUrl);
      if (previousDocument.isNull()) qunsetenv("UMEKO_DOCUMENT_PREVIEW_PATH"); else qputenv("UMEKO_DOCUMENT_PREVIEW_PATH", previousDocument);
      backend->newSession();
    });
    qunsetenv("UMEKO_WEB_PREVIEW_URL");
    QVariantList paths;
    for (const auto &name : {"notes.txt", "example.py", "readme.md"})
      paths << root + "/tests/fixtures/preview/" + name;
    qputenv("UMEKO_DOCUMENT_PREVIEW_PATH", QJsonDocument::fromVariant(paths).toJson(QJsonDocument::Compact));
    backend->injectDemo();
    QTRY_VERIFY_WITH_TIMEOUT(!find("webPane")->property("moving").toBool(), 4000);
    QTest::qWait(300);
    for (int index = 0; index < 3; ++index) {
      const auto key = "web-preview_" + QString::number(index);
      auto header = find("openChatbox_" + key);
      QVERIFY(header);
      movePointer(point(header, 50, 15));
      QTRY_VERIFY(header->property("interactionHovered").toBool());
      QTRY_VERIFY_WITH_TIMEOUT(find("hoverTipContent_chatboxTip_" + key) && find("hoverTipContent_chatboxTip_" + key)->isVisible(), 2000);
      QPointer<QQuickItem> tip = find("hoverTipContent_chatboxTip_" + key);
      click("openChatbox_" + key);
      movePointer(point(find("promptInput"), 30, 15));
      QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point(find("promptInput"), 30, 15));
      for (int other = 0; other < 3; ++other) {
        auto target = find("openChatbox_web-preview_" + QString::number(other));
        QTRY_VERIFY_WITH_TIMEOUT(!target->property("interactionHovered").toBool(), 1000);
      }
      QTRY_VERIFY_WITH_TIMEOUT(!tip || !tip->isVisible(), 1000);
    }
  }
  void webCardContextActions() {
    backend->newSession();
    const auto previous = qgetenv("UMEKO_WEB_PREVIEW_URL");
    const auto restore = qScopeGuard([&] {
      desktopTool = false;
      if (previous.isNull()) qunsetenv("UMEKO_WEB_PREVIEW_URL");
      else qputenv("UMEKO_WEB_PREVIEW_URL", previous);
      QDesktopServices::unsetUrlHandler("https");
      backend->newSession();
    });
    const QString url = "https://example.com/review?q=1#section";
    qputenv("UMEKO_WEB_PREVIEW_URL", url.toUtf8());
    backend->injectDemo();
    QTRY_VERIFY(find("openChatbox_web-preview_0"));
    QTRY_VERIFY_WITH_TIMEOUT(!find("webPane")->property("moving").toBool(), 4000);
    QTest::qWait(150);
    auto prompt = find("promptInput");
    prompt->setProperty("text", "Existing draft");
    const auto menu = [this] {
      QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier,
                        point(find("openChatbox_web-preview_0")));
      QTest::qWait(180);
    };
    menu();
    for (auto prefix : {"openChatboxWeb_", "copyChatboxUrl_", "attachChatboxWeb_"}) {
      auto action = find(QString(prefix) + "web-preview_0");
      QVERIFY(action && action->isVisible());
    }
    for (auto prefix : {"attachChatbox_", "copyChatboxFile_", "copyChatbox_", "revealChatbox_"})
      QVERIFY(!find(QString(prefix) + "web-preview_0")->isVisible());
    window->grabWindow().save(root + "/build/screenshots/webview-context-menu.png");
    click("copyChatboxUrl_web-preview_0");
    QCOMPARE(QGuiApplication::clipboard()->text(), url);
    QDesktopServices::setUrlHandler("https", this, "recordOpenedUrl");
    const auto count = openedUrls.size();
    menu(); click("openChatboxWeb_web-preview_0");
    QCOMPARE(openedUrls.size(), count + 1);
    QCOMPARE(openedUrls.last(), QUrl(url));
    menu(); click("attachChatboxWeb_web-preview_0");
    QCOMPARE(backend->attachments().size(), 1);
    QCOMPARE(backend->attachments().first().toMap()["url"].toString(), url);
    QCOMPARE(prompt->property("text").toString(), QString("Existing draft"));
    QVERIFY(prompt->hasActiveFocus());
    backend->attachChatboxUrl(url);
    backend->attachChatboxUrl("javascript:alert(1)");
    QCOMPARE(backend->attachments().size(), 1);
    QVERIFY(!find("webPane")->property("opened").toBool());
  }
  void clipboardPasteFormats() {
    window->setProperty("page", "chat");
    backend->newSession();
    const auto clearAttachments = [this] {
      for (const auto &value : backend->attachments())
        backend->removeAttachment(value.toMap()["id"].toString());
    };
    const auto cleanup = qScopeGuard(clearAttachments);
    clearAttachments();
    auto prompt = find("promptInput");
    QVERIFY(prompt);
    const QString code = QString::fromUtf8("def hello():\n\treturn \"中文𠮷 <tag>\"\n");
    const auto filePath = directory.filePath("copied file.txt");
    { QFile file(filePath); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("synthetic fixture"); }
    const auto url = QUrl::fromLocalFile(filePath);
    QImage image(8, 8, QImage::Format_RGB32);
    image.fill(0x668877);
    const auto paste = [this, prompt](QMimeData *mime) {
      prompt->setProperty("text", "");
      QGuiApplication::clipboard()->setMimeData(mime);
      prompt->forceActiveFocus();
      QTest::keyClick(window, Qt::Key_V, Qt::ControlModifier);
      QTest::qWait(40);
    };
    for (int format = 0; format < 6; ++format) {
      auto mime = new QMimeData;
      mime->setText(code);
      if (format == 1) {
        mime->setHtml("<pre><span style='color:red'>IDE code</span></pre>");
        mime->setData("text/rtf", "{\\rtf1\\ansi synthetic fixture}");
        mime->setData("application/x-java-jvm-local-objectref", "fixture");
      } else if (format == 2) {
        mime->setData("text/uri-list", QByteArray());
        QVERIFY(mime->hasUrls() && mime->urls().isEmpty());
      } else if (format == 3) {
        mime->setUrls({QUrl("https://example.invalid/source.py")});
      } else if (format == 4) {
        mime->setUrls({url});
      } else if (format == 5) {
        mime->setImageData(image);
      }
      paste(mime);
      QCOMPARE(prompt->property("text").toString(), code);
      QVERIFY(backend->attachments().isEmpty());
    }
    for (int format = 0; format < 3; ++format) {
      auto mime = new QMimeData;
      mime->setUrls({url});
      if (format == 1) mime->setText(url.toString());
      if (format == 2) mime->setText(QDir::toNativeSeparators(filePath));
      paste(mime);
      QCOMPARE(prompt->property("text").toString(), QString());
      QCOMPARE(backend->attachments().size(), 1);
      QCOMPARE(backend->attachments().first().toMap()["path"].toString(), U::cleanPath(filePath));
    }
    clearAttachments();
    auto screenshot = new QMimeData;
    screenshot->setImageData(image);
    paste(screenshot);
    QCOMPARE(prompt->property("text").toString(), QString());
    QCOMPARE(backend->attachments().size(), 1);
    QCOMPARE(backend->attachments().first().toMap()["kind"].toString(), QString("image"));
    clearAttachments();
    for (const auto &urls : QList<QList<QUrl>>{
             {}, {QUrl("https://example.invalid/source.py")},
             {QUrl::fromLocalFile(directory.filePath("missing.txt"))}}) {
      auto mime = new QMimeData;
      mime->setUrls(urls);
      QGuiApplication::clipboard()->setMimeData(mime);
      QVERIFY(!backend->pasteAttachments());
      QVERIFY(backend->attachments().isEmpty());
    }
    for (int i = 0; i < 20; ++i) {
      const auto path = directory.filePath(QString("occupied-%1.txt").arg(i));
      { QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture"); }
      QVERIFY(backend->addAttachments(QString::fromUtf8(QJsonDocument(QJsonArray{QUrl::fromLocalFile(path).toString()}).toJson())));
    }
    auto mime = new QMimeData;
    mime->setUrls({url});
    QGuiApplication::clipboard()->setMimeData(mime);
    QVERIFY(!backend->pasteAttachments());
    QCOMPARE(backend->attachments().size(), 20);
    prompt->setProperty("text", "");
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
  void hoverHintsPassThroughClicks() {
    QQmlComponent component(engine.get(), QUrl::fromLocalFile(root + "/tests/fixtures/tooltip-click.qml"));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    auto fixture = qobject_cast<QQuickItem *>(component.create(engine->rootContext()));
    QVERIFY(fixture);
    const auto release = qScopeGuard([&] { fixture->setParentItem(nullptr); fixture->deleteLater(); QTest::qWait(200); });
    fixture->setParentItem(window->contentItem()); fixture->setZ(500);
    QTest::qWait(750);
    auto button = fixture->findChild<QQuickItem *>("tooltipClickTarget");
    auto hint = fixture->findChild<QQuickItem *>("overlappingHoverTip");
    QVERIFY(button && hint && hint->isVisible());
    auto content = hint->property("contentItem").value<QQuickItem *>();
    QVERIFY(content);
    const auto center = content->mapToItem(fixture, content->boundingRect().center());
    button->setPosition(center - QPointF(button->width() / 2, button->height() / 2));
    QTest::qWait(50);
    const auto position = point(button);
    QVERIFY(content->contains(content->mapFromScene(position)));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, position);
    QTRY_COMPARE_WITH_TIMEOUT(fixture->property("clicks").toInt(), 1, 1000);
  }
  void umekoDesktopControl() {
    configureAPI();
    const auto savedAgent = backend->store.data["agent"];
    const auto savedTheme = backend->store.data["theme"].toString();
    const auto restore = qScopeGuard([&] {
      releaseReplies();
      backend->setFontScale(1);
      backend->setLanguage("zh");
      backend->setTheme(savedTheme);
      backend->store.data["agent"] = savedAgent;
      backend->store.save();
    });
    auto call = [&](QString operation, QVariantMap args = {}, QString caller = {}) {
      return backend->umekoRequest({{"operation", operation}, {"arguments", args}}, caller);
    };
    const auto report = [](QVariant value) { return QString::fromUtf8(QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact)); };
    auto catalog = call("operations.list")["operations"].toMap();
    QVERIFY(catalog.size() > 50);
    for (auto it = catalog.cbegin(); it != catalog.cend(); ++it) {
      if(it.key().startsWith("widgets."))continue; // Widget dispatch validates its public operation schema.
      const auto spec = it.value().toMap();
      const auto name = spec["target"].toString();
      QObject *target = name == "plugins" ? backend->providerPlugins() : name == "management" ? backend->piManagement() : name == "about" ? backend->appAbout() : backend.get();
      bool found = false;
      for (int i = 0; i < target->metaObject()->methodCount(); ++i) {
        const auto method = target->metaObject()->method(i);
        if (method.name() == spec["method"].toString().toUtf8() && method.parameterCount() == spec["args"].toList().size()) found = true;
      }
      QVERIFY2(found, qPrintable(it.key()));
    }
    QVERIFY(QFileInfo::exists(call("help")["entry"].toString()));
    QVERIFY(call("settings.get")["config"].toMap()["agent"].toMap()["maxTokens"].toInt() > 0);
    QVERIFY(call("appearance.fontScale", {{"scale", 1.25}})["ok"].toBool());
    QCOMPARE(backend->fontScale(), 1.25);
    QVERIFY(call("appearance.language", {{"language", "en"}})["ok"].toBool());
    QCOMPARE(backend->language(), QString("en"));
    QVERIFY(!call("appearance.fontScale", {{"scale", true}})["ok"].toBool());
    QVERIFY(!call("appearance.language", {{"language", "xx"}})["ok"].toBool());
    QVERIFY(!call("__getattribute__")["ok"].toBool());
    QVERIFY(!call("sessions.delete", {{"id", "missing"}})["ok"].toBool());
    const auto file = directory.filePath("control-theme.json");
    QVERIFY(U::writeJson(file, QVariantMap{{"format", "umeko-theme"}, {"version", 1}, {"id", "control"}, {"name", "Control"}, {"base", "dark"}, {"colors", QVariantMap{{"accent", "#9CAFFF"}}}}));
    QVERIFY(call("themes.import", {{"path", file}})["ok"].toBool());
    QCOMPARE(backend->config()["theme"].toString(), QString("custom:control"));
    desktopTool = true; desktopCalls = 0;
    backend->newSession();
    backend->send("apply a desktop setting with the real Agent tool");
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 15000);
    QCOMPARE(backend->fontScale(), 1.1);
    QVERIFY(desktopCalls >= 2);
    desktopTool = false;
    backend->newSession();
    const auto owner = backend->view;
    holdReplies = true;
    backend->send("desktop control fixture");
    QTRY_VERIFY_WITH_TIMEOUT(!heldReplies.isEmpty(), 15000);
    auto added = call("providers.save", {{"name", "Control API"}, {"api", "openai"}, {"baseUrl", "https://api.example.com/v1"}, {"key", "fixture-control-key"},
                                       {"models", QVariantList{QVariantMap{{"upstream", "control-model"}, {"alias", "control-model"}}}}}, owner->key);
    QVERIFY2(added["ok"].toBool(), qPrintable(report(added)));
    const auto provider = added["createdProviderIds"].toStringList().value(0);
    QVERIFY(!provider.isEmpty());
    QVERIFY(!report(call("providers.list")).contains("fixture-control-key"));
    const auto ticket = call("agent.save", {{"config", QVariantMap{{"instructions", "CONTROL_AFTER_IDLE"}}}}, owner->key);
    QCOMPARE(ticket["status"].toString(), QString("queued"));
    auto model = call("models.select", {{"provider", provider}, {"model", "control-model"}}, owner->key);
    QCOMPARE(model["status"].toString(), QString("queued"));
    backend->newSession();
    const auto viewer = backend->view;
    QVERIFY(call("sessions.reasoning", {{"value", "high"}}, owner->key)["ok"].toBool());
    QCOMPARE(owner->reasoningEffort, QString("high"));
    QCOMPARE(viewer->reasoningEffort, QString("off"));
    releaseReplies();
    QTRY_VERIFY_WITH_TIMEOUT(!owner->busy(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!backend->controlTimer.isActive(), 5000);
    for (const auto &id : {ticket["jobId"].toString(), model["jobId"].toString()}) {
      const auto jobs = call("jobs.get", {{"id", id}})["jobs"].toList();
      QCOMPARE(jobs.size(), 1);
      QVERIFY2(jobs.first().toMap()["status"] == "done", qPrintable(report(jobs.first())));
    }
    QCOMPARE(backend->store.data["agent"].toMap()["instructions"].toString(), QString("CONTROL_AFTER_IDLE"));
    QCOMPARE(owner->provider, provider);
    QCOMPARE(backend->view, viewer);
    const auto exported = directory.filePath("control-export.jsonl");
    QVERIFY(call("sessions.export", {{"id", owner->id}, {"path", exported}})["ok"].toBool());
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(exported), 10000);
    QVERIFY(call("sessions.fork", {{"id", owner->id}})["ok"].toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!backend->activeSession().isEmpty() && backend->activeSession() != owner->id, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 15000);
    const auto fork = backend->activeSession();
    QVERIFY(call("sessions.delete", {{"id", fork}})["ok"].toBool());
    QVERIFY(!call("sessions.get", {{"id", fork}})["ok"].toBool());
    const auto archive = directory.filePath("control-provider.zip");
    QVERIFY(package(archive, false, false, "example-control"));
    const auto imported = call("plugins.import", {{"path", archive}});
    QVERIFY2(imported["ok"].toBool(), qPrintable(report(imported)));
    QVERIFY(call("plugins.uninstall", {{"plugin", "example-control"}, {"version", "0.1.0"}})["ok"].toBool());
    QVERIFY(call("providers.remove", {{"id", provider}})["ok"].toBool());
    QVERIFY(call("themes.remove", {{"id", "custom:control"}})["ok"].toBool());
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
    QTRY_VERIFY_WITH_TIMEOUT(!about->busy(), 10000);
    QCOMPARE(about->release()["state"].toString(), QString("available"));
    const auto beforeFallback = network.urls.size();
    network.add({}, 0); network.add(metadata(false));
    about->m_updateProxy = "http://fixture.invalid:8080";
    about->m_busy = true;
    about->readRelease(QUrl("https://api.github.com/repos/umeiko/umeko-desktop-cpp/releases/latest"));
    QTRY_VERIFY_WITH_TIMEOUT(!about->busy(), 10000);
    QCOMPARE(about->release()["state"].toString(), QString("available"));
    QCOMPARE(network.urls.size(), beforeFallback + 2);
    QCOMPARE(network.proxy().type(), QNetworkProxy::NoProxy);
    QVERIFY(find("sidebarUpdateButton")->isVisible());
    const auto partial = directory.filePath("updates/Umeko/.umeko-downloads/" + U::sha(payload) + "/full.part");
    QDir().mkpath(QFileInfo(partial).absolutePath());
    QFile part(partial); QVERIFY(part.open(QIODevice::WriteOnly)); part.write(payload); part.close();
    click("sidebarUpdateButton");
    QTRY_COMPARE_WITH_TIMEOUT(about->download()["state"].toString(),
                              QString("ready"), 10000);
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
    QVERIFY(downloaded.remove());
    QTRY_VERIFY(!about->m_downloadProcess);
    QDir().mkpath(QFileInfo(partial).absolutePath());
    QVERIFY(part.open(QIODevice::WriteOnly)); part.write(QByteArray(payload.size(), 'z')); part.close();
    about->downloadUpdate();
    QTRY_COMPARE_WITH_TIMEOUT(about->download()["state"].toString(),
                              QString("error"), 10000);
    QVERIFY(about->download()["message"].toString().contains("SHA-256"));
    QTRY_VERIFY(!about->m_downloadProcess);
    QCOMPARE(QDir(directory.filePath("updates/Umeko"))
                 .entryList(QDir::Files | QDir::Hidden)
                 .size(),
             0);
    about->m_minimal = true;
    network.add(metadata(false));
    network.add(metadata(true));
    about->checkUpdates();
    QTRY_VERIFY_WITH_TIMEOUT(!about->busy(), 10000);
    QVERIFY(about->release()["asset"].toMap()["name"].toString().contains(
        "-minimal-"));
    QVERIFY(network.urls.last().path().endsWith("/v99.0.0-minimal"));
  }
  void updateDownloadProgressUi() {
    auto about = qobject_cast<AppAbout *>(backend->appAbout());
    const auto oldRelease = about->m_release, oldDownload = about->m_download;
    const auto oldPage = window->property("page"), oldTab = window->property("settingsTab");
    auto restore = qScopeGuard([&] {
      about->m_release = oldRelease; about->m_download = oldDownload; emit about->changed();
      window->setProperty("page",oldPage); window->setProperty("settingsTab",oldTab);
    });
    window->setProperty("page","settings"); window->setProperty("settingsTab",6);
    about->m_release = {{"state","available"},{"asset",QVariantMap{{"name","fixture.zip"}}}};
    about->m_download = {{"state","downloading"},{"message","正在下载更新包…"},{"progress",35},
      {"received",35*1024*1024},{"total",100*1024*1024},{"speed",2*1024*1024},{"eta",33},{"route","系统代理"},{"resumable",true}};
    emit about->changed(); QTest::qWait(100);
    QVERIFY(find("appDownloadDetails")->property("text").toString().contains("2.0 MB/s"));
    QVERIFY(find("appDownloadDetails")->property("text").toString().contains("系统代理"));
    QVERIFY(!find("downloadAppUpdateButton")->isEnabled());
    QCOMPARE(find("cancelAppDownloadButton")->property("text").toString(),QString("暂停下载"));
    const auto snapshot = qEnvironmentVariable("UMEKO_UPDATE_UI_SNAPSHOT");
    if (!snapshot.isEmpty()) {
      auto flick = find("settingsScroll")->property("contentItem").value<QObject *>();
      QVERIFY(flick); flick->setProperty("contentY",qMax(0.0,flick->property("contentHeight").toDouble()-flick->property("height").toDouble()));
      QTest::qWait(900); QVERIFY(window->grabWindow().save(snapshot));
    }
    about->m_download["state"]="verifying"; emit about->changed(); QTest::qWait(20);
    QVERIFY(find("appDownloadProgress")->property("indeterminate").toBool());
    QVERIFY(!find("checkAppUpdatesButton")->isEnabled());
    about->m_download["state"]="paused"; emit about->changed(); QTest::qWait(20);
    QVERIFY(find("downloadAppUpdateButton")->isEnabled());
    QCOMPARE(find("downloadAppUpdateButton")->property("text").toString(),QString("继续下载"));
    QVERIFY(!find("cancelAppDownloadButton")->isVisible());
  }
  void taskCompletionEventsAreNotDuplicatedOrSentForCancellation() {
    const auto metadata = backend->store.data["sessionMeta"];
    auto restore = qScopeGuard([&] {backend->store.data["sessionMeta"] = metadata; backend->store.save();});
    QSignalSpy finished(backend.get(), &Backend::taskFinished);
    auto task = std::make_shared<Session>(); task->id = "notification-fixture"; task->title = "Local task";
    task->action = "prompt"; task->phase = "running"; task->ready = true;
    backend->event(task, {{"type","done"}}); backend->event(task, {{"type","done"}});
    QCOMPARE(finished.count(),1); QVERIFY(finished.first()[2].toBool()); QVERIFY(!finished.first()[3].toBool());
    task->phase = "running"; task->cancelled = true; backend->event(task, {{"type","done"}}); QCOMPARE(finished.count(),1);
    task->cancelled = false; task->failed = true; task->phase = "running"; backend->event(task, {{"type","done"}});
    QCOMPARE(finished.count(),2); QVERIFY(!finished.last()[2].toBool());
    task->action = "reload"; task->phase = "working"; backend->event(task, {{"type","done"}}); QCOMPARE(finished.count(),2);
  }
  void longTaskNotificationsUseTheOwningPromptClock() {
    const auto metadata = backend->store.data["sessionMeta"];
    auto restore = qScopeGuard([&] {backend->store.data["sessionMeta"] = metadata; backend->store.save();});
    QSignalSpy finished(backend.get(), &Backend::taskFinished);
    auto task = std::make_shared<Session>(); task->id = "long-notification-fixture"; task->title = "Background task";
    task->action = "prompt"; task->phase = "running"; task->ready = true;
    task->promptStarted = std::chrono::steady_clock::now() - std::chrono::seconds(181);
    backend->event(task, {{"type","done"}});
    QCOMPARE(finished.count(),1); QCOMPARE(finished.first()[0].toString(),task->id);
    QVERIFY(finished.first()[2].toBool()); QVERIFY(finished.first()[3].toBool()); QVERIFY(!task->promptStarted);
    backend->event(task, {{"type","done"}}); QCOMPARE(finished.count(),1);
    task->phase = "running"; task->promptStarted = std::chrono::steady_clock::now() - std::chrono::seconds(179);
    backend->event(task, {{"type","done"}}); QCOMPARE(finished.count(),2); QVERIFY(!finished.last()[3].toBool());
    task->phase = "running"; task->failed = true; task->promptStarted = std::chrono::steady_clock::now() - std::chrono::seconds(181);
    backend->event(task, {{"type","done"}}); QCOMPARE(finished.count(),3); QVERIFY(!finished.last()[2].toBool()); QVERIFY(finished.last()[3].toBool());
    task->phase = "running"; task->cancelled = true; task->promptStarted = std::chrono::steady_clock::now() - std::chrono::seconds(181);
    backend->event(task, {{"type","done"}}); QCOMPARE(finished.count(),3); QVERIFY(!task->promptStarted);
    task->phase = "working"; task->action = "reload"; task->cancelled = false;
    task->promptStarted = std::chrono::steady_clock::now() - std::chrono::seconds(181);
    backend->event(task, {{"type","done"}}); QCOMPARE(finished.count(),3); QVERIFY(!task->promptStarted);
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
    QTRY_VERIFY(window->findChild<QObject*>("providerEditorPopup")->property("visible").toBool());
    QVERIFY(!inside(find("providerEditor"), find("commonProvidersSection")));
    QCOMPARE(find("providerName")->property("text").toString(), QString("Kimi(Work)"));
    click("closeProviderEditorButton");
    QTRY_VERIFY(!find("settingsPage")->property("editingProvider").toBool());
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
    // The hidden attached scrollbar was removed to avoid feeding estimated
    // content sizes back into virtualized history positioning.
    QVERIFY(!find("conversationScrollBar"));
    movePointer(point(nav));
    QTRY_VERIFY_WITH_TIMEOUT(nav->property("expanded").toBool(), 1000);
    auto panel = window->findChild<QObject *>("questionNavigatorPanel");
    QVERIFY(panel);
    // reveal() schedules its initial scroll with Qt.callLater. Wait for the
    // popup's enter transition so that scroll cannot undo our selection below.
    QTRY_VERIFY_WITH_TIMEOUT(panel->property("opened").toBool(), 2000);
    auto listing = find("questionNavigationList");
    QVERIFY(listing);
    const auto selectFirstQuestion = [&] {
      QMetaObject::invokeMethod(listing, "positionViewAtBeginning");
      QTest::qWait(150);
      auto first = find("questionNavigationItem_0");
      QVERIFY(first);
      QTRY_VERIFY_WITH_TIMEOUT(
          listing->boundingRect().contains(
              first->mapToItem(listing, first->boundingRect().center())), 2000);
      // Cocoa also observes the native cursor. Move into the popup before
      // clicking so its leave timer cannot close it under a synthetic click.
      movePointer(point(first));
      QTRY_VERIFY_WITH_TIMEOUT(first->property("hovered").toBool(), 1000);
      click("questionNavigationItem_0");
      QTRY_COMPARE_WITH_TIMEOUT(nav->property("activeQuestion").toInt(), 0, 2000);
      QTRY_VERIFY_WITH_TIMEOUT(!view->property("restoring").toBool(), 2000);
    };
    selectFirstQuestion();
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
    selectFirstQuestion();
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
  void composerModifierEnterKeepsDraftAndPlainEnterSends() {
    configureAPI(); window->setProperty("page", "chat"); QTest::qWait(200);
    auto prompt = find("promptInput"); QVERIFY(prompt); prompt->forceActiveFocus();
    const auto count = requests.size();
    prompt->setProperty("text", "first"); prompt->setProperty("cursorPosition", 5);
    for (const auto key : {Qt::Key_Return, Qt::Key_Enter}) {
      const auto before = prompt->property("text").toString();
      QTest::keyClick(window,key,Qt::AltModifier); QTest::qWait(30);
      QCOMPARE(prompt->property("text").toString(),before + "\n"); QCOMPARE(requests.size(),count);
    }
    QTest::keyClick(window,Qt::Key_Return,Qt::ShiftModifier); QTest::qWait(30);
    QCOMPARE(prompt->property("text").toString(),QString("first\n\n\n"));
    QMetaObject::invokeMethod(prompt,"select",Q_ARG(int,0),Q_ARG(int,5));
    QTest::keyClick(window,Qt::Key_Return,Qt::AltModifier); QTest::qWait(30);
    QCOMPARE(prompt->property("text").toString(),QString("\n\n\n\n")); QCOMPARE(requests.size(),count);
    prompt->setProperty("text","Plain Enter sends"); QTest::keyClick(window,Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(requests.size()>count,15000);
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),15000);
  }
  void providerCertificateSettings() {
    configureAPI();
    const auto id = backend->config()["selectedProvider"].toString();
    const auto ca = U::cleanPath(root + "/tests/fixtures/update-tls/ca.pem");
    auto network = [this, id] {
      for (const auto &value : backend->config()["providers"].toList()) {
        const auto provider = value.toMap();
        if (provider["id"] == id) return provider["proxy"].toMap();
      }
      return QVariantMap{};
    };
    window->setProperty("page", "settings");
    window->setProperty("settingsTab", 0);
    QTest::qWait(250);
    click("providerProxy_" + id);
    QVERIFY(find("providerCaFile"));
    find("providerCaFile")->setProperty("text", ca);
    find("providerProxyMode")->setProperty("currentIndex", 1);
    click("saveProviderProxy");
    QCOMPARE(network()["mode"].toString(), QString("direct"));
    QCOMPARE(network()["caFile"].toString(), ca);
    QVERIFY(backend->setProviderProxy(id, "custom", "http://127.0.0.1:7890"));
    QCOMPARE(network()["caFile"].toString(), ca);
    const auto saved = network();
    QVERIFY(!backend->setProviderNetwork(id, "system", "", root + "/missing-ca.pem"));
    QCOMPARE(network(), saved);
    click("providerProxy_" + id);
    QCOMPARE(find("providerCaFile")->property("text").toString(), ca);
    find("providerCaFile")->setProperty("text", root + "/missing-ca.pem");
    click("saveProviderProxy");
    QVERIFY(find("providerCaFile")->isVisible());
    QCOMPARE(network(), saved);
    find("providerCaFile")->setProperty("text", "");
    click("saveProviderProxy");
    QVERIFY(!network().contains("caFile"));
    QVERIFY(backend->setProviderProxy(id, "direct", ""));
    window->setProperty("page", "chat");
  }
  void appearanceAndRequestDetails() {
    configureAPI();
    window->setProperty("page", "settings");
    window->setProperty("settingsTab", 0);
    QTest::qWait(250);
    const auto editorId = backend->config()["selectedProvider"].toString();
    click("editProvider_" + editorId);
    QVERIFY(find("settingsPage")->property("editingProvider").toBool());
    QCOMPARE(find("editProvider_" + editorId)->property("glyph").toString(),
             QString("edit"));
    click("closeProviderEditorButton");
    QTRY_VERIFY(!find("settingsPage")->property("editingProvider").toBool());
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
    QVERIFY(find("requestDetailsText")->property("text").toString().contains("request failure fixture"));
    click("requestDetailsResponseTab");
    const auto responseText = find("requestDetailsText")->property("text").toString();
    QVERIFY(responseText.contains("400"));
    QVERIFY(responseText.contains("complete upstream error details"));
    QVERIFY(!responseText.contains("request failure fixture"));
    click("requestDetailsRequestTab");
    QVERIFY(find("requestDetailsText")->property("text").toString().contains("request failure fixture"));
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
  void themeThumbnailsAreIndependentOfCharacters() {
    const auto previousPage=window->property("page"),previousTab=window->property("settingsTab");
    const auto previousCharacter=backend->characterStyle()["id"].toString();
    const auto restore=qScopeGuard([&] {
      backend->selectCharacter(previousCharacter);
      window->setProperty("page",previousPage);
      window->setProperty("settingsTab",previousTab);
    });
    window->setProperty("page","settings");
    window->setProperty("settingsTab",5);
    for (const auto &character:QStringList{"yanami","komari","kaju"}) {
      QVERIFY(backend->selectCharacter(character));
      QTest::qWait(250);
      for (const auto &theme:QStringList{"light","dark","yanami"}) {
        auto card=find("theme_"+theme);
        QVERIFY(card && card->isVisible());
        QList<QQuickItem *> pending{card};
        while (!pending.isEmpty()) {
          auto item=pending.takeLast();
          QVERIFY2(!item->inherits("QQuickImage"),"Theme thumbnails must show colors without character artwork");
          pending.append(item->childItems());
        }
      }
    }
    QDir().mkpath(root+"/build/screenshots");
    QVERIFY(window->grabWindow().save(root+"/build/screenshots/theme-preview-no-character.png"));
  }
  void characterPickerAndZip() {
    const auto previousCharacter = backend->characterStyle()["id"].toString();
    const auto previousTheme =
        backend->store.data.value("theme", "light").toString();
    const auto previousPage = window->property("page"),
               previousTab = window->property("settingsTab");
    const auto restore = qScopeGuard([&] {
      backend->removeCharacter("zip-character");
      backend->selectCharacter(previousCharacter);
      backend->setTheme(previousTheme);
      window->setProperty("page", previousPage);
      window->setProperty("settingsTab", previousTab);
    });
    for (const auto &id : QStringList{"yanami", "komari", "kaju"})
      QVERIFY(!backend->removeCharacter(id));
    QVERIFY(backend->selectCharacter("kaju"));
    QCOMPARE(backend->characterStyle()["name"].toString(), QString("温水佳树"));
    QCOMPARE(backend->characterStyle()["assets"].toMap().size(), 10);
    for (const auto &url : backend->characterStyle()["assets"].toMap())
      QVERIFY(QFileInfo(QUrl(url.toString()).toLocalFile()).isFile());
    backend->setTheme("yanami");
    QCOMPARE(backend->characterStyle()["id"].toString(), QString("kaju"));
    window->setProperty("page", "settings");
    window->setProperty("settingsTab", 5);
    QTest::qWait(200);
    click("chooseCharacterButton");
    QTRY_VERIFY(find("character_kaju") && find("character_kaju")->isVisible());
    QVERIFY(!find("deleteCharacter_yanami")->isVisible());
    QVERIFY(!find("deleteCharacter_komari")->isVisible());
    QVERIFY(!find("deleteCharacter_kaju")->isVisible());
    click("character_komari");
    QCOMPARE(backend->characterStyle()["id"].toString(), QString("komari"));
    click("character_kaju");
    QDir().mkpath(root + "/build/screenshots");
    QVERIFY(window->grabWindow().save(
        root + "/build/screenshots/character-picker.png"));
    click("closeCharacterDialog");
    QTest::qWait(350);
    QVERIFY(window->grabWindow().save(
        root + "/build/screenshots/character-themes.png"));
    window->setProperty("page", "chat");
    backend->newSession();
    QTRY_VERIFY_WITH_TIMEOUT(find("welcomeMascot")->property("loaded").toBool(),
                             3000);
    QVERIFY(window->grabWindow().save(root +
                                      "/build/screenshots/kaju-welcome.png"));
    const auto fixtures = directory.filePath("character-zip-fixture");
    QVERIFY(QDir().mkpath(fixtures));
    QImage image(16, 16, QImage::Format_ARGB32);
    image.fill(QColor("#aa4466"));
    QByteArray png;
    QBuffer buffer(&png);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(image.save(&buffer, "PNG"));
    QVariantMap manifest{
        {"format", "umeko-character"},
        {"version", 1},
        {"id", "zip-character"},
        {"name", "ZIP Character"},
        {"assets", QVariantMap{{"welcome", "portrait.png"},
                               {"avatarSmile", "portrait.png"}}}};
    const auto archive = fixtures + "/character.zip";
    auto writeZip = [&](QList<QPair<QString, QByteArray>> entries) {
      mz_zip_archive zip{};
      if (!mz_zip_writer_init_file(&zip, archive.toUtf8().constData(), 0))
        return false;
      bool ok = true;
      for (const auto &entry : entries)
        ok &= mz_zip_writer_add_mem(&zip, entry.first.toUtf8().constData(),
                                    entry.second.constData(),
                                    entry.second.size(), MZ_BEST_COMPRESSION);
      ok &= mz_zip_writer_finalize_archive(&zip);
      mz_zip_writer_end(&zip);
      return ok;
    };
    const auto json = [](QVariant value) {
      return QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact);
    };
    for (const auto &prefix : QStringList{"", "wrapped/"}) {
      QVERIFY(writeZip({{prefix + "manifest.json", json(manifest)},
                        {prefix + "portrait.png", png}}));
      QVERIFY(
          backend->importCharacter(QUrl::fromLocalFile(archive).toString()));
      QCOMPARE(backend->characterStyle()["id"].toString(),
               QString("zip-character"));
    }
    const auto installed = backend->characterStyle()["assets"].toMap();
    for (const auto &id : QStringList{"yanami", "komari", "kaju"}) {
      auto reserved = manifest;
      reserved["id"] = id;
      QVERIFY(
          writeZip({{"manifest.json", json(reserved)}, {"portrait.png", png}}));
      QVERIFY(!backend->importCharacter(archive));
    }
    QList<QList<QPair<QString, QByteArray>>> bad{
        {{"manifest.json", json(manifest)}, {"../outside.png", png}},
        {{"manifest.json", json(manifest)},
         {"portrait.png", png},
         {"PORTRAIT.PNG", png}},
        {{"wrapped/manifest.json", json(manifest)}, {"portrait.png", png}},
        {{"a/b/manifest.json", json(manifest)}},
        {{"manifest.json", json(manifest)}, {"script.py", "print(1)"}},
        {{"manifest.json", json(manifest)},
         {"portrait.png", QByteArray(12 * 1024 * 1024 + 1, 'x')}},
        {{"manifest.json", json(manifest)}, {"portrait.png", "not an image"}},
        {{"manifest.json", json(manifest)},
         {"thing.json", "{}"},
         {"thing.json/image.png", png}}};
    for (const auto &entries : bad) {
      QVERIFY(writeZip(entries));
      QVERIFY(!backend->importCharacter(archive));
      QCOMPARE(backend->characterStyle()["id"].toString(),
               QString("zip-character"));
      QCOMPARE(backend->characterStyle()["assets"].toMap(), installed);
    }
    const auto skill = fixtures + "/create-umeko-character.zip";
    QVERIFY(
        backend->exportCharacterSkill(QUrl::fromLocalFile(skill).toString()));
    QFile file(skill);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto bytes = file.readAll();
    mz_zip_archive zip{};
    QVERIFY(mz_zip_reader_init_mem(&zip, bytes.constData(), bytes.size(), 0));
    QVERIFY(mz_zip_reader_locate_file(&zip, "create-umeko-character/SKILL.md",
                                      nullptr, 0) >= 0);
    QVERIFY(mz_zip_reader_locate_file(
                &zip, "create-umeko-character/scripts/package.py", nullptr,
                0) >= 0);
    mz_zip_reader_end(&zip);
    window->setProperty("page", "settings");
    window->setProperty("settingsTab", 5);
    click("chooseCharacterButton");
    QTRY_VERIFY(find("deleteCharacter_zip-character") &&
                find("deleteCharacter_zip-character")->isVisible());
    auto list = find("characterList");
    list->setProperty(
        "contentY",
        qMax(0.0, list->property("contentHeight").toDouble() - list->height()));
    QTest::qWait(350);
    click("deleteCharacter_zip-character");
    QTRY_VERIFY(find("confirmRemoveCharacter"));
    click("confirmRemoveCharacter");
    QCOMPARE(backend->characterStyle()["id"].toString(), QString("yanami"));
    click("closeCharacterDialog");
    QTest::qWait(350);
  }
  void characterArtworkAndDialogue() {
    const auto previousCharacter=backend->store.data.value("character","yanami");
    const auto previousOptions=backend->characterConfiguration()["overrides"].toMap();
    const auto previousPage=window->property("page");
    const auto json=[](QVariant value) { return QString::fromUtf8(QJsonDocument::fromVariant(value).toJson(QJsonDocument::Compact)); };
    const auto restore=qScopeGuard([&] {
      backend->configureCharacter(json(previousOptions));
      backend->selectCharacter(previousCharacter.toString());
      window->setProperty("page",previousPage);
    });
    backend->newSession();
    window->setProperty("page","chat");
    QVERIFY(backend->selectCharacter("komari"));
    QCOMPARE(backend->characterStyle()["id"].toString(),QString("komari"));
    QCOMPARE(backend->characterStyle()["assets"].toMap().size(),10);
    QTRY_VERIFY_WITH_TIMEOUT(find("welcomeMascot")->property("loaded").toBool(),3000);
    QDir().mkpath(root+"/build/screenshots");
    QVERIFY(window->grabWindow().save(root+"/build/screenshots/komari-welcome.png"));
    const auto fixtures = directory.filePath("character-fixture");
    QVERIFY(QDir().mkpath(fixtures));
    QImage portrait(16, 16, QImage::Format_ARGB32);
    portrait.fill(QColor("#bb4466"));
    QVERIFY(portrait.save(fixtures + "/portrait.png"));
    const auto gif = QByteArray::fromHex(
        "47494638396101000100800000000000ffffff21ff0b4e45545343415045322e300301"
        "00000021f904000a0000002c000000000100010000020244010021f904000a0000002c"
        "00000000010001000002024c01003b");
    QFile movie(fixtures + "/portrait.gif");
    QVERIFY(movie.open(QIODevice::WriteOnly));
    QCOMPARE(movie.write(gif), gif.size());
    movie.close();
    const QVariantMap options{
        {"scenes",
         QVariantMap{
             {"welcome", QVariantMap{{"tap", QVariantList{"custom welcome"}}}},
             {"reply", QVariantMap{{"tap", QVariantList{"custom reply"}}}}}},
        {"subagents",
         QVariantList{QVariantMap{{"name", "Custom Worker"},
                                  {"avatar", fixtures + "/portrait.png"},
                                  {"catch", fixtures + "/portrait.gif"},
                                  {"tap", QVariantList{"custom role"}}}}}};
    auto result = backend->umekoRequest(
        {{"operation", "characters.configure"},
         {"arguments", QVariantMap{{"config", options}}}});
    QVERIFY2(result["ok"].toBool(), qPrintable(json(result)));
    QCOMPARE(backend->characterNames().value(0), QString("Custom Worker"));
    QCOMPARE(backend->characterNames().value(1), QString("小鞠知花"));
    QVERIFY(backend->characterStyle()["subagents"]
                .toList()
                .first()
                .toMap()["avatar"]
                .toString()
                .contains("/character-assets/"));
    QTest::qWait(150);
    QVERIFY(QMetaObject::invokeMethod(find("welcomeMascot"), "tap"));
    QCOMPARE(find("welcomeMascot")->property("reactionText").toString(), QString("custom welcome"));
    QVERIFY(
        QFileInfo::exists(backend->store.directory + "/character-config.json"));
    const QVariantMap manifest{
        {"format", "umeko-character"},
        {"version", 1},
        {"id", "animated-test"},
        {"name", "Animated Test"},
        {"assets", QVariantMap{{"welcome", "portrait.gif"},
                               {"reply", "portrait.png"},
                               {"loading", "portrait.gif"}}}};
    QVERIFY(U::writeJson(fixtures + "/manifest.json", manifest));
    QVERIFY(backend->importCharacter(fixtures + "/manifest.json"));
    QTRY_VERIFY_WITH_TIMEOUT(find("welcomeMascot")->property("loaded").toBool(),
                             3000);
    QCOMPARE(backend->characterStyle()["id"].toString(),
             QString("animated-test"));
    const auto previousArtwork = backend->characterStyle()["assets"].toMap()["welcome"];
    QVERIFY(backend->importCharacter(fixtures + "/manifest.json"));
    QTRY_VERIFY_WITH_TIMEOUT(find("welcomeMascot")->property("loaded").toBool(),3000);
    QVERIFY(backend->characterStyle()["assets"].toMap()["welcome"] != previousArtwork);
    auto component = std::make_unique<QQmlComponent>(
        engine.get(), QUrl::fromLocalFile(root + "/app/qml/CharacterArt.qml"));
    std::unique_ptr<QObject> movieItem(component->create());
    QVERIFY2(movieItem, qPrintable(component->errorString()));
    auto image = qobject_cast<QQuickItem *>(movieItem.get());
    QVERIFY(image);
    image->setParentItem(window->contentItem());
    image->setWidth(32);
    image->setHeight(32);
    image->setProperty("artwork",
                       QUrl::fromLocalFile(fixtures + "/portrait.gif"));
    QTRY_COMPARE_WITH_TIMEOUT(image->property("frameCount").toInt(), 2, 3000);
    QVERIFY(image->property("playing").toBool());
    image->setVisible(false);
    QTest::qWait(50);
    QVERIFY(!image->property("playing").toBool());
    image->setParentItem(nullptr);
    movieItem.reset();
    component.reset();
    QVERIFY(backend->selectCharacter("yanami"));
    QVERIFY(backend->characterStyle()["assets"].toMap().isEmpty());
    QVERIFY(backend->removeCharacter("animated-test"));
    QVERIFY(!backend->removeCharacter("komari"));
    QVERIFY(backend->configureCharacter("{}"));
    QCOMPARE(backend->characterNames().value(0), QString("烧盐柠檬"));
    QVERIFY(!backend->configureCharacter("{\"scenes\":{\"unknown\":{}}}"));
  }
  void providerDescriptions() {
    auto manager = qobject_cast<Providers *>(backend->providerPlugins());
    QVERIFY(manager);
    const auto previousPage = window->property("page"),
               previousTab = window->property("settingsTab");
    auto restore = qScopeGuard([&] {
      for (int i = 0; i < 3; ++i)
        manager->uninstall("example-description", QString("0.1.%1").arg(i));
      window->setProperty("page", previousPage);
      window->setProperty("settingsTab", previousTab);
    });
    window->setProperty("page", "settings");
    window->setProperty("settingsTab", 0);
    const QVariantList descriptions{
        QVariant(), QString(" \n "),
        QString("  插件说明\nUse <b>plain text</b>.  ")};
    for (int i = 0; i < descriptions.size(); ++i) {
      const auto version = QString("0.1.%1").arg(i);
      const auto archive =
          directory.filePath("description-" + version + ".zip");
      QVERIFY(package(archive, false, false, "example-description", version, {},
                      {}, descriptions[i]));
      QVERIFY(manager->importZip(QUrl::fromLocalFile(archive).toString()));
      const auto row = manager->rows().last().toMap();
      const auto id = row["id"].toString();
      const auto expected = descriptions[i].toString().trimmed();
      QCOMPARE(row["description"].toString(), expected);
      bool listed = false;
      for (const auto &value : manager->packages()) {
        const auto plugin = value.toMap();
        if (plugin["id"] == "example-description" &&
            plugin["version"] == version) {
          QCOMPARE(plugin["description"].toString(), expected);
          listed = true;
        }
      }
      QVERIFY(listed);
      QTest::qWait(180);
      for (const auto &name :
           {"pluginDescription_example-description_" + version,
            "providerDescription_" + id}) {
        auto target = find(name);
        QVERIFY(target);
        QCOMPARE(target->isVisible(), !expected.isEmpty());
        if (expected.isEmpty())
          continue;
        auto scroll = find("settingsScroll");
        QVERIFY(scroll);
        auto flick = qobject_cast<QQuickItem *>(
            scroll->property("contentItem").value<QObject *>());
        QVERIFY(flick);
        const auto center =
            target->mapToItem(flick, target->boundingRect().center());
        flick->setProperty(
            "contentY",
            qBound(0.0,
                   flick->property("contentY").toDouble() + center.y() -
                       flick->height() / 2,
                   qMax(0.0, flick->property("contentHeight").toDouble() -
                                 flick->height())));
        QTest::qWait(180);
        movePointer(point(target));
        QTRY_VERIFY_WITH_TIMEOUT(target->property("hovered").toBool(), 2000);
        QTRY_VERIFY_WITH_TIMEOUT(target->property("pinned").toBool(), 2000);
        auto text = find("helpText_" + name);
        QVERIFY(text);
        QCOMPARE(text->property("text").toString(), expected);
        QCOMPARE(text->property("textFormat").toInt(), 0);
        QVERIFY(text->property("readOnly").toBool());
        QDir().mkpath(root + "/build/screenshots");
        QVERIFY(window->grabWindow().save(root + "/build/screenshots/" +
                                          name.section('_', 0, 0) + ".png"));
        movePointer(QPoint(2, 2));
        QTRY_VERIFY_WITH_TIMEOUT(!target->property("pinned").toBool(), 2000);
      }
    }
    const auto invalid = directory.filePath("invalid-description.zip");
    QVERIFY(package(invalid, false, false, "example-description", "9.0.0", {},
                    {}, 42));
    const auto before = manager->rows().size();
    QVERIFY(!manager->importZip(QUrl::fromLocalFile(invalid).toString()));
    QCOMPARE(manager->rows().size(), before);
    QVERIFY(
        manager->importError()["message"].toString().contains("description"));
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
  void interactiveProviderLogin() {
    auto manager = qobject_cast<Providers *>(backend->providerPlugins());
    const auto archive = directory.filePath("interactive-login.zip");
    const QByteArray code = R"PY(from umeko_provider_sdk import Provider,ProviderError
class Interactive(Provider):
 def authenticate(self, credentials):
  self.context.secrets.set('session',{'token':'synthetic-pending-token'})
  self.context.report_status('Waiting for browser confirmation')
  while not self.context.cancelled.wait(.05): pass
  raise ProviderError('Login cancelled')
 def list_models(self, refresh=False): return []
def create_provider(context): return Interactive(context)
)PY";
    QVERIFY(package(archive, false, false, "example-interactive", "0.1.0", {}, code));
    QVERIFY(manager->importZip(QUrl::fromLocalFile(archive).toString()));
    const auto id = manager->rows().last().toMap()["id"].toString();
    manager->login(id, "{\"username\":\"fixture-user\"}");
    QTRY_COMPARE_WITH_TIMEOUT(manager->rows().last().toMap()["status"].toString(), QString("Waiting for browser confirmation"), 15000);
    QVERIFY(manager->rows().last().toMap()["loggingIn"].toBool());
    bool bounded = false;
    for (auto process : manager->findChildren<QProcess *>())
      for (auto timer : process->findChildren<QTimer *>())
        bounded |= timer->interval() == 600000 && timer->isActive();
    QVERIFY(bounded);
    manager->cancelLogin(id);
    QTRY_VERIFY_WITH_TIMEOUT(!manager->rows().last().toMap()["busy"].toBool(), 5000);
    QCOMPARE(manager->rows().last().toMap()["status"].toString(), QString("登录已取消"));
    QVERIFY(backend->store.secret("account:" + id).isEmpty());
    QVERIFY(!manager->rows().last().toMap()["loggingIn"].toBool());
    QVERIFY(manager->uninstall("example-interactive", "0.1.0"));
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
  void toolCatalogViewportAndExtensionDetails() {
    auto manager = qobject_cast<PiManagement *>(backend->piManagement());
    QVERIFY(manager);
    const auto original = backend->store.data;
    QDesktopServices::setUrlHandler("https",this,"recordOpenedUrl");
    const auto previousPage = window->property("page"), previousTab = window->property("settingsTab");
    const auto restore = qScopeGuard([&] {
      QDesktopServices::unsetUrlHandler("https");
      if (find("closeExtensionDetailsButton")) QMetaObject::invokeMethod(find("closeExtensionDetailsButton"),"clicked");
      backend->store.data = original;
      window->setProperty("page",previousPage); window->setProperty("settingsTab",previousTab);
      backend->invalidate(); backend->notify();
    });
    configureAPI();
    window->setProperty("page","settings"); window->setProperty("settingsTab",3); QTest::qWait(150);
    if (find("openPiSettingsButton")->isVisible()) click("openPiSettingsButton");
    find("piFold_kernel")->setProperty("expanded",false);
    find("piFold_extensions")->setProperty("expanded",true);
    find("piFold_packages")->setProperty("expanded",true);
    find("piFold_tools")->setProperty("expanded",true);
    manager->scan();
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(),15000);
    QString coreId;
    for (const auto &value : manager->extensions()) {
      const auto row = value.toMap(), info = row["info"].toMap();
      if (row["path"] != "builtin:codemode") continue;
      coreId = row["id"].toString();
      QCOMPARE(info["origin"].toString(),QString("pi"));
      QCOMPARE(info["version"].toString(),backend->kernelVersion());
      QVERIFY(info["repository"].toString().startsWith("https://github.com/"));
    }
    QVERIFY(!coreId.isEmpty());
    QMetaObject::invokeMethod(find("openToolCatalogButton"),"clicked");
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),15000);
    QMetaObject::invokeMethod(find("toolCategory_native"),"clicked");
    auto list = find("toolCatalogList");
    QTRY_VERIFY_WITH_TIMEOUT(list->property("count").toInt() > 0,15000);
    QTRY_VERIFY(list->width() > 300 && list->height() > 100);
    QTRY_VERIFY(find("toolEntry_read"));
    auto read = find("toolEntry_read");
    const auto position = read->mapToItem(list,QPointF(read->width()/2,read->height()/2));
    QVERIFY(read->isVisible() && read->width() > 100 && read->height() > 20);
    QVERIFY(position.y() >= 0 && position.y() <= list->height());
    QMetaObject::invokeMethod(read,"clicked"); QTest::qWait(100);
    window->grabWindow().save(root+"/build/screenshots/tool-catalog-visible.png");
    QMetaObject::invokeMethod(find("backToPiButton"),"clicked");
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(),15000);
    QMetaObject::invokeMethod(find("agentExtensionsDetails_"+coreId),"clicked");
    QTRY_VERIFY(find("extensionDetailsName")->isVisible());
    QTRY_VERIFY_WITH_TIMEOUT(find("extensionDetailsToolList")->property("count").toInt() >= 1,15000);
    QTRY_VERIFY(find("extensionDetailsToolList")->height() > 100);
    const int before = openedUrls.size();
    QMetaObject::invokeMethod(find("extensionRepositoryButton"),"clicked");
    QCOMPARE(openedUrls.size(),before+1);
    QVERIFY(openedUrls.last().host() == "github.com");
    QTcpServer registry;
    QVERIFY(registry.listen(QHostAddress::LocalHost,0));
    connect(&registry,&QTcpServer::newConnection,this,[&] {
      while (registry.hasPendingConnections()) {
        auto socket = registry.nextPendingConnection();
        connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        connect(socket,&QTcpSocket::readyRead,this,[socket] {
          auto raw = socket->property("request").toByteArray()+socket->readAll(); socket->setProperty("request",raw);
          if (!raw.contains("\r\n\r\n")) return;
          const QByteArray body = R"({"name":"@earendil-works/pi-coding-agent","dist-tags":{"latest":"99.0.0"},"versions":{"99.0.0":{"name":"@earendil-works/pi-coding-agent","version":"99.0.0"}}})";
          socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);
          socket->disconnectFromHost();
        });
      }
    });
    auto agent = backend->store.data["agent"].toMap();
    agent["npmRegistry"] = QString("http://127.0.0.1:%1").arg(registry.serverPort());
    backend->store.data["agent"] = agent;
    QMetaObject::invokeMethod(find("checkExtensionUpdateButton"),"clicked");
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(),20000);
    QCOMPARE(manager->extensionUpdate()["id"].toString(),coreId);
    QCOMPARE(manager->extensionUpdate()["latest"].toString(),QString("99.0.0"));
    QCOMPARE(manager->extensionUpdate()["status"].toString(),QString("available"));
    QTRY_VERIFY(find("extensionUpdateResult")->property("text").toString().contains("99.0.0"));
    QCOMPARE(backend->kernelVersion(),QString("1.3.0"));
    window->grabWindow().save(root+"/build/screenshots/extension-details.png");
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
  void mcpAddPopupUsesTheCurrentPiConfiguration() {
    auto manager = qobject_cast<PiManagement*>(backend->piManagement());
    QVERIFY(manager);
    const auto original = backend->store.data;
    const auto global = backend->readMcp("global"), project = backend->readMcp("project");
    const auto previousPage = window->property("page"), previousTab = window->property("settingsTab");
    const auto restore = qScopeGuard([&] {
      U::writeJson(backend->mcpConfigPath("global"), QJsonDocument::fromJson(global.toUtf8()).toVariant());
      if (!backend->mcpConfigPath("project").isEmpty()) U::writeJson(backend->mcpConfigPath("project"), QJsonDocument::fromJson(project.toUtf8()).toVariant());
      backend->store.data = original;
      window->setProperty("page",previousPage); window->setProperty("settingsTab",previousTab);
      backend->invalidate(); backend->notify();
      QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(),15000);
    });
    if (backend->config()["workspace"].toString().isEmpty()) {
      const auto workspace = directory.filePath("mcp-workspace");
      QDir().mkpath(workspace);
      backend->setWorkspace(QUrl::fromLocalFile(workspace).toString());
    }
    manager->setRecoveryMode(false);
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(),15000);
    window->setProperty("page","settings"); window->setProperty("settingsTab",3); QTest::qWait(200);
    click("openPiSettingsButton");
    find("piFold_kernel")->setProperty("expanded",false);
    find("piFold_extensions")->setProperty("expanded",true);
    find("piFold_mcp")->setProperty("expanded",true);
    QTest::qWait(200);
    QTRY_VERIFY_WITH_TIMEOUT(find("addMcpButton")->isEnabled(),15000);
    QMetaObject::invokeMethod(find("addMcpButton"),"clicked");
    QTRY_VERIFY(find("mcpInstallCommand") && find("mcpInstallCommand")->isVisible());
    auto command = find("mcpInstallCommand"), scope = find("mcpInstallScope");
    command->setProperty("text","pi mcp add ui-fixture --url http://127.0.0.1:9/mcp");
    window->grabWindow().save(root + "/build/screenshots/mcp-add-popup.png");
    click("confirmMcpInstallButton");
    QTRY_VERIFY_WITH_TIMEOUT(manager->installState() != "installing",15000);
    QCOMPARE(manager->installState(),QString("succeeded"));
    QVERIFY(QJsonDocument::fromJson(backend->readMcp("global").toUtf8()).object()["mcpServers"].toObject().contains("ui-fixture"));
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),15000);
    scope->setProperty("currentIndex",1);
    command->setProperty("text","{\"mcpServers\":{\"project-fixture\":{\"url\":\"http://127.0.0.1:9/mcp\",\"enabled\":false}}}");
    click("confirmMcpInstallButton");
    QTRY_VERIFY_WITH_TIMEOUT(manager->installState() != "installing",15000);
    QCOMPARE(manager->installState(),QString("succeeded"));
    QVERIFY(QJsonDocument::fromJson(backend->readMcp("project").toUtf8()).object()["mcpServers"].toObject().contains("project-fixture"));
    QVERIFY(!QJsonDocument::fromJson(backend->readMcp("global").toUtf8()).object()["mcpServers"].toObject().contains("project-fixture"));
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),15000);
    click("closeMcpInstallButton");
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
    auto revealAndClick = [&](QString name) -> bool {
      auto item = find(name);
      auto scroll = find("settingsScroll");
      if (!item || !scroll) return false;
      auto flick = qobject_cast<QQuickItem*>(scroll->property("contentItem").value<QObject*>());
      if (!flick) return false;
      // Cocoa can finish polishing the new column after its backend signal.
      // Check the actual viewport before sending a native mouse event.
      scroll->forceActiveFocus();
      QMetaObject::invokeMethod(flick,"cancelFlick");
      for (int attempt = 0; attempt < 12; ++attempt) {
        QTest::qWait(100);
        flick->ensurePolished(); item->ensurePolished();
        const QPointF center(item->width()/2,item->height()/2);
        const auto local = item->mapToItem(flick,center);
        const auto scene = item->mapToScene(center);
        if (item->isVisible() && flick->contains(local) &&
            QRectF(0,0,window->width(),window->height()).contains(scene)) {
          QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,scene.toPoint());
          QTest::qWait(100);
          return true;
        }
        const auto y = flick->property("contentY").toReal() + local.y() - flick->height()/2;
        flick->setProperty("contentY",qBound(0.,y,qMax(0.,flick->property("contentHeight").toReal()-flick->height())));
      }
      qWarning() << "Could not reveal" << name << "at" << point(item)
                 << "viewport" << flick->size() << "contentHeight" << flick->property("contentHeight");
      return false;
    };
    QVERIFY(!find("proxyAddressPicker"));
    QCOMPARE(find("proxyOpenaiBaseUrl")->property("text").toString(),QString("http://127.0.0.1:%1/v1").arg(server["port"].toInt()));
    QVERIFY(!find("mappingModelId_0")); QVERIFY(!find("mappingModelId_1"));
    QVERIFY(revealAndClick("mappingProvider_group-a"));
    QTRY_VERIFY(find("mappingModelId_0") && find("mappingModelId_0")->isVisible()); QVERIFY(!find("mappingModelId_1"));
    auto alias = find("mappingAlias_0"); alias->setProperty("text","renamed-a"); QMetaObject::invokeMethod(alias,"textEdited");
    QVERIFY(revealAndClick("backToMappingProviders")); QVERIFY(revealAndClick("mappingProvider_group-b"));
    QVERIFY(!find("mappingModelId_0")); QTRY_VERIFY(find("mappingModelId_1") && find("mappingModelId_1")->isVisible());
    QVERIFY(revealAndClick("backToMappingProviders")); QVERIFY(revealAndClick("mappingProvider_group-a"));
    QTRY_VERIFY(find("mappingAlias_0"));
    QCOMPARE(find("mappingAlias_0")->property("text").toString(),QString("renamed-a"));
    QVERIFY(revealAndClick("saveMappingsButton"));
    const auto saved = backend->store.data["providers"].toList();
    QCOMPARE(saved[0].toMap()["models"].toList()[0].toMap()["alias"].toString(),QString("renamed-a"));
    QCOMPARE(saved[1].toMap()["models"].toList()[0].toMap()["alias"].toString(),QString("model-b"));
    server["lan"] = true; backend->store.data["server"] = server; emit backend->changed(); QTest::qWait(100);
    const auto addresses = backend->proxyAddresses();
    const auto base = addresses.size() > 1 ? QString("http://%1:%2").arg(addresses[1]).arg(server["port"].toInt()) : QString();
    QCOMPARE(find("proxyAnthropicBaseUrl")->property("text").toString(),base);
    if (!base.isEmpty()) {
      QVERIFY(revealAndClick("copyProxyOpenaiBaseUrl")); QCOMPARE(QGuiApplication::clipboard()->text(),base+"/v1");
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
  void desktopPluginPagesAndSessionBoundary() {
    auto manager=qobject_cast<DesktopPlugins*>(backend->desktopPlugins());
    QVERIFY(manager);QCOMPARE(manager->rows().size(),4);
    QTemporaryDir workspace;QVERIFY(workspace.isValid());
    QFile file(workspace.filePath("hello.md"));QVERIFY(file.open(QIODevice::WriteOnly));file.write("# Plugin fixture\n");file.close();
#ifndef Q_OS_WIN
    QSignalSpy chooser(manager,&DesktopPlugins::openWithRequested);
    manager->hostEvent("file-tree",{{"event","file.openWith"},{"arguments",QVariantMap{{"path",workspace.filePath("hello.md")}}}});
    // The request must be in the selected workspace before offering a chooser.
    QCOMPARE(chooser.size(),0);
#endif
    QVERIFY(QDir().mkpath(workspace.filePath("folder")));
    backend->setWorkspace(QUrl::fromLocalFile(workspace.path()).toString());
#ifndef Q_OS_WIN
    manager->hostEvent("file-tree",{{"event","file.openWith"},{"arguments",QVariantMap{{"path",workspace.filePath("hello.md")}}}});
    QCOMPARE(chooser.size(),1);QCOMPARE(chooser.first()[0].toString(),workspace.filePath("hello.md"));
#endif
    window->resize(1560,1000);window->setProperty("settingsTab",3);window->setProperty("page","settings");QTest::qWait(100);click("openDesktopPluginsButton");QTest::qWait(200);
    QVERIFY(find("desktopPluginsPage"));QVERIFY(find("desktopPluginsPage")->isVisible());
    auto search=find("desktopPluginSearch");QVERIFY(search);search->setProperty("text","版本");QTest::qWait(100);
    QCOMPARE(find("desktopPluginInstalledList")->property("count").toInt(),1);search->setProperty("text","");
    const auto output=root+"/build/screenshots";QDir().mkpath(output);window->grabWindow().save(output+"/desktop-plugin-manager.png");
    QVERIFY(manager->openPage("file-tree"));
    QTRY_VERIFY_WITH_TIMEOUT(find("fileTreeEntries")&&find("fileTreeEntries")->property("count").toInt()>=2,10000);
    QCOMPARE(find("webPane")->property("mode").toString(),"plugin");QTRY_VERIFY(!find("webPane")->property("moving").toBool());
    window->grabWindow().save(output+"/desktop-plugin-tree.png");
    click("fileTreeRow_hello.md");
    QVERIFY(backend->attachments().isEmpty());
    auto treeRow=find("fileTreeRow_hello.md");QVERIFY(treeRow);QTest::mouseDClick(window,Qt::LeftButton,Qt::NoModifier,treeRow->mapToScene(QPointF(treeRow->width()/2,treeRow->height()/2)).toPoint());
    QTRY_VERIFY(!backend->attachments().isEmpty());
    QVERIFY(manager->openPage("file-tree"));QTRY_VERIFY(find("fileTreeEntries"));
    window->setProperty("page","settings");QTest::qWait(100);QVERIFY(manager->page().isEmpty());QVERIFY(!find("webPane")->property("opened").toBool());
    manager->setSessionEnabled("file-tree",false);QVERIFY(!manager->openPage("file-tree"));manager->setSessionEnabled("file-tree",true);
    window->setProperty("page","chat");QVERIFY(manager->openPage("file-tree"));QTest::qWait(150);backend->newSession();QTest::qWait(100);QVERIFY(manager->page().isEmpty());
    QVERIFY(!manager->uninstall("file-tree"));
    // A drive/filesystem root is a valid explicitly selected workspace.
    const auto filesystemRoot=QStorageInfo(workspace.path()).rootPath();
    backend->setWorkspace(QUrl::fromLocalFile(filesystemRoot).toString());
    QSignalSpy documentEvents(backend.get(),&Backend::documentRequested);
    QVERIFY(manager->openPage("file-tree"));
    const auto relative=QDir(filesystemRoot).relativeFilePath(workspace.filePath("hello.md"));
    QVERIFY(!manager->call("preview",QString::fromUtf8(QJsonDocument(QJsonObject{{"path",relative}}).toJson(QJsonDocument::Compact))).isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!documentEvents.isEmpty(),10000);
    backend->setWorkspace(QUrl::fromLocalFile(workspace.path()).toString());

    backend->setFontScale(2);window->setProperty("settingsTab",3);window->setProperty("page","settings");QTest::qWait(100);click("openDesktopPluginsButton");QTest::qWait(250);
    QVERIFY(find("desktopPluginSearch")->isVisible());backend->setFontScale(1);window->setProperty("settingsTab",0);window->setProperty("page","chat");
  }
  void desktopPluginZipLifecycle() {
    auto manager=qobject_cast<DesktopPlugins*>(backend->desktopPlugins());QVERIFY(manager);
    QTemporaryDir workspace;QVERIFY(workspace.isValid());backend->setWorkspace(QUrl::fromLocalFile(workspace.path()).toString());
    auto archive=directory.filePath("desktop-plugin.zip");auto marker=directory.filePath("plugin-activated.txt");
    mz_zip_archive zip{};QVERIFY(mz_zip_writer_init_file(&zip,archive.toUtf8().constData(),0));
    QDir source(root+"/umeko-plugins/file-tree");QDirIterator files(source.path(),QDir::Files,QDirIterator::Subdirectories);
    while(files.hasNext()){
      QFile file(files.next());auto name=source.relativeFilePath(file.fileName());if(name.contains("__pycache__"))continue;
      QVERIFY(file.open(QIODevice::ReadOnly));auto raw=file.readAll();
      if(name=="manifest.json"){auto m=QJsonDocument::fromJson(raw).object();m["id"]="zip-plugin-fixture";m["name"]="ZIP fixture";m["builtin"]=true;raw=QJsonDocument(m).toJson();}
      if(name=="scripts/plugin.py") {auto quoted=QJsonDocument(QJsonArray{marker}).toJson(QJsonDocument::Compact);quoted=quoted.mid(1,quoted.size()-2);raw="from pathlib import Path\nPath("+quoted+").write_text('activated')\n"+raw;}
      QVERIFY(mz_zip_writer_add_mem(&zip,name.toUtf8().constData(),raw.constData(),raw.size(),MZ_DEFAULT_COMPRESSION));
    }
    QVERIFY(mz_zip_writer_finalize_archive(&zip));mz_zip_writer_end(&zip);
    auto providerManager=qobject_cast<Providers*>(backend->providerPlugins());
    const auto providerCount=providerManager->rows().size();
    QVERIFY(!providerManager->importZip(QUrl::fromLocalFile(archive).toString()));
    QCOMPARE(providerManager->rows().size(),providerCount);
    manager->importZip(QUrl::fromLocalFile(archive).toString());QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(),10000);
    QVERIFY2(manager->details("zip-plugin-fixture").contains("id"),qPrintable(manager->status()));QVERIFY(!QFileInfo::exists(marker));QVERIFY(!manager->openPage("zip-plugin-fixture"));
    manager->setSessionEnabled("zip-plugin-fixture",true);QVERIFY(manager->openPage("zip-plugin-fixture"));QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(marker),10000);manager->closePage();
    manager->saveConfig("zip-plugin-fixture","{\"showHidden\":true,\"directoryLimit\":200}");QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(),10000);
    auto config=QJsonDocument::fromJson(manager->details("zip-plugin-fixture")["configText"].toString().toUtf8()).object();QVERIFY(config["showHidden"].toBool());
    QVERIFY(manager->uninstall("zip-plugin-fixture"));QVERIFY(manager->details("zip-plugin-fixture").isEmpty());QTest::qWait(300);
  }
  void applicationWidgetsAndAgentChannel() {
    auto m=qobject_cast<DesktopPlugins*>(backend->desktopPlugins());QVERIFY(m);
    auto help=backend->umekoRequest({{"operation","help"},{"arguments",QVariantMap{{"topic","widget"}}}});
    QVERIFY(help["entry"].toString().endsWith("/widget-plugins.md"));QVERIFY(!help["activeWidgets"].toList().isEmpty());
    QTemporaryDir workspace;backend->setWorkspace(QUrl::fromLocalFile(workspace.path()).toString());window->setProperty("page","chat");
    QVERIFY(!m->enabled("terminal"));m->setEnabled("terminal",true);QVERIFY(m->openPage("terminal"));
    QTRY_VERIFY_WITH_TIMEOUT(m->workers.value("terminal")&&m->workers.value("terminal")->ready,10000);
    QSignalSpy replies(m,&DesktopPlugins::response);
    auto await=[&](QString request){QVariantMap result;QElapsedTimer timer;timer.start();while(timer.elapsed()<10000){QCoreApplication::processEvents();for(auto row:replies)if(row[0].toString()==request)return row[1].toMap();QTest::qWait(10);}return result;};
    auto listed=await(m->call("list","{\"ensure\":true}"));QVERIFY2(listed["ok"].toBool(),qPrintable(listed["error"].toString()));
    const auto terminals=listed["result"].toMap()["terminals"].toList();QVERIFY(!terminals.isEmpty());auto terminal=terminals.first().toMap();const auto id=terminal["id"].toString();auto process=m->workers["terminal"]->process;
    auto raw=QString::fromUtf8(QJsonDocument::fromVariant(QVariantMap{{"terminalId",id}}).toJson(QJsonDocument::Compact));
    QVERIFY(await(m->call("attach",raw))["ok"].toBool());QVERIFY(backend->view->draft.contains(id));
    auto call=backend->umekoRequest({{"operation","widgets.call"},{"arguments",QVariantMap{{"id","terminal"},{"method","read"},{"arguments",QVariantMap{{"terminalId",id}}}}}});
    QVERIFY2(call["ok"].toBool(),qPrintable(call["error"].toString()));QCOMPARE(call["status"].toString(),"accepted");
    auto request=call["requestId"].toString();QTRY_VERIFY(m->channel->execute("widgets.result",{{"requestId",request}},{} )["status"]!="running");
    auto result=backend->umekoRequest({{"operation","widgets.result"},{"arguments",QVariantMap{{"requestId",request}}}});QCOMPARE(result["status"].toString(),"completed");QCOMPARE(result["result"].toMap()["result"].toMap()["pid"],terminal["pid"]);
    m->closePage();QVERIFY(m->workers.contains("terminal"));QCOMPARE(m->workers["terminal"]->process,process);
    auto old=backend->view;backend->newSession();QTest::qWait(100);QCOMPARE(m->workers["terminal"]->process,process);
    auto forbidden=backend->umekoRequest({{"operation","widgets.result"},{"arguments",QVariantMap{{"requestId",request}}}});QVERIFY(!forbidden["ok"].toBool());
    auto oldSnapshot=QVariantMap{{"token",old->key},{"id",old->id},{"workspace",old->workspace}};
    // A passive application update stays with its captured chat; it does not wake Pi.
    auto notification=QVariantMap{{"id",QString(32,'a')},{"message","background update"},{"data",QVariantMap{}},{"wake",false},{"session",oldSnapshot}};
    m->channel->notify("terminal",notification);
    auto updates=backend->umekoRequest({{"operation","widgets.messages"}},old->key);QVERIFY(updates["ok"].toBool());QCOMPARE(updates["updates"].toList().size(),1);
    QVERIFY(backend->umekoRequest({{"operation","widgets.messages"}})["updates"].toList().isEmpty());
    m->setEnabled("terminal",false);QVERIFY(!m->workers.contains("terminal"));QTRY_VERIFY(process.isNull()||process->state()==QProcess::NotRunning);
    auto pet=qobject_cast<DesktopPet*>(m->pet());QVERIFY(pet);QVERIFY(!pet->appearances().isEmpty());QVERIFY(pet->ui("show",{})["ok"].toBool());QVERIFY(pet->visible());QVERIFY(!pet->ui("configure",{{"sampleIntervalMs",100}})["ok"].toBool());
    QTRY_VERIFY(window->findChild<QObject*>("desktopPetWindowLoader"));
    QVERIFY(pet->ui("hide",{})["ok"].toBool());QVERIFY(!pet->visible());QVERIFY(m->workers.contains("desktop-pet"));
  }
  void terminalGridColorsSelectionAndInput() {
    TerminalView terminal;terminal.setWidth(640);terminal.setHeight(320);
    QVariantMap row{{"text","RED 中文"},{"runs",QVariantList{QVariantList{0,"RED","red","default",0,3},QVariantList{4,"中文","default","default",1,4}}}};
    terminal.applyFrame({{"revision",1},{"reset",true},{"screen",QVariantList{row}},{"cursor",QVariantMap{{"x",8},{"y",0},{"hidden",true}}}});
    QImage image(640,320,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);QPainter painter(&image);terminal.paint(&painter);painter.end();
    bool red=false;for(int y=4;y<28;++y)for(int x=4;x<40;++x){auto pixel=image.pixelColor(x,y);red|=pixel.red()>pixel.green()*1.5&&pixel.red()>pixel.blue()*1.5;}
    QVERIFY(red);QCOMPARE(image.pixelColor(500,200),QColor("#ffffff"));
    terminal.selectAll();QVERIFY(terminal.hasSelection());QCOMPARE(terminal.selectedText(),QString("RED 中文"));
    QSignalSpy input(&terminal,&TerminalView::input);QKeyEvent up(QEvent::KeyPress,Qt::Key_Up,Qt::ControlModifier);QCoreApplication::sendEvent(&terminal,&up);QCOMPARE(input.last()[0].toString(),QString("\x1b[1;5A"));
    QInputMethodEvent ime;ime.setCommitString("你好");QCoreApplication::sendEvent(&terminal,&ime);QCOMPARE(input.last()[0].toString(),QString("你好"));
    terminal.applyFrame({{"revision",2},{"bracketedPaste",true}});QGuiApplication::clipboard()->setText("line1\nline2");terminal.paste();QCOMPARE(input.last()[0].toString(),QString("\x1b[200~line1\nline2\x1b[201~"));
    terminal.reset();QVERIFY(!terminal.hasSelection());
  }
  void whalePetBuiltInAndPlayback() {
    auto pet=qobject_cast<DesktopPet*>(backend->desktopExtensions->pet());QVERIFY(pet);
    const auto original=pet->config();
    const auto restore=qScopeGuard([&]{pet->ui("configure",original);});
    bool found=false;
    for (const auto &item:pet->appearances()) {
      const auto row=item.toMap();
      if(row["id"]=="whale-girl") {found=true;QVERIFY(row["builtin"].toBool());}
    }
    QVERIFY(found);
    QVERIFY(!pet->ui("removeAppearance",{{"id","whale-girl"}})["ok"].toBool());
    auto configured=pet->ui("configure",{{"appearance","whale-girl"},{"visible",true},{"paused",false}});
    QVERIFY2(configured["ok"].toBool(),qPrintable(configured["error"].toString()));
    const auto folder=root+"/umeko-plugins/desktop-pet/assets/whale-girl";
    const auto manifest=U::readJson(folder+"/pet.json").toMap();
    QCOMPARE(manifest["sourceCommit"].toString(),QString("53e9ab8e2c779f4c0feb902cfa940c098c83d228"));
    for(const auto &state:QStringList{"idle","thinking","working","waiting","done","error"}) {
      QImageReader reader(folder+"/"+manifest["states"].toMap()[state].toString());
      QVERIFY(reader.canRead());QVERIFY(reader.imageCount()>1);
      const auto image=reader.read();QVERIFY(!image.isNull());QVERIFY(image.hasAlphaChannel());
      QCOMPARE(image.size(),QSize(360,360));
    }
    QTRY_VERIFY_WITH_TIMEOUT(window->findChild<QObject*>("desktopPetWindow"),5000);
    auto petWindow=qobject_cast<QQuickWindow*>(window->findChild<QObject*>("desktopPetWindow"));QVERIFY(petWindow);
    QTRY_VERIFY(petWindow->isVisible());
    QTRY_VERIFY(petWindow->findChild<QObject*>("desktopPetAnimation"));
    auto animation=petWindow->findChild<QObject*>("desktopPetAnimation");
    QTRY_VERIFY(animation->property("ready").toBool());
    QVERIFY(animation->property("source").toUrl().toString().contains("/whale-girl/"));
    const auto frame=animation->property("currentFrame").toInt();
    QTRY_VERIFY(animation->property("currentFrame").toInt()!=frame);
    QVERIFY(pet->ui("configure",{{"paused",true}})["ok"].toBool());
    QTest::qWait(150);const auto frozen=animation->property("currentFrame");QTest::qWait(250);QCOMPARE(animation->property("currentFrame"),frozen);
    pet->interact("drag");
    QTRY_VERIFY(!petWindow->property("caption").toString().isEmpty());
    QVERIFY(manifest["dialogue"].toMap()["drag"].toList().contains(petWindow->property("caption").toString()));
    auto bubble=petWindow->findChild<QQuickItem*>("desktopPetSpeechBubble");QVERIFY(bubble);
    auto art=petWindow->findChild<QQuickItem*>("desktopPetAnimationLoader");QVERIFY(art);
    for (int size:{96,192,320}) {
      QVERIFY(pet->ui("configure",{{"size",size}})["ok"].toBool());QTest::qWait(70);
      const auto pictureTop=art->y()+art->height()-animation->property("paintedHeight").toReal()*(1-pet->state()["contentTop"].toReal());
      const auto gap=pictureTop-bubble->y()-bubble->height();
      QVERIFY2(gap>=0 && gap<=24,qPrintable(QString("Speech gap: %1").arg(gap)));
    }
    QDir().mkpath(root+"/build/screenshots");QVERIFY(petWindow->grabWindow().save(root+"/build/screenshots/whale-desktop-pet.png"));
    QVERIFY(pet->ui("hide",{})["ok"].toBool());
    QTRY_VERIFY(!window->findChild<QObject*>("desktopPetWindowLoader")->property("item").value<QObject*>());
  }
  void desktopPetImportsDataWithoutCode() {
    auto pet=qobject_cast<DesktopPet*>(backend->desktopExtensions->pet());QVERIFY(pet);
    QTemporaryDir temp;QImage image(64,64,QImage::Format_ARGB32);image.fill(QColor("#4488ff"));auto source=temp.filePath("sample.png");QVERIFY(image.save(source));
    QSignalSpy imported(pet,&DesktopPet::imported);auto result=pet->ui("importAppearance",{{"path",source}});QVERIFY(result["ok"].toBool());QTRY_VERIFY_WITH_TIMEOUT(!imported.isEmpty(),10000);
    auto actual=imported.last()[1].toMap();QVERIFY2(actual["ok"].toBool(),qPrintable(actual["error"].toString()));auto id=actual["result"].toMap()["appearance"].toString();QVERIFY(id.startsWith("custom-"));QVERIFY(pet->ui("selectAppearance",{{"id",id}})["ok"].toBool());QVERIFY(pet->state()["source"].toString().endsWith(".png"));QVERIFY(!pet->state()["animated"].toBool());
    QVERIFY(pet->ui("removeAppearance",{{"id",id}})["ok"].toBool());QCOMPARE(pet->config()["appearance"].toString(),"yanami");QVERIFY(!pet->ui("removeAppearance",{{"id","yanami"}})["ok"].toBool());
    auto zipPath=temp.filePath("unsafe.zip");mz_zip_archive zip{};QVERIFY(mz_zip_writer_init_file(&zip,zipPath.toUtf8().constData(),0));QVERIFY(mz_zip_writer_add_mem(&zip,"../escape.png","invalid",7,MZ_DEFAULT_COMPRESSION));QVERIFY(mz_zip_writer_finalize_archive(&zip));mz_zip_writer_end(&zip);
    imported.clear();QVERIFY(pet->ui("importAppearance",{{"path",zipPath}})["ok"].toBool());QTRY_VERIFY_WITH_TIMEOUT(!imported.isEmpty(),10000);QVERIFY(!imported.last()[1].toMap()["ok"].toBool());QVERIFY(!QFileInfo::exists(temp.filePath("escape.png")));
  }
  void whalePetDialogueUsesActualTaskOutcomes() {
    auto pet=qobject_cast<DesktopPet*>(backend->desktopExtensions->pet());QVERIFY(pet);
    const auto original=pet->config();
    auto task=std::make_shared<Session>();task->id="whale-dialogue-fixture";
    backend->records[task->key]=task;
    const auto restore=qScopeGuard([&]{backend->records.remove(task->key);pet->ui("configure",original);pet->sample();});
    QVERIFY(pet->ui("configure",{{"appearance","whale-girl"}})["ok"].toBool());
    const auto dialogue=U::readJson(root+"/umeko-plugins/desktop-pet/assets/whale-girl/pet.json").toMap()["dialogue"].toMap();
    QSet<QString> lines;for(auto group:dialogue)for(auto line:group.toList())lines.insert(line.toString());
    QCOMPARE(lines,(QSet<QString>{"要米","米","饿，饿","米呢","好了米","有问题了","米？","没问题了米","米！"}));
    QSignalSpy spoken(pet,&DesktopPet::reaction);
    for(const auto &kind:QStringList{"tap","hover","drag"}) {
      pet->interact(kind);QVERIFY(!spoken.isEmpty());
      QVERIFY(dialogue[kind].toList().contains(spoken.last()[0]));
    }
    spoken.clear();pet->sample();QVERIFY(spoken.isEmpty());
    task->phase="running";pet->sample();task->phase="idle";pet->sample();
    QCOMPARE(spoken.count(),1);QVERIFY(dialogue["done"].toList().contains(spoken.first()[0]));
    pet->sample();QCOMPARE(spoken.count(),1);
    task->phase="running";pet->sample();task->phase="idle";task->failed=true;pet->sample();
    QCOMPARE(spoken.count(),2);QCOMPARE(spoken.last()[0].toString(),QString("有问题了"));
    pet->sample();QCOMPARE(spoken.count(),2);
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
