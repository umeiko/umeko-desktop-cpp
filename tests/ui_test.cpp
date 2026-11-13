#include "terminal_view.hpp"
#include "widget_channel.hpp"
#include "widget_host.hpp"
#include "input_method.hpp"
#include <QDirIterator>
#include <QQmlComponent>
#include <QQuickView>
#include "desktop_pet.hpp"
#include "global_agents.hpp"
#include "backend.hpp"
#include "miniz.h"
#include "syntaxhighlighter.hpp"
#include "text_spacing.hpp"
#include "update_network_fixture.hpp"
#include <QDesktopServices>
#include <QImageReader>
#include <QAbstractItemModel>
#include <QAbstractItemModelTester>
#include <QJSValue>
#include <QQmlExpression>
#include <QQmlIncubationController>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QQuickStyle>
#include <QWheelEvent>
#include <QtTest>

class EditorSdk : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool active READ active NOTIFY activeChanged)
  Q_PROPERTY(double scale READ scale CONSTANT)
  Q_PROPERTY(QString language READ language NOTIFY languageChanged)
  Q_PROPERTY(QVariantMap colors READ colors CONSTANT)
  Q_PROPERTY(QVariantMap hostCapabilities READ hostCapabilities CONSTANT)
public:
  bool active() const { return false; }
  double scale() const { return 1.0; }
  QString language() const { return "en"; }
  QVariantMap colors() const {
    return {{"surface", "#202521"}, {"bg", "#181d19"}, {"ink", "#e4e9e3"},
            {"muted", "#929e94"}, {"accent", "#9cc69f"},
            {"accentBg", "#334636"}, {"hover", "#29362b"}, {"line", "#465148"}};
  }
  QVariantMap hostCapabilities() const { return {{"operations", QVariantMap{}}}; }
signals:
  void activeChanged();
  void languageChanged();
};

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
  bool globalBridge = false;
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
    // Status replies can replace ListView delegates during an event-loop wait.
    // Resolve again after each wait instead of retaining a destroyed item.
    QTest::qWait(150);
    auto item=find(name);QVERIFY2(item,qPrintable(name));
    QVERIFY(item->isVisible() && item->width()>0 && item->height()>0);
    movePointer(point(item));QTest::qWait(40);
    item=find(name);QVERIFY2(item,qPrintable(name));
    QVERIFY(item->isVisible() && item->width()>0 && item->height()>0);
    QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,point(item));
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
                                             {"reasoningLevels",QJsonArray{"low","high","max"}},
                                             {"reasoningLevelsAuto",false},{"reasoningFormatAuto",false},
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
  qmlRegisterType<TextSpacing>("Umeko.Text",1,0,"TextSpacing");
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
          if(globalBridge) {
            auto msgs=body["messages"].toList();auto last=msgs.isEmpty()?QVariantMap{}:msgs.last().toMap();
            auto value=last["content"];QString content=value.toString();
            if(value.metaType().id()==QMetaType::QVariantList)for(auto v:value.toList())content+=v.toMap()["text"].toString();
            auto request=QRegularExpression("requestId=([a-f0-9]{32})").match(content);
            QVariantMap call;
            if(last["role"]!="tool"&&request.hasMatch())call={{"operation","agent_bus.reply"},{"arguments",QVariantMap{{"requestId",request.captured(1)},{"status","completed"},{"result","Native bridge result from the actual target session"}}}};
            else if(last["role"]!="tool"&&content.startsWith("coordinate "))call={{"operation","agent_bus.dispatch"},{"arguments",QVariantMap{{"targetId",content.mid(11)},{"text","Check the native agent bridge and return its result"}}}};
            if(!call.isEmpty()) {
              auto args=QString::fromUtf8(QJsonDocument::fromVariant(call).toJson(QJsonDocument::Compact));
              message={{"role","assistant"},{"content",QVariant()},{"tool_calls",QVariantList{QVariantMap{{"id","bridge-"+U::uuid()},{"type","function"},{"function",QVariantMap{{"name","umeko_desktop"},{"arguments",args}}}}}}};finish="tool_calls";
            } else {message={{"role","assistant"},{"content",content.contains("Agent Bus result [completed]")?"GLOBAL_BRIDGE_SUMMARY_COMPLETE\n\n```python\nprint(1)\n```":"Waiting for the bridge result"},{"reasoning_content","Check the correlated request and host-assigned identity."}};finish="stop";}
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
  void sidebarContextMenuPlacement() {
    const auto previous=backend->view;const auto savedIndex=backend->sessionIndex;
    const auto savedProjects=backend->store.data["projects"];
    const auto scale=backend->fontScale();const auto language=backend->language();
    const auto size=window->size();
    const auto workspace=directory.filePath("menu-workspace");QDir().mkpath(workspace);
    const auto project=backend->ensureProject(workspace);
    backend->sessionIndex.clear();
    for(int i=0;i<30;++i)backend->sessionIndex<<QVariantMap{{"id","menu-session-"+QString::number(i)},{"title",QString("Context menu fixture %1").arg(i,2,10,QChar('0'))},{"workspace",workspace}};
    backend->activate(backend->fresh(workspace));
    QTRY_VERIFY(find("sidebarRow_menu-session-0"));
    // A preceding search may have just reopened the animated sidebar.
    QTest::qWait(300); window->grabWindow();
    auto menu=window->findChild<QObject*>("sessionContextMenu");
    auto projectMenu=window->findChild<QObject*>("projectContextMenu");
    QVERIFY(menu);QVERIFY(projectMenu);
    const auto initial=menu->property("implicitHeight").toDouble();
    const auto openRow=[&](QString name){auto row=find(name);const auto cursor=point(row);QTest::mouseClick(window,Qt::RightButton,Qt::NoModifier,cursor);return cursor;};
    const auto verify=[&](QObject *popup,QPoint cursor,bool exact){
      QTRY_VERIFY(popup->property("visible").toBool());QTest::qWait(140);
      const auto x=popup->property("x").toDouble(),y=popup->property("y").toDouble();
      const auto w=popup->property("width").toDouble(),h=popup->property("height").toDouble();
      qInfo()<<popup->objectName()<<"cursor:"<<cursor<<"rect:"<<QRectF(x,y,w,h)<<"anchored:"<<popup->property("positionFromAnchor")<<"request:"<<popup->property("requestedPosition");
      QVERIFY2(x>=9 && y>=9,"Context menu is outside the top/left window edge");
      QVERIFY2(x+w<=window->width()-9 && y+h<=window->height()-9,"Context menu overflows the window");
      if(exact){QVERIFY(qAbs(x-cursor.x())<2);QVERIFY2(qAbs(y-cursor.y())<2,"Cold popup lost the right-click anchor");}
      else{QVERIFY(x<=cursor.x()+1 && cursor.x()-x<=w+12);QVERIFY(y<=cursor.y()+1 && cursor.y()-y<=h+12);}
    };
    const auto close=[&](QObject *popup){QVERIFY(QMetaObject::invokeMethod(popup,"close"));QTRY_VERIFY(!popup->property("visible").toBool());QTest::qWait(25);};
    const auto cursor=openRow("sidebarRow_menu-session-0");verify(menu,cursor,true);close(menu);
    verify(menu,openRow("sidebarRow_menu-session-0"),true);close(menu);
    verify(projectMenu,openRow("sidebarRow_"+project["id"].toString()),true);close(projectMenu);
    backend->setFontScale(2);backend->setLanguage("en");QTest::qWait(100);
    verify(menu,openRow("sidebarRow_menu-session-0"),false);close(menu);
    backend->setFontScale(1);backend->setLanguage("zh");QTest::qWait(100);
    QVERIFY(QMetaObject::invokeMethod(find("projectList"),"positionViewAtEnd"));QTest::qWait(100);
    QTRY_VERIFY(find("sidebarRow_menu-session-29"));verify(menu,openRow("sidebarRow_menu-session-29"),false);close(menu);
    window->resize(1100,720);QTest::qWait(100);
    auto owner=window->findChild<QObject*>("sidebarMenus");QVERIFY(owner);
    const QVariant row=QVariantMap{{"id","edge"},{"kind","session"},{"title","Edge"}};
    const QVariant edge=QPointF(1085,705);
    QVERIFY(QMetaObject::invokeMethod(owner,"openFor",Q_ARG(QVariant,row),Q_ARG(QVariant,edge)));
    verify(menu,QPoint(1085,705),false);close(menu);
    qInfo()<<"Sidebar cold menu height before polish:"<<initial<<"click anchor verified:"<<cursor;
    window->resize(size);backend->setFontScale(scale);backend->setLanguage(language);
    backend->sessionIndex=savedIndex;backend->store.data["projects"]=savedProjects;backend->activate(previous);
  }
  void conversationSearchSidebar() {
    auto modelTester=std::make_unique<QAbstractItemModelTester>(backend->conversationSearch,QAbstractItemModelTester::FailureReportingMode::QtTest,this);
    const auto previous=backend->view;
    const auto workspace=directory.filePath("search-workspace");QDir().mkpath(workspace);
    const auto history=directory.filePath("search-history.jsonl");
    QFile source(history);QVERIFY(source.open(QIODevice::WriteOnly));
    for(const auto entry:QVariantList{
      QVariantMap{{"type","session"},{"id","search-history"},{"cwd",workspace}},
      QVariantMap{{"type","message"},{"id","search-u"},{"parentId",QVariant{}},{"message",QVariantMap{{"role","user"},{"content","marker original question"},{"timestamp",1700000000000LL}}}},
      QVariantMap{{"type","message"},{"id","search-a"},{"parentId","search-u"},{"message",QVariantMap{{"role","assistant"},{"content","marker code answer"},{"timestamp",1700000001000LL}}}}})
      source.write(QJsonDocument::fromVariant(entry).toJson(QJsonDocument::Compact)+'\n');
    for(int i=0;i<1000;++i){
      QVariantMap entry{{"type","message"},{"id","bulk-"+QString::number(i)},
        {"parentId",i ? "bulk-"+QString::number(i-1) : "search-a"},
        {"message",QVariantMap{{"role",i%2 ? "assistant" : "user"},{"content",QString("sample ").repeated(80)},{"timestamp",1700000002000LL+i}}}};
      source.write(QJsonDocument::fromVariant(entry).toJson(QJsonDocument::Compact)+'\n');
    }
    source.close();
    backend->sessionIndex<<QVariantMap{{"id","search-history"},{"file",history},{"workspace",workspace},{"title","Search fixture"}};
    auto running=backend->fresh(workspace);running->id="search-running";running->phase="working";
    running->draft="keep my draft";running->attachments<<QVariantMap{{"id","keep-file"},{"path",history},{"name","fixture"}};
    backend->activate(running);
    click("searchChatsButton");
    QTRY_VERIFY(window->property("searchVisible").toBool());
    auto pane=find("conversationSearch");auto input=find("conversationSearchInput");QVERIFY(pane);QVERIFY(input);
    input->setProperty("text","marker");QVERIFY(QMetaObject::invokeMethod(pane,"execute"));
    QTRY_VERIFY_WITH_TIMEOUT(!backend->conversationSearch->busy(),10000);
    QCOMPARE(backend->conversationSearch->count(),2);
    modelTester.reset();
    QTimer heartbeat;heartbeat.setInterval(10);QElapsedTimer clock;clock.start();qint64 lastTick=0,maxGap=0;int ticks=0;
    connect(&heartbeat,&QTimer::timeout,this,[&]{const auto now=clock.elapsed();maxGap=qMax(maxGap,now-lastTick);lastTick=now;++ticks;});heartbeat.start();
    input->setProperty("text","sample");QVERIFY(QMetaObject::invokeMethod(pane,"execute"));
    QTRY_VERIFY_WITH_TIMEOUT(!backend->conversationSearch->busy(),15000);heartbeat.stop();
    QCOMPARE(backend->conversationSearch->count(),20000);QVERIFY(backend->conversationSearch->limitHit());
    QVERIFY(ticks>=4);QVERIFY2(maxGap<1000,"Bulk results blocked the GUI thread");
    qInfo()<<"20,000 native search hits, UI heartbeats:"<<ticks<<"maximum interval(ms):"<<maxGap;
    input->setProperty("text","marker");QVERIFY(QMetaObject::invokeMethod(pane,"execute"));
    QTRY_VERIFY_WITH_TIMEOUT(!backend->conversationSearch->busy(),10000);QCOMPARE(backend->conversationSearch->count(),2);
    QTest::keyClick(window,Qt::Key_Escape);
    QTRY_VERIFY(!window->property("searchVisible").toBool());
    QTest::keyClick(window,Qt::Key_K,Qt::ControlModifier);
    QTRY_VERIFY(window->property("searchVisible").toBool());
    QCOMPARE(input->property("text").toString(),QString("marker"));
    QCOMPARE(backend->conversationSearch->count(),2);
    // Switching focus outside the sidebar preserves the completed query.
    QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,QPoint(window->width()-60,100));
    QTRY_VERIFY(!window->property("searchVisible").toBool());
    click("searchChatsButton");
    QTRY_VERIFY(find("searchBackButton")->isEnabled());click("searchBackButton");
    QTRY_VERIFY(!window->property("searchVisible").toBool());
    QCOMPARE(input->property("text").toString(),QString());QCOMPARE(backend->conversationSearch->count(),0);
    click("searchChatsButton");input->setProperty("text","marker");QVERIFY(QMetaObject::invokeMethod(pane,"execute"));
    QTRY_COMPARE_WITH_TIMEOUT(backend->conversationSearch->count(),2,10000);
    QTRY_VERIFY_WITH_TIMEOUT(!backend->conversationSearch->busy(),10000);
    const auto out=qEnvironmentVariable("UMEKO_TEST_ARTIFACTS",root+"/artifacts/search-ui");QDir().mkpath(out);
    window->grabWindow().save(out+"/search-sidebar.png");
    const auto priorTheme=backend->store.data["theme"].toString();const auto priorScale=backend->fontScale();
    backend->setTheme("dark");backend->setFontScale(2);QTest::qWait(200);
    window->grabWindow().save(out+"/search-sidebar-dark-200.png");
    QVERIFY(input->width()>0);QVERIFY(find("searchBackButton")->isVisible());
    backend->setTheme(priorTheme);backend->setFontScale(priorScale);QTest::qWait(100);
    QTRY_VERIFY(find("searchResult_1"));click("searchResult_1");
    QTRY_COMPARE_WITH_TIMEOUT(backend->activeSession(),QString("search-history"),10000);
    QTRY_VERIFY(!window->property("searchVisible").toBool());
    QCOMPARE(running->phase,QString("working"));QCOMPARE(running->draft,QString("keep my draft"));QCOMPARE(running->attachments.size(),1);
    QVERIFY(!backend->findSession("search-history")->process); // read-only: no Pi inspect
    QCOMPARE(backend->conversationSearch->query(),QString("marker"));
    QTRY_COMPARE(window->property("searchHighlightRow").toInt(),0);
    // The shortcut can temporarily open a collapsed sidebar without changing preference.
    window->setProperty("sidebarCollapsed",true);QTest::keyClick(window,Qt::Key_K,Qt::ControlModifier);
    QTRY_VERIFY(window->property("searchVisible").toBool());
    QTRY_VERIFY(input->hasActiveFocus());
    QTest::keyClick(window,Qt::Key_Escape);QTRY_VERIFY(!window->property("searchVisible").toBool());
    QVERIFY(window->property("sidebarCollapsed").toBool());window->setProperty("sidebarCollapsed",false);
    running->phase="idle";backend->activate(previous);
  }
  void welcomeAndEggs() {
    auto mascot = find("welcomeMascot");
    QVERIFY(mascot);
    QTRY_VERIFY_WITH_TIMEOUT(mascot->property("loaded").toBool(),2000);
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
  void toolCatalogViewport() {
    auto manager = qobject_cast<PiManagement *>(backend->piManagement());
    QVERIFY(manager);
    const auto original = backend->store.data;
    QDesktopServices::setUrlHandler("https",this,"recordOpenedUrl");
    const auto previousPage = window->property("page"), previousTab = window->property("settingsTab");
    const auto restore = qScopeGuard([&] {
      QDesktopServices::unsetUrlHandler("https");
      backend->store.data = original;
      window->setProperty("page",previousPage); window->setProperty("settingsTab",previousTab);
      backend->invalidate(); backend->notify();
    });
    configureAPI();
    window->setProperty("page","settings"); window->setProperty("settingsTab",3); QTest::qWait(150);
    if (find("openPiSettingsButton")->isVisible()) click("openPiSettingsButton");
    find("piFold_kernel")->setProperty("expanded",false);
    find("piFold_extensions")->setProperty("expanded",true);
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
  }
  void nativeMultimodalSettingsNavigation() {
    window->setProperty("page","settings");window->setProperty("settingsTab",0);QTest::qWait(100);
    auto page=find("settingsPage");QVERIFY(page);page->setProperty("multimodalOpen",false);
    auto scroll=find("settingsScroll")->property("contentItem").value<QObject*>();QVERIFY(scroll);scroll->setProperty("contentY",0);
    click("multimodalSettingsEntry");QVERIFY(page->property("multimodalOpen").toBool());
    QVERIFY(find("imageGenerationSettings")->isVisible());QVERIFY(find("speechGenerationSettings")->isVisible());QVERIFY(find("videoGenerationSettings")->isVisible());
    QVERIFY(!find("commonProvidersSection")->isVisible());
    scroll->setProperty("contentY",0);click("backMultimodalSettings");QVERIFY(!page->property("multimodalOpen").toBool());QVERIFY(find("commonProvidersSection")->isVisible());
    window->setProperty("page","chat");
  }
  void nativeReasoningDiscoveryAndChatPriority() {
    configureAPI();const auto original=backend->store.data;
    const auto cleanup=qScopeGuard([&]{releaseReplies();backend->cancelReasoningDetection();backend->store.data=original;backend->notify();});
    auto providers=backend->store.data["providers"].toList();auto account=providers[0].toMap();auto models=account["models"].toList();auto model=models[0].toMap();
    model["reasoningLevelsAuto"]=true;model["reasoningFormatAuto"]=true;model["reasoningDetection"]=QVariantMap{};model["reasoningFormatDetection"]=QVariantMap{};
    models[0]=model;account["models"]=models;providers[0]=account;backend->store.data["providers"]=providers;backend->configureGateway();
    const auto id=account["id"].toString(),alias=model["alias"].toString();const auto before=requests.size();
    QVERIFY(backend->ensureModelReasoningDetection(id,alias));
    QTRY_COMPARE_WITH_TIMEOUT(backend->modelReasoningSettings(id,alias)["states"].toMap()["format"].toString(),QString("known"),15000);
    QVERIFY(requests.size()>before);
    for(int i=before;i<requests.size();++i){auto body=requests[i].toMap();QVERIFY(body["max_tokens"].toInt()<=16);QVERIFY(!body.contains("_umeko_provider"));QVERIFY(!body.contains("_umeko_reasoning_probe"));}
    holdReplies=true;QVERIFY(backend->redetectModelReasoning(id,alias));
    QTRY_VERIFY_WITH_TIMEOUT(!heldReplies.isEmpty(),5000);
    backend->send("Chat takes priority over discovery");QVERIFY(backend->busy());releaseReplies();
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),10000);QVERIFY(lastText().contains("Chat takes priority"));
  }
  void nativeMarketCachedListAndBoundary() {
    configureAPI();auto manager=backend->desktopExtensions;auto host=qobject_cast<WidgetHost*>(manager->hostServices());
    const auto cleanup=qScopeGuard([&]{manager->closePage();});
    const auto catalog=directory.filePath("widget-market/catalog.json");QDir().mkpath(QFileInfo(catalog).absolutePath());
    QVERIFY(U::writeJson(catalog,QVariantMap{{"checkedAt",QDateTime::currentMSecsSinceEpoch()/1000.0},{"responses",QVariantMap{}},{"rows",QVariantList{QVariantMap{{"id","sample-widget"},{"name","Sample Widget"},{"version","1.0.0"},{"description","Cached local test"}}}}}));
    manager->setEnabled("widget-market",true);manager->setSessionEnabled("widget-market",true);QVERIFY(manager->openPage("widget-market"));
    QTRY_VERIFY_WITH_TIMEOUT(find("widgetMarket"),10000);
    QTRY_COMPARE_WITH_TIMEOUT(find("widgetMarket")->property("rows").toList().size(),1,10000);
    QVariant description;
    const QVariant billionRow=QVariantMap{{"id","billion-context"},{"description","old wrapper-only description"}};
    QVERIFY(QMetaObject::invokeMethod(find("widgetMarket"),"description",Q_RETURN_ARG(QVariant,description),Q_ARG(QVariant,billionRow)));
    QVERIFY(description.toString().contains("上下文压缩"));QVERIFY(description.toString().contains("单独安装"));
    QSignalSpy replies(manager,&DesktopPlugins::hostResponse);
    const auto token=host->requestFor("widget-market",{{"operation","market.install"},{"arguments",QVariantMap{{"token",QString(32,'a')}}},{"session",manager->m_session}});
    QVERIFY(!token.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!replies.isEmpty(),5000);
    QVERIFY(!replies.last()[2].toMap()["ok"].toBool());
    QVERIFY(host->pendingApproval().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!find("webPane")->property("moving").toBool(),5000);
    QVERIFY(window->grabWindow().save(root+"/build/screenshots/widget-market.png"));
  }
  void publishedTagNativeFeatures() {
    const auto original=backend->store.data;const auto selected=backend->characterStyle()["id"].toString();
    const auto cleanup=qScopeGuard([&]{backend->store.data=original;backend->selectCharacter(selected);backend->notify();});
    configureAPI();
    QVERIFY(backend->saveMultimodalSettings("{\"image\":{\"count\":2,\"model\":\"image-fixture\"}}"));
    QCOMPARE(backend->config()["multimodal"].toMap()["image"].toMap()["count"].toInt(),2);
    QVERIFY(!backend->saveMultimodalSettings("{\"image\":{\"count\":2.5}}"));
    QVERIFY(!backend->saveMultimodalSettings("{\"video\":{\"seconds\":\"-1\"}}"));
    QVERIFY(!backend->saveAuxiliaryVision("{\"autoAnalyze\":\"yes\"}"));
    QVERIFY(!backend->saveAuxiliaryVision("{\"providerId\":\"missing\",\"model\":\"missing\"}"));
    QVERIFY(backend->selectCharacter("kaju"));
    QVERIFY(backend->setCharacterEnabled(false));
    QCOMPARE(backend->characterStyle()["id"].toString(),QString("system"));
    QVERIFY(!backend->removeCharacter("system"));
    QVERIFY(backend->setCharacterEnabled(true));
    QCOMPARE(backend->characterStyle()["id"].toString(),QString("kaju"));
    backend->setTheme("white");QCOMPARE(backend->config()["theme"].toString(),QString("white"));
    backend->view->approval.clear();QVERIFY(!backend->canGrantDangerousApproval());QVERIFY(backend->view->approval.isEmpty());
    backend->view->approval={{"id","fixture"},{"method","confirm"},{"dangerCategory",1}};
    QVERIFY(backend->canGrantDangerousApproval());backend->grantDangerousApproval();QVERIFY(backend->dangerousAllowed());
    backend->revokeDangerousApproval();QVERIFY(!backend->dangerousAllowed());
    auto host=qobject_cast<WidgetHost*>(qobject_cast<DesktopPlugins*>(backend->desktopPlugins())->hostServices());
    QVERIFY(host->capabilities()["operations"].toMap().contains("market.prepare"));
    QCOMPARE(host->capabilities()["operations"].toMap()["sessions.acquire"].toMap()["permission"].toString(),QString("sessions.manage"));
  }
  void pluginSessionServiceOwnsNativeConversations() {
    configureAPI();auto manager=qobject_cast<DesktopPlugins*>(backend->desktopPlugins());
    auto host=qobject_cast<WidgetHost*>(manager->hostServices());QVERIFY(host);
    const auto original=backend->store.data;const auto front=backend->view->key;
    const QString pluginId="session-task-service";
    const auto cleanup=qScopeGuard([&]{manager->setEnabled(pluginId,false);manager->uninstall(pluginId);QElapsedTimer cleanupTimer;cleanupTimer.start();while(manager->busy()&&cleanupTimer.elapsed()<5000)QTest::qWait(10);backend->store.data=original;backend->notify();});
    const auto archive=directory.filePath("session-task-service.zip");mz_zip_archive zip{};
    QVERIFY(mz_zip_writer_init_file(&zip,archive.toUtf8().constData(),0));
    QDir source(root+"/plugin-runtime/examples/session-task-service");QDirIterator files(source.path(),QDir::Files,QDirIterator::Subdirectories);
    while(files.hasNext()) {
      QFile file(files.next());const auto name=source.relativeFilePath(file.fileName());if(name.contains("__pycache__"))continue;
      QVERIFY(file.open(QIODevice::ReadOnly));auto raw=file.readAll();
      if(name=="scripts/plugin.py")raw="from umeko_plugin_sdk import Plugin\ndef create_plugin(context): return Plugin(context)\n";
      QVERIFY(mz_zip_writer_add_mem(&zip,name.toUtf8().constData(),raw.constData(),raw.size(),MZ_DEFAULT_COMPRESSION));
    }
    QVERIFY(mz_zip_writer_finalize_archive(&zip));mz_zip_writer_end(&zip);
    const auto imported=manager->importZip(QUrl::fromLocalFile(archive).toString());QVERIFY(!imported.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(),10000);QVERIFY2(manager->entries.contains(pluginId),qPrintable(manager->status()));
    manager->setEnabled(pluginId,true);
    QTRY_VERIFY_WITH_TIMEOUT(manager->workers.contains(pluginId)&&manager->workers[pluginId]->ready,10000);
    QTemporaryDir work;QVERIFY(work.isValid());
    backend->store.data["pluginSessionProfiles"]=QVariantMap{{pluginId,QVariantMap{{"fixture",QVariantMap{{"workspace",work.path()},{"model",backend->view->model},{"provider",backend->view->provider},{"tools",QStringList{"read"}},{"permissionMode","ask"},{"allowApprovals",true}}}}}};
    QSignalSpy replies(manager,&DesktopPlugins::hostResponse);
    auto call=[&](QString op,QVariantMap args){
      const auto id=host->requestFor(pluginId,{{"operation",op},{"arguments",args},{"session",QVariantMap{}}});
      if(id.isEmpty())return QVariantMap{{"ok",false},{"error","Host rejected request immediately"}};QElapsedTimer timer;timer.start();while(timer.elapsed()<20000){QCoreApplication::processEvents();for(const auto &row:replies)if(row[1].toString()==id)return row[2].toMap();QTest::qWait(10);}for(const auto &record:backend->records)qInfo()<<"Session timeout state"<<record->id<<record->phase<<record->failed<<record->status<<record->messages.size();return QVariantMap{{"ok",false},{"error","Host request timed out: "+op}};
    };
    auto acquired=call("sessions.acquire",{{"externalKey","fixture"},{"profile","fixture"}});
    QVERIFY2(acquired["ok"].toBool(),qPrintable(acquired["error"].toString()));
    const auto identity=acquired["result"].toMap();const auto handle=identity["handle"].toString(),sessionId=identity["sessionId"].toString();
    QVERIFY(identity["created"].toBool());QVERIFY(!handle.isEmpty());QCOMPARE(backend->view->key,front);
    auto record=backend->findSession(sessionId);QVERIFY(record);QVERIFY(QFileInfo(record->file).isFile());QVERIFY(!record->dangerousAllowed);
    auto repeated=call("sessions.acquire",{{"externalKey","fixture"},{"profile","fixture"}});
    QVERIFY(repeated["ok"].toBool());QCOMPARE(repeated["result"].toMap()["sessionId"].toString(),sessionId);QVERIFY(!repeated["result"].toMap()["created"].toBool());
    QVERIFY(!call("sessions.history",{{"handle","foreign"}})["ok"].toBool());
    QVERIFY(call("sessions.subscribe",{{"handle",handle}})["ok"].toBool());
    auto submitted=call("sessions.submit",{{"handle",handle},{"requestId","request-1"},{"content","plugin session fixture"}});
    QVERIFY2(submitted["ok"].toBool(),qPrintable(submitted["error"].toString()));const auto run=submitted["result"].toMap()["runId"].toString();QVERIFY(!run.isEmpty());
    auto duplicate=call("sessions.submit",{{"handle",handle},{"requestId","request-1"},{"content","must not duplicate"}});QCOMPARE(duplicate["result"].toMap()["runId"].toString(),run);
    auto result=call("sessions.wait",{{"handle",handle},{"runId",run}});
    QVERIFY2(result["ok"].toBool(),qPrintable(result["error"].toString()));QCOMPARE(result["result"].toMap()["status"].toString(),QString("completed"));QVERIFY(!result["result"].toMap()["assistantMessages"].toStringList().isEmpty());
    QCOMPARE(backend->view->key,front);auto history=call("sessions.history",{{"handle",handle},{"limit",10}});QVERIFY(history["ok"].toBool());QVERIFY(history["result"].toMap()["messages"].toList().size()>=2);
    QVERIFY(call("approvals.registerHandler",{{"handle",handle}})["ok"].toBool());
    QVERIFY(host->captureSessionApproval(record->key,{{"type","ui_request"},{"id","approval-fixture"},{"method","confirm"},{"title","Fixture approval"}}));
    auto answer=call("approvals.respond",{{"handle",handle},{"requestId","approval-fixture"},{"decision","deny"}});QVERIFY(answer["result"].toMap()["accepted"].toBool());QVERIFY(!record->dangerousAllowed);
    QVERIFY(call("sessions.release",{{"handle",handle}})["ok"].toBool());QVERIFY(!call("sessions.history",{{"handle",handle}})["ok"].toBool());
  }
  void native066SkillsSlashAndDocumentPreview() {
    const auto oldSize=window->size();const auto oldPaneWidth=window->property("webPreferredWidth");
    const auto original=backend->store.data;
    auto widgets=qobject_cast<DesktopPlugins *>(backend->desktopPlugins());
    const auto cleanup=qScopeGuard([&]{widgets->closePage();backend->store.data=original;backend->newSession();backend->notify();window->resize(oldSize);window->setProperty("webPreferredWidth",oldPaneWidth);});
    auto agent=original["agent"].toMap();const auto agentDir=directory.filePath("native066-agent");
    agent["agentDir"]=agentDir;agent["safeMode"]=false;backend->store.data["agent"]=agent;
    const auto folder=agentDir+"/skills/native-fixture";QDir().mkpath(folder);
    QFile skill(folder+"/SKILL.md");QVERIFY(skill.open(QIODevice::WriteOnly));skill.write("---\nname: native-fixture\ndescription: Native fixture.\n---\n# Fixture");skill.close();
    backend->newSession();backend->notify();window->setProperty("page","chat");window->resize(1320,900);window->setProperty("webPreferredWidth",600);
    widgets->setEnabled("skill-manager",true);widgets->setSessionEnabled("skill-manager",true);
    backend->slash->refresh();
    auto hasSkill=[&]{for(const auto &v:backend->slash->entries())if(U::cleanPath(v.toMap()["path"].toString())==U::cleanPath(folder+"/SKILL.md"))return true;return false;};
    QTRY_VERIFY_WITH_TIMEOUT(hasSkill(),10000);
    QString skillId,skillPath;for(const auto &v:backend->slash->entries())if(U::cleanPath(v.toMap()["path"].toString())==U::cleanPath(folder+"/SKILL.md")){skillId=v.toMap()["id"].toString();skillPath=v.toMap()["path"].toString();}
    QVERIFY(backend->slash->choose(skillId));QCOMPARE(backend->attachments().size(),1);QCOMPARE(backend->attachments().first().toMap()["kind"].toString(),QString("folder"));
    QCOMPARE(backend->messages().size(),0); // Selection attaches; it never sends.
    QVERIFY(widgets->openPage("skill-manager"));
    QTRY_VERIFY_WITH_TIMEOUT(find("skillManagerWidget"),10000);
    auto page=find("skillManagerWidget");
    QTRY_COMPARE_WITH_TIMEOUT(page->property("skills").toList().size(),1,10000);
    QTRY_VERIFY_WITH_TIMEOUT(!page->property("working").toBool(),10000);
    QTRY_VERIFY_WITH_TIMEOUT(page->property("preview").toMap().contains("markdown"),10000);
    QDir().mkpath(root+"/build/screenshots");QVERIFY(window->grabWindow().save(root+"/build/screenshots/native066-skills.png"));
    const auto rows=page->property("skills").toList();
    QMetaObject::invokeMethod(page,"setEnabled",Q_ARG(QVariant,rows.first()),Q_ARG(QVariant,false));
    QTRY_VERIFY_WITH_TIMEOUT(backend->view->skillModes.contains(skillPath),5000);
    QVERIFY(!backend->view->skillModes[skillPath].toBool());
    widgets->closePage();widgets->setSessionEnabled("skill-manager",false);backend->slash->invalidate();backend->slash->refresh();
    QVERIFY(!hasSkill());QVERIFY(QFile::exists(folder+"/SKILL.md"));
    QFile document(directory.filePath("native066-large.md"));QVERIFY(document.open(QIODevice::WriteOnly));
    const auto body=QByteArray("# Heading\n\n| A | B |\n|---|---|\n| Full table | Value |\n\n```cpp\nint value = 42;\n```\n\n").repeated(1200);
    document.write(body);document.close();
    backend->documents->load(document.fileName(),"markdown");
    QTRY_VERIFY_WITH_TIMEOUT(!backend->documents->state()["busy"].toBool(),10000);
    QVERIFY(backend->documents->state()["chunked"].toBool());QVERIFY(!backend->documents->state()["truncated"].toBool());
    auto blocks=qobject_cast<QAbstractItemModel *>(backend->documents->blocks());QVERIFY(blocks->rowCount()>1000);
    const auto first=blocks->data(blocks->index(0,0),Qt::UserRole+1).toMap();QVERIFY(first["html"].toString().contains("Heading"));
    backend->documents->setMode("markdown","source");backend->documents->copyAll();QCOMPARE(QGuiApplication::clipboard()->text().toUtf8(),body);
    backend->documents->cancel();
  }
  void native066LongHistoryWindow() {
    const auto cleanup=qScopeGuard([&]{backend->newSession();});
    const auto path=directory.filePath("long-native066.jsonl");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(QJsonObject{{"type","session"},{"id","native066-history"},{"cwd",directory.path()}}).toJson(QJsonDocument::Compact)+'\n');
    QString parent;
    for(int i=0;i<2400;++i){const auto id=QString::number(i);const QJsonObject message{{"role",i%2?"assistant":"user"},{"content",QJsonArray{QJsonObject{{"type","text"},{"text",QString("Message %1\n\n**Native history.**").arg(i)}}}},{"stopReason","stop"}};
      file.write(QJsonDocument(QJsonObject{{"type","message"},{"id",id},{"parentId",parent},{"message",message}}).toJson(QJsonDocument::Compact)+'\n');parent=id;}
    file.close();backend->sessionIndex.prepend(QVariantMap{{"id","native066-history"},{"file",path},{"title","Long native history"},{"workspace",directory.path()}});
    backend->setMetadata("native066-history",{{"viewport",QVariantMap{{"index",1200},{"offset",10},{"follow",false}}}});
    QSignalSpy ready(backend->previewLoader,&SessionPreview::ready);
    backend->previewLoader->prepare("native066-history",6606);QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(),10000);QVERIFY(ready.last()[1].toBool());
    QVERIFY(backend->previewLoader->activate(6606));QCOMPARE(backend->chat.rowCount(),2400);QVERIFY(backend->chat.paged());
    QTRY_VERIFY_WITH_TIMEOUT(backend->chat.rangeReady(1200,1204),5000);
    QCOMPARE(backend->scrollAnchor(),1200);QVERIFY(!backend->followTail());QVERIFY(!backend->view->process);
    QVERIFY(!backend->chat.data(backend->chat.index(1201,0),ChatModel::MessageRole).toMap()["_loading"].toBool());
    backend->chat.requestRange(2396,2399);QTRY_VERIFY_WITH_TIMEOUT(backend->chat.rangeReady(2396,2399),5000);
    QVERIFY(backend->chat.data(backend->chat.index(2399,0),ChatModel::MessageRole).toMap()["text"].toString().contains("2399"));
  }
  void sharedAgentHistoryNavigation() {
    configureAPI();backend->newSession();window->setProperty("page","chat");window->resize(1640,1040);
    auto owner=backend->view;owner->id="shared-agent-owner";
    const auto path=directory.filePath("shared-agent-history.jsonl");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(QJsonObject{{"type","session"},{"id","shared-child"},{"cwd",directory.path()}}).toJson(QJsonDocument::Compact)+'\n');
    QString parent;
    for(int i=0;i<3000;++i){auto id=QString::number(i);file.write(QJsonDocument(QJsonObject{{"type","message"},{"id",id},{"parentId",parent},{"message",QJsonObject{{"role",i%2?"assistant":"user"},{"content",QString("Question %1\n\n**Shared history**\n\n```python\nprint(%1)\n```").arg(i)},{"timestamp",1700000000000.0+i},{"stopReason","stop"}}}}).toJson(QJsonDocument::Compact)+'\n');parent=id;}file.close();
    const auto digest=U::sha([&]{QFile f(path);f.open(QIODevice::ReadOnly);return f.readAll();}());
    backend->children->merge(owner,{{"id","shared-child"},{"status","completed"},{"outputFile",path},{"avatarSlot",0},{"task","完整历史与阅读位置"}});
    auto child=qobject_cast<ConversationState*>(backend->children->conversation());QVERIFY(child);
    QElapsedTimer clock;clock.start();qint64 last=0,gap=0;QTimer heartbeat;heartbeat.setInterval(10);connect(&heartbeat,&QTimer::timeout,this,[&]{auto now=clock.elapsed();gap=qMax(gap,now-last);last=now;});heartbeat.start();
    QVariantList samples;
    for(int n=0;n<3;++n){QElapsedTimer timing;timing.start();backend->children->open("shared-child");
      QTRY_COMPARE_WITH_TIMEOUT(child->model.rowCount(),3000,10000);QTRY_VERIFY_WITH_TIMEOUT(find("subagentConversation"),10000);QTRY_VERIFY_WITH_TIMEOUT(!find("subagentConversation")->property("restoring").toBool(),10000);samples<<timing.elapsed();
      if(n==0){QVERIFY(QMetaObject::invokeMethod(find("subagentConversation"),"jumpToMessage",Q_ARG(QVariant,1200)));QTRY_VERIFY_WITH_TIMEOUT(!find("subagentConversation")->property("restoring").toBool(),5000);QTRY_VERIFY(child->model.rangeReady(1200,1203));child->saveViewport(1200,0,false);}
      else QVERIFY(!child->follow);
      backend->children->closeView();QTest::qWait(30);
    }
    heartbeat.stop();qInfo()<<"Shared child history cold/warm(ms):"<<samples<<"UI max gap(ms):"<<gap<<"platform:"<<QGuiApplication::platformName();QVERIFY(gap<1000);
    backend->children->open("shared-child");QTRY_VERIFY(find("subagentContextUsage"));
    child->usage={{"tokens",3200},{"contextWindow",10000},{"percent",32.0},{"model","child-model"}};child->canCompact=true;child->busy=false;emit child->chatChanged();
    auto originalChildCompact=child->compact;const auto restoreCompact=qScopeGuard([&]{child->compact=originalChildCompact;});int childCompactions=0;child->compact=[&]{++childCompactions;};
    QTRY_VERIFY(!find("webPane")->property("moving").toBool());auto childRing=find("subagentContextUsage");QTRY_COMPARE(childRing->property("fraction").toDouble(),.32);
    QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,point(childRing));
    auto childPopup=childRing->findChild<QObject*>("compactContextPopup");QVERIFY(childPopup);QTRY_VERIFY(childPopup->property("visible").toBool());
    auto childAction=childPopup->findChild<QQuickItem*>("compactContextAction");QVERIFY(childAction);QTRY_VERIFY(childAction->isEnabled());QTRY_VERIFY(!childPopup->property("animating").toBool());QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,point(childAction));QTRY_COMPARE(childCompactions,1);
    backend->children->closeView();
    auto manager=backend->desktopExtensions;auto g=manager->globalAgents();QVERIFY(g);QVERIFY(!manager->selectConversation("forbidden"));
    manager->setEnabled("global-agents",true);QVERIFY(manager->openPage("global-agents"));QTRY_VERIFY(find("globalAgentsPage"));
    auto created=g->ui("create",{{"name","共享历史验收"}});QVERIFY(created["ok"].toBool());auto id=created["result"].toMap()["id"].toString();auto record=g->ensure(id);record->file=path;record->usage={{"tokens",3200},{"contextWindow",10000},{"percent",32.0},{"model",record->model}};
    const auto cleanup=qScopeGuard([&]{g->ui("delete",{{"id",id}});manager->closePage();backend->newSession();});
    QVERIFY(QMetaObject::invokeMethod(find("globalAgentsPage"),"openAgent",Q_ARG(QVariant,id)));
    QTRY_COMPARE_WITH_TIMEOUT(g->presentation.model.rowCount(),3000,10000);QTRY_VERIFY(find("globalAgentConversation"));QCOMPARE(g->presentation.model.questions().size(),1500);QCOMPARE(g->presentation.usage["percent"].toDouble(),32.0);g->changed(false);QTest::qWait(100);QCOMPARE(g->presentation.model.rowCount(),3000);
    QTRY_VERIFY(find("globalAgentContextUsage"));auto ring=find("globalAgentContextUsage");QTRY_COMPARE(ring->property("fraction").toDouble(),.32);
    auto originalCompact=g->presentation.compact;const auto restoreGlobalCompact=qScopeGuard([&]{g->presentation.compact=originalCompact;});int globalCompactions=0;g->presentation.compact=[&]{++globalCompactions;};g->presentation.canCompact=true;g->presentation.busy=false;emit g->presentation.chatChanged();
    QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,point(ring));auto popup=ring->findChild<QObject*>("compactContextPopup");QVERIFY(popup);QTRY_VERIFY(popup->property("visible").toBool());auto action=popup->findChild<QQuickItem*>("compactContextAction");QVERIFY(action);QTRY_VERIFY(action->isEnabled());QTRY_VERIFY(!popup->property("animating").toBool());QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,point(action));QTRY_COMPARE(globalCompactions,1);QCOMPARE(childCompactions,1);
    g->presentation.compact=originalCompact;
    auto navigator=find("globalAgentConversationQuestions");QVERIFY(navigator);QVERIFY(QMetaObject::invokeMethod(navigator,"jump",Q_ARG(QVariant,10)));
    QTRY_VERIFY(g->presentation.model.rangeReady(20,23));QCOMPARE(g->presentation.token,"global/"+id);QVERIFY(backend->view==owner);
    int prepared=0;for(int i=0;i<3000;++i)prepared+=!g->presentation.model.data(g->presentation.model.index(i),ChatModel::MessageRole).toMap()["_loading"].toBool();QVERIFY(prepared<=256);
    QDir().mkpath(root+"/build/issue131");QVERIFY(window->grabWindow().save(root+"/build/issue131/shared-history.png"));QCOMPARE(U::sha([&]{QFile f(path);f.open(QIODevice::ReadOnly);return f.readAll();}()),digest);
  }
  void childReplyIdentityAndRequestDetails() {
    configureAPI();backend->newSession();window->setProperty("page","chat");window->resize(1640,1040);
    const auto previousCharacter=backend->characterStyle()["id"].toString();backend->selectCharacter("yanami");
    const auto cleanup=qScopeGuard([&]{upstreamError=false;backend->children->closeView();backend->newSession();backend->selectCharacter(previousCharacter);});
    auto owner=backend->view;owner->id="child-identity-owner";
    const auto writeHistory=[&](const QString &path,const QVariantList &messages){
      QFile file(path);if(!file.open(QIODevice::WriteOnly))return false;
      file.write(QJsonDocument(QJsonObject{{"type","session"},{"id","child-identity"},{"cwd",directory.path()}}).toJson(QJsonDocument::Compact)+'\n');
      QString parent;int index=0;for(const auto &message:messages){const auto id=QString::number(index++);file.write(QJsonDocument::fromVariant(QVariantMap{{"type","message"},{"id",id},{"parentId",parent},{"message",message}}).toJson(QJsonDocument::Compact)+'\n');parent=id;}return true;
    };
    const auto completed=directory.filePath("child-completed-identity.jsonl");
    QVERIFY(writeHistory(completed,{QVariantMap{{"role","user"},{"content","Check the fixture"}},QVariantMap{{"role","assistant"},{"content","Complete"},{"stopReason","stop"}}}));
    const QStringList roles{"lemon-yakishio","chika-komari","kaju-nukumizu","yumeko-shikiya"};QSet<QString> reactions;
    auto state=qobject_cast<ConversationState*>(backend->children->conversation());QVERIFY(state);
    for(int slot=0;slot<roles.size();++slot){
      const auto id="reply-role-"+QString::number(slot);backend->children->merge(owner,{{"id",id},{"avatarSlot",slot},{"status","completed"},{"outputFile",completed},{"name",roles[slot]}});backend->children->open(id);
      QTRY_COMPARE_WITH_TIMEOUT(state->model.rowCount(),2,10000);QTRY_VERIFY_WITH_TIMEOUT(find("subagentReplyMascot_1")&&find("subagentReplyMascot_1")->isVisible(),5000);
      QTRY_VERIFY_WITH_TIMEOUT(find("subagentReplyMascot_1")->property("loaded").toBool(),5000);QTRY_VERIFY(!find("webPane")->property("moving").toBool());
      auto mascot=find("subagentReplyMascot_1");QVERIFY(mascot->property("portraitSource").toUrl().toString().endsWith(roles[slot]+".png"));window->grabWindow();
      for(int tap=0;tap<3;++tap)click("subagentReplyMascot_1");
      const auto reaction=mascot->property("reaction").toString();QVERIFY(!reaction.isEmpty());QCOMPARE(reaction,mascot->property("celebrationLine").toString());QVERIFY(!reactions.contains(reaction));reactions.insert(reaction);
      backend->children->merge(owner,{{"id",id},{"status","running"}});QTRY_VERIFY(!mascot->isVisible());QCOMPARE(mascot->property("reaction").toString(),QString());backend->children->merge(owner,{{"id",id},{"status","completed"}});backend->children->closeView();
    }
    upstreamError=true;backend->send("child request-details identity fixture");QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),30000);upstreamError=false;
    backend->chat.requestRange(0,1);QTRY_VERIFY(backend->chat.rangeReady(0,1));
    const auto diagnostic=backend->chat.data(backend->chat.index(1,0),ChatModel::MessageRole).toMap()["diagnosticId"].toString();QVERIFY(!diagnostic.isEmpty());
    // Read the synthetic child's native file without asking the live Pi process
    // (which owns the real failed request) to watch an unregistered fixture ID.
    backend->newSession();owner=backend->view;
    const auto failed=directory.filePath("child-failed-identity.jsonl");const auto error="Child fixture failure [Umeko:"+diagnostic+"]";
    QVERIFY(writeHistory(failed,{QVariantMap{{"role","assistant"},{"content",error},{"stopReason","error"},{"errorMessage",error}}}));
    backend->children->merge(owner,{{"id","failed-child"},{"status","error"},{"outputFile",failed}});backend->children->open("failed-child");
    QTRY_COMPARE_WITH_TIMEOUT(state->model.rowCount(),1,10000);QTRY_VERIFY_WITH_TIMEOUT(find("subagentRequestDetails_0")&&find("subagentRequestDetails_0")->isVisible(),5000);QTRY_VERIFY(!find("webPane")->property("moving").toBool());
    click("subagentRequestDetails_0");QTRY_VERIFY(find("requestDetailsText")->isVisible());click("requestDetailsResponseTab");QVERIFY(find("requestDetailsText")->property("text").toString().contains("complete upstream error details"));QTest::keyClick(window,Qt::Key_Escape);
  }
  void transcriptDelegateSurvivesInvalidatedRole() {
    configureAPI();backend->newSession();window->setProperty("page","chat");
    QPointer<QQuickItem> shell;QVariant saved;
    const auto cleanup=qScopeGuard([&]{if(shell && saved.isValid())shell->setProperty("modelData",saved);backend->newSession();});
    backend->view->messages={QVariantMap{{"role","user"},{"text","Model reset fixture"}},QVariantMap{{"role","assistant"},{"text","Preserve this native message while the delegate detaches."},{"stopReason","stop"}}};emit backend->chatChanged();
    QTRY_VERIFY_WITH_TIMEOUT(find("chatMessage_1") && find("chatMessage_1")->isVisible(),5000);
    shell=find("chatMessage_1");while(shell && !shell->property("messageSlot").toBool())shell=shell->parentItem();QVERIFY(shell);
    saved=shell->property("modelData");QCOMPARE(saved.toMap()["text"].toString(),QString("Preserve this native message while the delegate detaches."));
    const auto before=warnings.size();const auto previousHeight=shell->height();
    // Qt can invalidate a role before its asynchronous delegate is destroyed
    // during a DSH transcript reset. Exercise that interval deterministically.
    QVERIFY(shell->setProperty("modelData",QVariant{}));QTest::qWait(40);
    QTRY_VERIFY_WITH_TIMEOUT(!find("chatMessage_1"),2000);
    QVERIFY(qAbs(shell->height()-previousHeight)<.5);
    QCOMPARE(warnings.size(),before);
    QCOMPARE(backend->view->messages.size(),2);
    QVERIFY(shell->setProperty("modelData",saved));
    QTRY_VERIFY_WITH_TIMEOUT(find("chatMessage_1") && find("chatMessage_1")->isVisible(),5000);
    QCOMPARE(find("chatMessage_1")->property("modelData").toMap()["text"],saved.toMap()["text"]);
    backend->newSession();QTRY_COMPARE(backend->chat.rowCount(),0);QTest::qWait(40);QCOMPARE(warnings.size(),before);
  }
  void publishedHistoryStress() {
    configureAPI();window->setProperty("page","chat");window->resize(1320,860);
    const auto cleanup=qScopeGuard([&]{backend->newSession();});
    const auto path=directory.filePath("published-history-stress.jsonl");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(QJsonObject{{"type","session"},{"id","published-stress"},{"cwd",directory.path()}}).toJson(QJsonDocument::Compact)+'\n');
    QString parent;
    for(int i=0;i<6000;++i){const auto id=QString::number(i);QJsonObject message{{"role",i%2?"assistant":"user"},{"content",QString("Message %1\n\n**Native history** and `code`. ").arg(i)},{"stopReason","stop"}};
      if(i==5999){message["content"]="";message["stopReason"]="error";message["errorMessage"]=QString("upstream failed ")+QString(8*1024*1024,'x')+" [Umeko:12345678-1234-1234-1234-123456789012]";}
      file.write(QJsonDocument(QJsonObject{{"type","message"},{"id",id},{"parentId",parent},{"message",message}}).toJson(QJsonDocument::Compact)+'\n');parent=id;}
    file.close();QFile intact(path);QVERIFY(intact.open(QIODevice::ReadOnly));const auto digest=U::sha(intact.readAll());intact.close();
    backend->sessionIndex.prepend(QVariantMap{{"id","published-stress"},{"file",path},{"title","Stress"},{"workspace",directory.path()}});
    backend->setMetadata("published-stress",{{"viewport",QVariantMap{{"index",5996},{"offset",0},{"follow",false}}}});
    QElapsedTimer clock;clock.start();qint64 last=0,maxGap=0;QTimer heartbeat;heartbeat.setInterval(10);
    QString phase="cold/repeated navigation",previousPhase=phase,maxGapPhase;
    connect(&heartbeat,&QTimer::timeout,this,[&]{auto now=clock.elapsed();if(now-last>maxGap){maxGap=now-last;maxGapPhase=previousPhase+" -> "+phase;}last=now;previousPhase=phase;});heartbeat.start();
    QVariantList samples;
    for(int iteration=0;iteration<4;++iteration){backend->newSession();QSignalSpy ready(backend->previewLoader,&SessionPreview::ready);QElapsedTimer timing;timing.start();
      backend->previewLoader->prepare("published-stress",7000+iteration);QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(),10000);QVERIFY(ready.last()[1].toBool());
      QVERIFY(backend->previewLoader->activate(7000+iteration));QTRY_COMPARE_WITH_TIMEOUT(backend->chat.rowCount(),6000,5000);QTRY_VERIFY_WITH_TIMEOUT(backend->chat.rangeReady(5996,5999),5000);QTRY_VERIFY_WITH_TIMEOUT(!find("conversation")->property("restoring").toBool(),5000);qInfo()<<"Restore diagnostic"<<iteration<<backend->scrollAnchor()<<find("conversation")->property("restoreIndex")<<find("conversation")->property("restoreAttempts")<<find("conversation")->property("contentY")<<find("conversation")->property("followTail");QVERIFY(backend->scrollAnchor()>5980);samples<<timing.elapsed();
      QVERIFY(backend->chat.data(backend->chat.index(5999),ChatModel::MessageRole).toMap()["text"].toString().size()<1000);
    }
    phase="rapid switching";
    QVariantList rapidSamples;QElapsedTimer rapid;rapid.start();
    for(int iteration=0;iteration<8;++iteration){QElapsedTimer call;call.start();backend->newSession();const auto freshMs=call.restart();backend->openSession("published-stress");rapidSamples<<QVariantMap{{"new",freshMs},{"open",call.elapsed()}};}
    qInfo()<<"Rapid history switching synchronous(ms):"<<rapid.elapsed()<<rapidSamples;
    QTRY_COMPARE_WITH_TIMEOUT(backend->chat.rowCount(),6000,5000);QTRY_VERIFY_WITH_TIMEOUT(!find("conversation")->property("restoring").toBool(),5000);QVERIFY(backend->scrollAnchor()>5980);
    auto view=find("conversation");QVERIFY(view);
    phase="scroll reversals";
    for(int row:QList<int>{20,5990,2500,40,5980,100,3000}){QVERIFY(QMetaObject::invokeMethod(view,"jumpToMessage",Q_ARG(QVariant,row)));QTest::qWait(35);}
    QTest::qWait(250);QVERIFY(backend->chat.rangeReady(3000,3003));
    phase="prepared-row instrumentation";QElapsedTimer probe;probe.start();
    int prepared=0;for(int i=0;i<6000;++i)prepared+=!backend->chat.data(backend->chat.index(i),ChatModel::MessageRole).toMap()["_loading"].toBool();QVERIFY(prepared<=256);
    const auto probeMs=probe.elapsed();phase="idle completion";
    QTest::qWait(150);heartbeat.stop();qInfo()<<"History heartbeat diagnostics:"<<maxGap<<maxGapPhase<<"prepared-row probe(ms):"<<probeMs;
    QVERIFY2(maxGap<1000,qPrintable(QString("UI heartbeat gap %1ms (%2)").arg(maxGap).arg(maxGapPhase)));
    QVERIFY(intact.open(QIODevice::ReadOnly));QCOMPARE(U::sha(intact.readAll()),digest);
    qInfo()<<"6000 messages + 8MiB failure; cold/repeated(ms):"<<samples<<"max UI gap(ms):"<<maxGap<<"prepared rows:"<<prepared<<"platform:"<<QGuiApplication::platformName();
  }
  void navigationKeepsDraftAndFlushesSettingsOnClose() {
    QTemporaryDir profile;QVERIFY(profile.isValid());
    Backend local(root,profile.path());
    local.setWorkspace(profile.path());
    auto saved=local.view;saved->id="navigation-persistence";saved->draft="Keep this unsent draft";
    saved->attachments<<QVariantMap{{"id","kept-file"},{"name","fixture.txt"}};
    saved->scrollIndex=1;saved->scrollY=12;saved->followTail=false;
    local.newSession();local.openSession(saved->id);
    QCOMPARE(local.view,saved);QCOMPARE(local.view->draft,QString("Keep this unsent draft"));
    QCOMPARE(local.view->attachments.size(),1);QCOMPARE(local.view->scrollIndex,1);QVERIFY(!local.view->followTail);
    local.newSession();QVERIFY(local.appearanceSave.isActive());
    // Closing before the deferred save fires must still persist navigation.
    local.close();
    const auto settings=U::readJson(profile.filePath("settings.json")).toMap();
    const auto viewport=settings["sessionMeta"].toMap()[saved->id].toMap()["viewport"].toMap();
    QCOMPARE(viewport["index"].toInt(),1);QCOMPARE(viewport["offset"].toDouble(),12.0);QVERIFY(!viewport["follow"].toBool());
    QCOMPARE(settings["workspace"].toString(),U::cleanPath(profile.path()));
  }
  void piManagerColdToolInventory() {
    const auto restoreFocus=qScopeGuard([&] {window->requestActivate();QTest::qWait(100);});
    QQmlEngine fixtureEngine;
    QQmlComponent component(&fixtureEngine, QUrl::fromLocalFile(root+"/tests/fixtures/PiManagerColdStart.qml"));
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    for (bool failed : {false, true}) {
      std::unique_ptr<QObject> fixture(component.createWithInitialProperties({{"failInventory",failed}}));
      QVERIFY2(fixture, qPrintable(component.errorString()));
      QTRY_COMPARE_WITH_TIMEOUT(fixture->property("inventoryRequests").toInt(),1,5000);
      auto page=fixture->property("page").value<QObject *>();
      QVERIFY(page);
      if (failed) {
        QTRY_VERIFY_WITH_TIMEOUT(fixture->property("registryFailed").toBool() && page->property("notice").toString().contains("失败"),5000);
      } else {
        QTRY_VERIFY_WITH_TIMEOUT(fixture->property("tagsReady").toBool(),5000);
        QVERIFY(page->property("selected").isNull());
      }
      QVERIFY(!page->property("inventoryLoading").toBool());
      const auto requests=fixture->property("statusRequests").toInt();
      QTest::qWait(900);
      QCOMPARE(fixture->property("statusRequests").toInt(),requests);
      QCOMPARE(fixture->property("inventoryRequests").toInt(),1);
    }
  }
  void sessionToolVisibilityWidget() {
    auto widgets=qobject_cast<DesktopPlugins *>(backend->desktopPlugins());
    auto host=qobject_cast<WidgetHost *>(widgets->hostServices());

    const auto oldAgent=backend->store.data["agent"];
    const auto cleanup=qScopeGuard([&]{ widgets->closePage();backend->store.data["agent"]=oldAgent; });
    const auto agentDir=directory.filePath("tool-visibility-agent"), workspace=directory.filePath("tool-visibility-workspace");
    QDir().mkpath(agentDir);QDir().mkpath(workspace);
    auto agent=oldAgent.toMap();agent["agentDir"]=agentDir;backend->store.data["agent"]=agent;
    backend->newSession();auto origin=backend->view;origin->id="tool-visibility-owner";origin->workspace=workspace;
    origin->resources["allTools"]=QVariantList{
      QVariantMap{{"name","write"},{"source","builtin:write"},{"description","Write a file"},{"exposure","direct"}},
      QVariantMap{{"name","read"},{"source","builtin:read"},{"description","Read a file"},{"exposure","direct"}},
      QVariantMap{{"name","submit_to_chatbox"},{"source","builtin:chatbox"},{"description","Display an artifact"},{"exposure","direct"}}};
    backend->setLanguage("zh");backend->notify();window->setProperty("page","chat");window->resize(1320,900);window->setProperty("webPreferredWidth",600);QTest::qWait(100);
    widgets->setEnabled("pi-manager",true);QVERIFY(widgets->openPage("pi-manager"));
    QTRY_VERIFY_WITH_TIMEOUT(find("piExtension_builtin:pi-tools"),10000);
    auto page=find("piManagerWidget");QTRY_VERIFY_WITH_TIMEOUT(!page->property("working").toBool(),10000);
    QCOMPARE(page->property("expandedId").toString(),QString());
    click("piExtension_builtin:pi-tools");
    QTRY_VERIFY_WITH_TIMEOUT(find("piToolSwitch_write") && find("piToolSwitch_write")->isVisible(),10000);
    QTRY_VERIFY_WITH_TIMEOUT(!page->property("working").toBool(),10000);
    QVERIFY(find("piToolSwitch_write")->property("checked").toBool());
    click("piToolSwitch_write");
    QTRY_COMPARE_WITH_TIMEOUT(origin->blockedTools,QStringList{"write"},5000);
    QVERIFY(host->pendingApproval().isEmpty()); // The official per-chat controls apply immediately.
    QTRY_VERIFY_WITH_TIMEOUT(!page->property("working").toBool(),5000);
    QVERIFY(!find("piToolSwitch_write")->property("checked").toBool());
    QVERIFY(find("piToolSwitch_read")->property("checked").toBool());
    QCOMPARE(backend->metadata(origin->id)["blockedTools"].toStringList(),QStringList{"write"});
    Store saved(directory.path());QCOMPARE(saved.data["sessionMeta"].toMap()[origin->id].toMap()["blockedTools"].toStringList(),QStringList{"write"});
    QCOMPARE(saved.data["sessionMeta"].toMap()[origin->id].toMap()["piCapabilities"].toMap()["preset"].toString(),QString("custom"));
    click("piPreset_full");QTRY_VERIFY_WITH_TIMEOUT(origin->blockedTools.isEmpty() && !page->property("working").toBool(),5000);
    click("piPreset_custom");QTRY_VERIFY_WITH_TIMEOUT(origin->blockedTools.contains("write") && !page->property("working").toBool(),5000);
    QTRY_VERIFY_WITH_TIMEOUT(!origin->busy(),5000);
    QString description;for(const auto &entry:origin->resources["allTools"].toList())if(entry.toMap()["name"]=="write")description=entry.toMap()["description"].toString();
    QVERIFY(!description.isEmpty());
    find("piManagerSearch")->setProperty("text",description.left(80));
    QTRY_COMPARE_WITH_TIMEOUT(find("piManagerList")->property("count").toInt(),1,5000);
    QTRY_VERIFY_WITH_TIMEOUT(!page->property("working").toBool(),5000);
    QDir().mkpath(root+"/build/screenshots");
    QVERIFY(window->grabWindow().save(root+"/build/screenshots/session-tool-visibility-list.png"));
    backend->newSession();QVERIFY(backend->view->blockedTools.isEmpty());
    backend->activate(origin);QCOMPARE(backend->view->blockedTools,QStringList{"write"});
    origin->subagentsActive=true;QVERIFY_EXCEPTION_THROWN(backend->management->setToolVisible("write",true),std::runtime_error);origin->subagentsActive=false;
    backend->management->setToolVisible("write",true);QVERIFY(origin->blockedTools.isEmpty());
  }
  void nativePiCapabilitiesReachEmbeddedRuntime() {
    auto widgets=backend->desktopExtensions;const auto oldAgent=backend->store.data["agent"];
    const auto oldSize=window->size();const auto oldWidth=window->property("webPreferredWidth");
    const auto cleanup=qScopeGuard([&]{widgets->closePage();backend->store.data["agent"]=oldAgent;window->resize(oldSize);window->setProperty("webPreferredWidth",oldWidth);});
    configureAPI();const auto agentDir=directory.filePath("pi-capabilities-agent"),work=directory.filePath("pi-capabilities-work");
    QDir().mkpath(agentDir+"/extensions");QDir().mkpath(work);
    QFile extension(agentDir+"/extensions/capability-fixture.mjs");QVERIFY(extension.open(QIODevice::WriteOnly));
    extension.write("export default pi=>{for(const name of ['fixture_one','fixture_two'])pi.registerTool({name,label:name,description:'Fixture tool '+name,parameters:{type:'object',properties:{}},execute:async()=>({content:[{type:'text',text:'fixture'}]})});};");extension.close();
    QVERIFY(U::writeJson(agentDir+"/settings.json",QVariantMap{{"extensions",QStringList{extension.fileName()}}}));
    auto agent=oldAgent.toMap();agent["agentDir"]=agentDir;agent["safeMode"]=false;backend->store.data["agent"]=agent;
    backend->newSession();auto record=backend->view;record->workspace=work;backend->notify();
    window->resize(1480,960);window->setProperty("webPreferredWidth",500);window->setProperty("page","chat");
    const auto before=requests.size();QVERIFY(widgets->openPage("pi-manager"));
    QTRY_VERIFY_WITH_TIMEOUT(find("piManagerWidget"),10000);QPointer<QQuickItem> page=find("piManagerWidget");
    const auto pageValue=[&](const char *name){return page ? page->property(name) : QVariant();};
    auto ready=[&]{return page && !record->busy() && !pageValue("working").toBool() && !pageValue("sessionBusy").toBool() && !pageValue("inventoryLoading").toBool() && backend->management->extensionsCurrent() && record->resources.contains("toolCatalog");};
    QTRY_VERIFY2_WITH_TIMEOUT(ready(),qPrintable(QString("busy=%1 working=%2 current=%3 catalog=%4 action=%5 failed=%6 status=%7 notice=%8 pending=%9").arg(record->busy()).arg((page && pageValue("working").toBool())).arg(backend->management->extensionsCurrent()).arg(record->resources.contains("toolCatalog")).arg(record->action).arg(record->failed).arg(backend->management->status()).arg((page ? pageValue("notice").toString() : QString("Widget was destroyed"))).arg(QString::fromUtf8(QJsonDocument::fromVariant((page ? pageValue("pending") : QVariant())).toJson(QJsonDocument::Compact)))),15000);QCOMPARE(pageValue("expandedId").toString(),QString());QCOMPARE(record->workspace,U::cleanPath(work));QVERIFY(page->isVisible());
    QVariantMap row;for(const auto &value:backend->management->extensions())if(value.toMap()["path"].toString().endsWith("capability-fixture.mjs"))row=value.toMap();
    QVERIFY(!row.isEmpty());find("piManagerSearch")->setProperty("text","fixture_one");
    QTRY_COMPARE_WITH_TIMEOUT(find("piManagerList")->property("count").toInt(),1,5000);QTRY_VERIFY_WITH_TIMEOUT(ready(),5000);
    find("piManagerSearch")->setProperty("text","capability-fixture");QTRY_VERIFY_WITH_TIMEOUT(pageValue("query").toString()=="capability-fixture" && ready(),5000);
    click("piExtension_"+row["id"].toString());QTRY_VERIFY_WITH_TIMEOUT(find("piToolSwitch_fixture_one"),5000);QTRY_VERIFY_WITH_TIMEOUT(ready(),5000);
    click("piToolSwitch_fixture_one");QTRY_VERIFY_WITH_TIMEOUT(ready() && !record->resources["tools"].toStringList().contains("fixture_one"),10000);
    QVERIFY(record->resources["tools"].toStringList().contains("fixture_two"));
    QTRY_VERIFY_WITH_TIMEOUT(find("piPluginSwitch_"+row["id"].toString())->isEnabled(),5000);
    click("piPluginSwitch_"+row["id"].toString());QTRY_VERIFY2_WITH_TIMEOUT(ready() && !record->resources["tools"].toStringList().contains("fixture_two"),qPrintable(QString("profile=%1 tools=%2 busy=%3 working=%4 uiBusy=%5 inventory=%6 notice=%7").arg(QString::fromUtf8(QJsonDocument::fromVariant(record->piCapabilities).toJson(QJsonDocument::Compact))).arg(record->resources["tools"].toStringList().join(",")).arg(record->busy()).arg((page && pageValue("working").toBool())).arg(pageValue("sessionBusy").toBool()).arg(pageValue("inventoryLoading").toBool()).arg((page ? pageValue("notice").toString() : QString("Widget was destroyed")))),10000);
    QTRY_VERIFY_WITH_TIMEOUT(find("piToolSwitch_fixture_two"),5000); // Cached definitions survive unloading.
    click("piPluginSwitch_"+row["id"].toString());QTRY_VERIFY_WITH_TIMEOUT(ready() && record->resources["tools"].toStringList().contains("fixture_two"),10000);
    QVERIFY(!record->resources["tools"].toStringList().contains("fixture_one"));
    find("piManagerSearch")->setProperty("text","");QTRY_VERIFY_WITH_TIMEOUT(pageValue("query").toString().isEmpty() && ready(),5000);
    click("piPreset_minimal");QTRY_VERIFY_WITH_TIMEOUT(ready() && record->piCapabilities["preset"]=="minimal",10000);
    const auto names=record->resources["tools"].toStringList();QVERIFY(names.contains("read") && names.contains("write"));QVERIFY(!names.contains("grep") && !names.contains("fixture_two"));QVERIFY(!names.contains("umeko_desktop"));
    click("piPreset_custom");QTRY_VERIFY_WITH_TIMEOUT(ready() && record->piCapabilities["preset"]=="custom",10000);
    QCOMPARE(record->piCapabilities["custom"].toMap()["base"].toString(),QString("full"));
    QVERIFY(record->resources["tools"].toStringList().contains("fixture_two"));QVERIFY(!record->resources["tools"].toStringList().contains("fixture_one"));
    QVERIFY(!record->id.isEmpty());QCOMPARE(backend->metadata(record->id)["piCapabilities"].toMap(),record->piCapabilities);
    QCOMPARE(requests.size(),before); // Inventory and loadout changes never prompt a model.
    auto process=widgets->workers.value("pi-manager")->process;widgets->closePage();
    QTRY_VERIFY_WITH_TIMEOUT(!process || process->state()==QProcess::NotRunning,5000);
    backend->newSession();QVERIFY(backend->view->piCapabilities.isEmpty());backend->activate(record);QCOMPARE(backend->view->piCapabilities["preset"].toString(),QString("custom"));
  }
  void piManagerIsOnlyACloseableWidget() {
    backend->setLanguage("zh");
    auto widgets = qobject_cast<DesktopPlugins *>(backend->desktopPlugins());
    auto host = qobject_cast<WidgetHost *>(widgets->hostServices());
    const QString id="pi-manager";
    QVERIFY(widgets->entries.contains(id));
    QVERIFY(!widgets->workers.contains(id));
    const auto originalAgent=backend->store.data["agent"];
    const auto oldWidth=window->width(), oldHeight=window->height();
    const auto cleanup=qScopeGuard([&] {
      widgets->closePage();widgets->setEnabled(id,true);
      backend->store.data["agent"]=originalAgent;
      window->resize(oldWidth,oldHeight);window->setProperty("webPreferredWidth",0);
    });
    const auto agentDir=directory.filePath("widget-manager-agent"), workspace=directory.filePath("widget-manager-project");
    QDir().mkpath(agentDir);QDir().mkpath(workspace);
    auto agent=originalAgent.toMap();agent["safeMode"]=false;agent["agentDir"]=agentDir;
    backend->store.data["agent"]=agent;
    backend->newSession();backend->view->workspace=workspace;
    backend->notify();
    window->setProperty("page","settings");window->setProperty("settingsTab",3);QTest::qWait(100);
    if(find("openPiSettingsButton")->isVisible())click("openPiSettingsButton");
    QVERIFY(!find("piFold_packages"));
    window->setProperty("page","chat");window->resize(1120,720);window->setProperty("webPreferredWidth",320);
    widgets->setEnabled(id,true);
    QVERIFY(widgets->openPage(id));
    QTRY_VERIFY_WITH_TIMEOUT(find("piManagerWidget"),10000);
    auto page=find("piManagerWidget");
    QTRY_VERIFY_WITH_TIMEOUT(find("piManagerList")->property("count").toInt()>=7,20000);
    QTRY_VERIFY_WITH_TIMEOUT(!page->property("working").toBool(),10000);
    QDir().mkpath(root+"/build/screenshots");
    QVERIFY(window->grabWindow().save(root+"/build/screenshots/pi-widget-list.png"));
    find("piManagerSearch")->setProperty("text","chatbox");
    QTRY_COMPARE_WITH_TIMEOUT(find("piManagerList")->property("count").toInt(),1,10000);
    QTRY_VERIFY_WITH_TIMEOUT(!page->property("working").toBool(),10000);
    QVariantMap row;
    for(const auto &value:backend->management->extensions())if(value.toMap()["path"]=="builtin:chatbox")row=value.toMap();
    QVERIFY(!row.isEmpty());
    // A bounded long Markdown fixture exercises the actual Widget scroll container.
    backend->management->loadExtensionDetails(row["id"].toString());
    QTRY_VERIFY_WITH_TIMEOUT(!backend->management->busy(),10000);
    click("piExtensionDetails_"+row["id"].toString());
    QTRY_VERIFY_WITH_TIMEOUT(find("piManagerReadme")->property("text").toString().contains("submit_to_chatbox"),10000);
    QTRY_VERIFY_WITH_TIMEOUT(!page->property("working").toBool(),10000);
    auto scroll=find("piManagerDetailScroll"), readme=find("piManagerReadme");
    QVERIFY(scroll->clip());
    QVERIFY(readme->width()<=scroll->width());
    QVERIFY(scroll->mapToItem(page,scroll->boundingRect().bottomRight()).y()<=page->height()+1);
    page->setProperty("details",QVariantMap{{"markdown",QString("# Local Markdown\n\n## Section\n\nText within the viewport.\n\n").repeated(150)}});
    QTest::qWait(200);
    QVERIFY(scroll->property("contentHeight").toDouble()>scroll->height()*2);
    QVERIFY(window->grabWindow().save(root+"/build/screenshots/pi-widget-detail-scroll.png"));
    QVERIFY(!row["info"].toMap()["required"].toBool());
    QVERIFY(find("piManagerSessionSwitch")->isEnabled());
    QVERIFY(find("piManagerSessionSwitch")->property("checked").toBool());
    click("piManagerSessionSwitch");
    QTRY_VERIFY_WITH_TIMEOUT(!page->property("working").toBool(),5000);
    QVERIFY(!find("piManagerSessionSwitch")->property("checked").toBool());
    QVERIFY(!backend->view->piCapabilities["custom"].toMap()["plugins"].toMap()["builtin:chatbox"].toBool());
    click("piManagerSessionSwitch");
    QTRY_VERIFY_WITH_TIMEOUT(!page->property("working").toBool(),5000);
    QVERIFY(find("piManagerSessionSwitch")->property("checked").toBool());
    click("piManagerBack");
    find("piManagerSearch")->setProperty("text","mcp");
    QTRY_COMPARE_WITH_TIMEOUT(find("piManagerList")->property("count").toInt(),1,10000);
    for(const auto &value:backend->management->extensions())if(value.toMap()["path"]=="builtin:mcp")row=value.toMap();
    QTRY_VERIFY_WITH_TIMEOUT(find("piExtensionDetails_"+row["id"].toString()),5000);
    QTRY_VERIFY_WITH_TIMEOUT(!page->property("working").toBool(),5000);
    click("piExtensionDetails_"+row["id"].toString());
    QTRY_VERIFY_WITH_TIMEOUT(!page->property("working").toBool(),10000);
    QVERIFY(find("piManagerSessionSwitch")->isEnabled());
    click("piManagerSessionSwitch");
    QTRY_COMPARE_WITH_TIMEOUT(backend->view->piCapabilities["preset"].toString(),QString("custom"),5000);
    QTRY_VERIFY_WITH_TIMEOUT(backend->view->piCapabilities["custom"].toMap()["plugins"].toMap().contains("builtin:mcp"),5000);
    QVERIFY(!backend->view->piCapabilities["custom"].toMap()["plugins"].toMap()["builtin:mcp"].toBool());
    QVERIFY(host->pendingApproval().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!page->property("working").toBool(),10000);
    QVERIFY(!QFile::exists(agentDir+"/settings.json"));
    auto origin=backend->view;
    click("piManagerBack");
    QTRY_VERIFY_WITH_TIMEOUT(!page->property("working").toBool(),5000);
    click("piManagerInstall");
    find("piManagerInstallCommand")->setProperty("text","pi install npm:fixture && whoami");
    click("piManagerInstallConfirm");
    QVERIFY(host->pendingApproval().isEmpty());QVERIFY(!backend->management->busy());
    const auto package=directory.filePath("a quoted widget package");
    QDir().mkpath(package+"/extensions");
    {QFile file(package+"/extensions/local.ts");QVERIFY(file.open(QIODevice::WriteOnly));file.write("export default () => {};\n");}
    find("piManagerInstallCommand")->setProperty("text","pi install \""+package+"\" --local");
    QCOMPARE(find("piManagerInstallScope")->property("currentIndex").toInt(),1);
    QVERIFY(!find("piManagerInstallScope")->isEnabled());
    click("piManagerInstallConfirm");
    QTRY_VERIFY_WITH_TIMEOUT(host->pendingApproval().contains("id"),2000);
    click("widgetHost_allowToolButton");
    QTRY_COMPARE_WITH_TIMEOUT(backend->management->installState(),QString("succeeded"),20000);
    QTRY_VERIFY_WITH_TIMEOUT(!page->property("working").toBool(),10000);
    QVERIFY(QFile::exists(workspace+"/.pi/settings.json"));
    // A deliberately stalled loopback registry exercises the real cancel UI.
    QTcpServer registry;QVERIFY(registry.listen(QHostAddress::LocalHost,0));
    QList<QTcpSocket*> sockets;int received=0;
    connect(&registry,&QTcpServer::newConnection,this,[&] {
      while(registry.hasPendingConnections()) {
        auto socket=registry.nextPendingConnection();sockets<<socket;
        connect(socket,&QTcpSocket::readyRead,socket,[&,socket]{socket->readAll();++received;});
      }
    });
    const auto closeRegistry=qScopeGuard([&]{for(auto socket:sockets){socket->disconnect();socket->abort();}registry.close();});
    const auto npmrc=directory.filePath("widget-empty.npmrc");
    {QFile file(npmrc);QVERIFY(file.open(QIODevice::WriteOnly));}
    auto networkAgent=backend->store.data["agent"].toMap();
    networkAgent["npmRegistry"]=QString("http://127.0.0.1:%1").arg(registry.serverPort());networkAgent["npmUserconfig"]=npmrc;
    backend->store.data["agent"]=networkAgent;
    click("piManagerInstall");find("piManagerInstallCommand")->setProperty("text","npm:@fixture/waiting");
    click("piManagerInstallConfirm");QTRY_VERIFY_WITH_TIMEOUT(host->pendingApproval().contains("id"),2000);click("widgetHost_allowToolButton");
    QTRY_VERIFY_WITH_TIMEOUT(received>0,10000);
    QTRY_VERIFY_WITH_TIMEOUT(find("piManagerCancelInstall")->isVisible() && find("piManagerCancelInstall")->isEnabled(),5000);
    QVERIFY(find("piManagerNotice")->property("text").toString().contains("正在安装"));
    click("piManagerCancelInstall");
    QTRY_COMPARE_WITH_TIMEOUT(backend->management->installState(),QString("cancelled"),5000);
    QTRY_VERIFY_WITH_TIMEOUT(!page->property("working").toBool(),5000);
    // Closing the owner Widget also cancels a later, still-running install.
    const auto before=received;
    click("piManagerInstall");click("piManagerInstallConfirm");
    QTRY_VERIFY_WITH_TIMEOUT(host->pendingApproval().contains("id"),2000);click("widgetHost_allowToolButton");
    QTRY_VERIFY_WITH_TIMEOUT(received>before,10000);
    auto process=widgets->workers.value(id)->process;
    click("closeWebPaneButton");
    QTRY_VERIFY_WITH_TIMEOUT(!widgets->workers.contains(id),3000);
    QTRY_COMPARE_WITH_TIMEOUT(backend->management->installState(),QString("cancelled"),5000);
    QTRY_VERIFY_WITH_TIMEOUT(!process || process->state()==QProcess::NotRunning,3000);
    QVERIFY(widgets->openPage(id));
    QTRY_VERIFY_WITH_TIMEOUT(widgets->workers.contains(id) && widgets->workers[id]->ready,10000);
    auto restarted=widgets->workers.value(id)->process;
    widgets->setEnabled(id,false);
    QTRY_VERIFY_WITH_TIMEOUT(!widgets->workers.contains(id) && widgets->page().isEmpty(),3000);
    QTRY_VERIFY_WITH_TIMEOUT(!restarted || restarted->state()==QProcess::NotRunning,3000);
    QVERIFY(!widgets->openPage(id));
    backend->newSession();
    QVERIFY(backend->view->extensionModes.isEmpty());
    QCOMPARE(origin->piCapabilities["preset"].toString(),QString("custom"));
    QVERIFY(!origin->piCapabilities["custom"].toMap()["plugins"].toMap()["builtin:mcp"].toBool());
    QVERIFY(backend->view->piCapabilities.isEmpty());
  }
  void widgetHostServicesAreScopedAndAsynchronous() {
    backend->newSession();
    auto manager = qobject_cast<DesktopPlugins *>(backend->desktopPlugins());
    auto host = qobject_cast<WidgetHost *>(manager->hostServices());
    QVERIFY(host);
    QVERIFY(host->capabilities()["operations"].toMap().contains("pi.remove"));
    const auto source = root + "/plugin-runtime/examples/pi-management";
    const auto archive = directory.filePath("pi-management.zip");
    mz_zip_archive zip{};
    QVERIFY(mz_zip_writer_init_file(&zip, archive.toUtf8().constData(), 0));
    QDirIterator files(source, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
      const auto path = files.next();
      const auto relative = QDir(source).relativeFilePath(path);
      QVERIFY(mz_zip_writer_add_file(&zip, relative.toUtf8().constData(), path.toUtf8().constData(), nullptr, 0, MZ_BEST_COMPRESSION));
    }
    QVERIFY(mz_zip_writer_finalize_archive(&zip));
    mz_zip_writer_end(&zip);
    QVERIFY(!manager->importZip(QUrl::fromLocalFile(archive).toString()).isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!manager->busy(), 10000);
    const QString id = "pi-management-example";
    QVERIFY2(manager->entries.contains(id), qPrintable(manager->status()));
    const auto removeFixture = qScopeGuard([&] {
      if (manager->entries.contains(id)) {
        manager->closePage();
        manager->setEnabled(id, false);
        manager->uninstall(id);
      }
    });
    manager->setEnabled(id, true);
    QVERIFY(manager->openPage(id));
    QTRY_VERIFY_WITH_TIMEOUT(manager->workers.contains(id) && manager->workers[id]->ready, 10000);
    QHash<QString,QVariantMap> results;
    const auto listener = connect(manager, &DesktopPlugins::hostResponse, this,
        [&results](QString,QString request,QVariantMap value) { results[request] = value; });
    const auto cleanup = qScopeGuard([&] { disconnect(listener); });
    auto request = [&](QString operation,QVariantMap arguments = {}) {
      return host->requestFor(id, {{"operation", operation}, {"arguments", arguments}, {"session", manager->m_session}});
    };
    auto wait = [&](QString token) {
      QDeadlineTimer deadline(15000);
      while (!results.contains(token) && !deadline.hasExpired()) QTest::qWait(10);
      return results.value(token);
    };
    auto token = request("providers.list");
    QVERIFY(!wait(token)["ok"].toBool());
    token = request("pi.status", {{"limit",true}});
    QVERIFY(!wait(token)["ok"].toBool());
    backend->management->scan();
    token = request("pi.scan");
    auto accepted = wait(token);
    QVERIFY2(accepted["ok"].toBool(), qPrintable(QString::fromUtf8(QJsonDocument::fromVariant(accepted).toJson(QJsonDocument::Compact))));
    QVERIFY(!accepted["jobId"].toString().isEmpty());
    const auto job = accepted["jobId"].toString();
    QTRY_VERIFY_WITH_TIMEOUT(backend->controlJobs[job].toMap()["status"] == "done", 20000);
    token = request("pi.job", {{"id", job}});
    const auto jobs = wait(token)["jobs"].toList();
    QCOMPARE(jobs.size(), 1);
    QCOMPARE(jobs[0].toMap()["status"].toString(), QString("done"));
    token = request("pi.status", {{"limit", 2}});
    const auto snapshot = wait(token);
    QVERIFY(snapshot["ok"].toBool());
    QVERIFY(snapshot["result"].toMap()["contextCurrent"].toBool());
    QVERIFY(snapshot["result"].toMap()["extensions"].toList().size() <= 2);
    token = request("pi.install", {{"source", "npm:fixture@1.0.0"}});
    QTRY_VERIFY(host->pendingApproval().contains("id"));
    QVERIFY(!results.contains(token));
    click("widgetHost_denyToolButton");
    QCOMPARE(wait(token)["status"].toString(), QString("declined"));
    token = request("pi.enable", {{"id", "fixture"}, {"enabled", false}});
    QTRY_VERIFY(host->pendingApproval().contains("id"));
    const auto session = manager->m_session;
    manager->m_session["workspace"] = "/changed";
    host->respondApproval(true);
    QVERIFY(!wait(token)["ok"].toBool());
    manager->m_session = session;
    // Exercise a confirmed native install/remove, not just mocked dispatch.
    const auto originalAgent = backend->store.data["agent"];
    auto testAgent = originalAgent.toMap(); testAgent["safeMode"] = false;
    backend->store.data["agent"] = testAgent;
    const auto restoreAgent = qScopeGuard([&] { backend->store.data["agent"] = originalAgent; });
    const auto package = directory.filePath("widget package");
    QVERIFY(!backend->umekoRequest({{"operation","extensions.install"},{"arguments",QVariantMap{{"source",package}}}}, backend->view->key)["ok"].toBool());
    QVERIFY(!backend->umekoRequest({{"operation","extensions.install"},{"origin","widget"},{"arguments",QVariantMap{{"source",package}}}}, backend->view->key)["ok"].toBool());
    QVERIFY(QDir().mkpath(package + "/extensions"));
    { QFile file(package + "/extensions/example.ts"); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("export default () => {};\n"); }
    token = request("pi.install", {{"source", package}});
    QTRY_VERIFY(host->pendingApproval().contains("id"));
    host->respondApproval(true);
    accepted = wait(token);
    QVERIFY2(accepted["ok"].toBool(), qPrintable(accepted["error"].toString()));
    auto installJob = accepted["jobId"].toString();
    QVERIFY(!installJob.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(backend->controlJobs[installJob].toMap()["status"] == "done", 20000);
    QTRY_VERIFY_WITH_TIMEOUT(!backend->management->busy(), 20000);
    QString extension;
    for (const auto &item : backend->management->extensions())
      if (QFileInfo(item.toMap()["path"].toString()) == QFileInfo(package + "/extensions/example.ts")) extension = item.toMap()["id"].toString();
    QVERIFY2(!extension.isEmpty(), qPrintable(backend->management->status() + "\n" + QString::fromUtf8(QJsonDocument::fromVariant(backend->management->extensions()).toJson())));
    token = request("pi.remove", {{"id", extension}});
    QTRY_VERIFY(host->pendingApproval().contains("id"));
    host->respondApproval(true);
    accepted = wait(token);
    QVERIFY2(accepted["ok"].toBool(), qPrintable(accepted["error"].toString()));
    const auto removeJob = accepted["jobId"].toString();
    QVERIFY(!removeJob.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(backend->controlJobs[removeJob].toMap()["status"] == "done", 20000);
    QTRY_VERIFY_WITH_TIMEOUT(!backend->management->busy(), 20000);
    for (const auto &item : backend->management->extensions()) QVERIFY(item.toMap()["id"].toString() != extension);
    QVERIFY(QFile::exists(package + "/extensions/example.ts"));
    backend->store.data["agent"] = originalAgent;
    click("piWidgetScan");
    QTRY_VERIFY_WITH_TIMEOUT(find("piWidgetSummary")->property("text").toString().contains("共 "), 20000);
    QTest::qWait(200);
    QDir().mkpath(root + "/build/screenshots");
    QVERIFY(window->grabWindow().save(root + "/build/screenshots/widget-host-sdk.png"));
    if (qEnvironmentVariableIsSet("UMEKO_WIDGET_SDK_REVIEW")) {
      window->setTitle("Umeko C++ · Widget SDK 验收");
      QEventLoop loop;
      connect(window, &QWindow::visibleChanged, &loop, [&loop](bool visible) { if (!visible) loop.quit(); });
      loop.exec();
    }
    manager->closePage();
    const auto process = manager->workers.value(id)->process;
    manager->setEnabled(id, false);
    QTRY_VERIFY_WITH_TIMEOUT(!process || process->state() == QProcess::NotRunning, 3000);
    QVERIFY(manager->uninstall(id));
  }
  void selectedTextAttachment() {
    const auto previousSession = backend->view;
    const auto previousSettings = backend->store.data;
    const auto restore = qScopeGuard([this, previousSession, previousSettings] {
      backend->store.data = previousSettings;
      backend->activate(previousSession);
    });
    const auto project = directory.filePath("selection-project");
    QVERIFY(QDir().mkpath(project));
    backend->setWorkspace(QUrl::fromLocalFile(project).toString());
    const auto original = backend->view;
    const QString text = "中文𠮷 selected words\n\n```python\nprint(\"preserved\")\n```\n\nLast paragraph.";
    original->messages = {QVariantMap{{"role","assistant"},{"text",text},{"error",false}}};
    emit backend->chatChanged(); QTest::qWait(200);
    find("promptInput")->setProperty("text", "existing draft question");
    backend->setDraft("existing draft question");
    const auto existing = project + "/existing.txt";
    QFile existingFile(existing); QVERIFY(existingFile.open(QIODevice::WriteOnly)); existingFile.write("keep"); existingFile.close();
    QVERIFY(backend->addAttachments(QString::fromUtf8(QJsonDocument(QJsonArray{QUrl::fromLocalFile(existing).toString()}).toJson())));
    QGuiApplication::clipboard()->setText("clipboard stays intact");
    auto selection = find("chatSelection"); QVERIFY(selection);
    const QVariantMap start{{"row",0},{"block",0},{"offset",0}}, end{{"row",0},{"block",2},{"offset",5}};
    selection->setProperty("anchor",start); selection->setProperty("cursor",end);
    QMetaObject::invokeMethod(selection,"refresh");
    auto editor = find("selectText_0_0"); QVERIFY(editor);
    QTest::mouseClick(window,Qt::RightButton,Qt::NoModifier,point(editor,8,6)); QTest::qWait(100);
    auto action = find("attachSelectedText"); QVERIFY(action && action->isVisible() && action->isEnabled());
    QDir().mkpath(root+"/artifacts/screenshots");
    window->grabWindow().save(root+"/artifacts/screenshots/issue135-selection-menu.png");
    click("attachSelectedText");
    QTRY_COMPARE(backend->attachments().size(),2);
    QVERIFY(!backend->selectionAttachmentBusy());
    const auto item = backend->attachments().last().toMap();
    QCOMPARE(item["kind"].toString(),QString("file"));
    QCOMPARE(item["name"].toString(),QString("中文𠮷 selected words p.md"));
    const auto path = item["path"].toString();
    QCOMPARE(QFileInfo(path).absolutePath(),project+"/.umeko");
    QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly));
    const auto expected = QString("中文𠮷 selected words\n\nprint(\"preserved\")\n\nLast ");
    QCOMPARE(QString::fromUtf8(file.readAll()),expected); file.close();
    QCOMPARE(original->draft,QString("existing draft question"));
    QCOMPARE(find("promptInput")->property("text").toString(),original->draft);
    QCOMPARE(backend->attachments().first().toMap()["path"].toString(),existing);
    QCOMPARE(QGuiApplication::clipboard()->text(),QString("clipboard stays intact"));
    QCOMPARE(backend->messages().size(),1);
    QVERIFY(U::attachmentPrompt("follow up",backend->attachments()).contains(path));
    window->grabWindow().save(root+"/artifacts/screenshots/issue135-text-attachment.png");
    const auto raw = QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap({{"anchor",start},{"cursor",end}})).toJson());
    backend->attachChatSelection(raw);
    QTRY_COMPARE(backend->attachments().size(),3);
    QVERIFY(backend->attachments().last().toMap()["name"].toString().endsWith(" (2).md"));
    // Completion cannot reach another session, even if it finishes before navigation returns.
    backend->attachChatSelection(raw); backend->newSession();
    QTRY_VERIFY(!backend->selectionAttachmentBusy());
    QVERIFY(backend->attachments().isEmpty()); QCOMPARE(original->attachments.size(),3);
    backend->view->messages = {QVariantMap{{"role","assistant"},{"text",text}}}; emit backend->chatChanged();
    backend->attachChatSelection(raw);
    backend->view->messages = {QVariantMap{{"role","assistant"},{"text","replacement"}}}; emit backend->chatChanged();
    QTRY_VERIFY(!backend->selectionAttachmentBusy()); QVERIFY(backend->attachments().isEmpty());
    QCOMPARE(QGuiApplication::clipboard()->text(),QString("clipboard stays intact"));
    // Test write failure and attachment limit without replacing a project file.
    const auto blocked = directory.filePath("blocked-selection"); QVERIFY(QDir().mkpath(blocked));
    QFile obstacle(blocked+"/.umeko"); QVERIFY(obstacle.open(QIODevice::WriteOnly)); obstacle.write("keep"); obstacle.close();
    backend->view->workspace=blocked;
    backend->attachChatSelection(raw); QTRY_VERIFY(!backend->selectionAttachmentBusy()); QVERIFY(backend->attachments().isEmpty());
    for(int i=0;i<20;++i)backend->view->attachments<<QVariantMap{{"id",QString::number(i)}};
    backend->attachChatSelection(raw); QVERIFY(!backend->selectionAttachmentBusy()); QCOMPARE(backend->attachments().size(),20);
    backend->view->attachments.clear(); emit backend->attachmentsChanged();
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
  void sessionFilesImportArchiveDeleteAndDrop() {
    QTemporaryDir temporary;
    const auto workspace=temporary.filePath("project");QVERIFY(QDir().mkpath(workspace));
    const auto source=temporary.filePath("backup.jsonl"),bad=temporary.filePath("broken.jsonl");
    const auto stamp="2026-11-12T12:00:00Z";
    const QByteArray original=QJsonDocument(QJsonObject{{"type","session"},{"version",3},{"id","import-source"},{"timestamp",stamp},{"cwd",workspace}}).toJson(QJsonDocument::Compact)+'\n'+
      QJsonDocument(QJsonObject{{"type","message"},{"id","message-1"},{"parentId",QJsonValue::Null},{"timestamp",stamp},{"message",QJsonObject{{"role","user"},{"content","Imported conversation fixture"},{"timestamp",1}}}}).toJson(QJsonDocument::Compact)+'\n';
    {QFile file(source);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(original),original.size());}
    {QFile file(bad);QVERIFY(file.open(QIODevice::WriteOnly));file.write(original+"{\"type\":");}
    backend->archived=false;backend->setWorkspace(QUrl::fromLocalFile(workspace).toString());window->setProperty("page","chat");
    const auto project=backend->store.data["activeProject"].toString();QVERIFY(!project.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(find("sidebarRow_"+project),4000);
    auto target=find("sidebarRow_"+project);QVERIFY(target->isVisible());window->update();QTest::qWait(100);
    const auto at=point(target);QMimeData mime;mime.setUrls({QUrl::fromLocalFile(source)});
    QDragEnterEvent enter(at,Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QCoreApplication::sendEvent(window,&enter);
    QDragMoveEvent move(at,Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QCoreApplication::sendEvent(window,&move);
    auto drop=find("projectDropArea");QVERIFY(drop);QVERIFY(drop->property("containsDrag").toBool());
    QCOMPARE(drop->property("targetProjectId").toString(),project);
    QDropEvent event(QPointF(at),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QCoreApplication::sendEvent(window,&event);QVERIFY(event.isAccepted());
    QTRY_VERIFY_WITH_TIMEOUT(!backend->files->busy(),10000);QVERIFY2(backend->files->failure().isEmpty(),qPrintable(backend->files->failure().value("details").toString()));
    QVariantMap imported;for(const auto &value:backend->sessionIndex)if(value.toMap()["title"].toString().startsWith("Imported conversation"))imported=value.toMap();
    QVERIFY(!imported.isEmpty());QCOMPARE(imported["workspace"].toString(),U::cleanPath(workspace));QVERIFY(imported["id"]!="import-source");
    const auto file=imported["file"].toString(),id=imported["id"].toString();QVERIFY(QFileInfo::exists(file));
    backend->dropProjects(QString::fromUtf8(QJsonDocument(QJsonArray{QUrl::fromLocalFile(bad).toString()}).toJson()),project);
    QTRY_VERIFY_WITH_TIMEOUT(!backend->files->busy(),10000);QVERIFY(!backend->files->failure().isEmpty());
    auto notice=window->findChild<QObject*>("sessionFilesFailurePopup");QVERIFY(notice);QTRY_VERIFY(notice->property("visible").toBool());QDir().mkpath(root+"/build/issue133");QTest::qWait(150);QVERIFY(window->grabWindow().save(root+"/build/issue133/invalid-import.png"));QMetaObject::invokeMethod(notice,"close");
    backend->archiveProject(project,true);const auto normalRows=backend->projectRows();QVERIFY(std::none_of(normalRows.begin(),normalRows.end(),[&](const QVariant &v){return v.toMap()["id"]==project;}));
    backend->toggleArchived();const auto archiveRows=backend->projectRows();QVERIFY(std::any_of(archiveRows.begin(),archiveRows.end(),[&](const QVariant &v){return v.toMap()["id"]==id;}));
    backend->archiveProject(project,false);backend->toggleArchived();
    auto live=backend->fresh(workspace);live->id=id;live->file=file;live->subagentsActive=true;
    backend->removeProject(project);QVERIFY(!backend->files->busy());QVERIFY(QFileInfo::exists(file));live->subagentsActive=false;
    QDir().mkpath(file+".images");{QFile image(file+".images/private.png");QVERIFY(image.open(QIODevice::WriteOnly));image.write("private");}
    const auto child=file.left(file.size()-6)+"/tasks/child.jsonl";QDir().mkpath(QFileInfo(child).absolutePath());QVERIFY(QFile::copy(source,child));
    // Exercise the destructive confirmation through the same sidebar menu as users.
    QTRY_VERIFY(find("sidebarRow_"+project));
    QTest::mouseClick(window,Qt::RightButton,Qt::NoModifier,point(find("sidebarRow_"+project)));
    auto menu=window->findChild<QObject*>("projectContextMenu");QVERIFY(menu);QTRY_VERIFY(menu->property("visible").toBool());
    click("contextRemoveProject");auto confirmation=window->findChild<QObject*>("deleteProjectPopup");QVERIFY(confirmation);QTRY_VERIFY(confirmation->property("visible").toBool());
    QDir().mkpath(root+"/build/issue133");QTest::qWait(150);QVERIFY(window->grabWindow().save(root+"/build/issue133/delete-confirmation.png"));
    click("confirmDeleteProjectButton");QTRY_VERIFY_WITH_TIMEOUT(!backend->files->busy(),10000);
    QVERIFY2(backend->files->failure().isEmpty(),qPrintable(backend->files->failure().value("details").toString()));
    QVERIFY(!QFileInfo::exists(file));QVERIFY(!QFileInfo::exists(file+".images"));QVERIFY(!QFileInfo::exists(child));
    QVERIFY(backend->metadata(id).isEmpty());QVERIFY(QFileInfo(workspace).isDir());
    QFile untouched(source);QVERIFY(untouched.open(QIODevice::ReadOnly));QCOMPARE(untouched.readAll(),original);
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
      QTest::keyClick(restoredWindow, Qt::Key_N, Qt::ControlModifier);
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
  void emptyChatRestorationDoesNotWaitForRows() {
    const auto previous=backend->view;
    const auto savedProjects=backend->store.data["projects"];
    const auto cleanup=qScopeGuard([&]{
      QMetaObject::invokeMethod(window,"cancelSidebarSelection");
      backend->store.data["projects"]=savedProjects;backend->activate(previous);
    });
    window->setProperty("page","chat");
    const auto workspace=directory.filePath("empty-chat-restoration");QDir().mkpath(workspace);
    const auto project=backend->ensureProject(workspace);
    auto history=backend->fresh(directory.path());
    history->messages={QVariantMap{{"role","user"},{"text","Existing question"}},
                       QVariantMap{{"role","assistant"},{"text","Existing answer"}}};
    auto cover=find("chatLoadingOverlay"), view=find("conversation"), prompt=find("promptInput");
    QVERIFY(cover);QVERIFY(view);QVERIFY(prompt);
    QVariantList timings;
    for(int i=0;i<3;++i){
      backend->activate(history);
      QTRY_COMPARE_WITH_TIMEOUT(view->property("count").toInt(),2,2000);
      QTRY_VERIFY_WITH_TIMEOUT(!view->property("restoring").toBool(),2000);
      const QVariant row=QVariantMap{{"id",project["id"]},{"title","Empty project"},{"kind","project"},{"selected",false}};
      QElapsedTimer clock;clock.start();
      QVERIFY(QMetaObject::invokeMethod(window,"selectSidebar",Q_ARG(QVariant,row)));
      QVERIFY(cover->isVisible());
      QTRY_COMPARE_WITH_TIMEOUT(backend->messageCount(),0,2000);
      // The real sidebar/cover path must unlock the composer without waiting
      // for the 200-frame deadline intended for a nonempty pending model.
      QTRY_VERIFY_WITH_TIMEOUT(!cover->isVisible(),1000);
      timings<<clock.elapsed();
      QVERIFY(!view->property("restoring").toBool());
      QVERIFY(view->property("restoreAttempts").toInt()<=2);
      QVERIFY(prompt->isEnabled());QVERIFY(prompt->hasActiveFocus());
    }
    qInfo()<<"Empty project selection to input ready(ms):"<<timings;
    // A temporary model reset in a nonempty transcript still needs its rows.
    backend->activate(history);
    QTRY_VERIFY_WITH_TIMEOUT(!view->property("restoring").toBool(),2000);
    backend->saveViewport(0,4,false);
    backend->chat.switchContext("pending-nonempty-restoration");
    QCOMPARE(view->property("count").toInt(),0);QCOMPARE(backend->messageCount(),2);
    QVERIFY(QMetaObject::invokeMethod(view,"restoreViewport"));
    QTest::qWait(80);
    QVERIFY(view->property("restoring").toBool());
    QCOMPARE(backend->scrollAnchor(),0);QCOMPARE(backend->scrollPosition(),4.0);
    backend->chat.update(history->messages);
    QTRY_VERIFY_WITH_TIMEOUT(!view->property("restoring").toBool(),2000);
    QVERIFY(!backend->followTail());
    // The direct New chat action also finishes empty restoration immediately.
    QVERIFY(QMetaObject::invokeMethod(window,"startChat"));
    QCOMPARE(backend->messageCount(),0);
    QVERIFY(!view->property("restoring").toBool());
    QVERIFY(prompt->isEnabled());QVERIFY(prompt->hasActiveFocus());
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
    // Model refresh can launch background capability probes. The uninstall
    // contract rejects live provider workers, even after the refresh job ends.
    auto providers=qobject_cast<Providers*>(backend->providerPlugins());QVERIFY(providers);
    QTRY_VERIFY_WITH_TIMEOUT(([&]{for(auto process:providers->findChildren<QProcess*>())if(process->state()!=QProcess::NotRunning)return false;return true;})(),20000);
    const auto uninstalled=call("plugins.uninstall", {{"plugin", "example-job"}, {"version", "0.1.0"}});
    QVERIFY2(uninstalled["ok"].toBool(),qPrintable(report(uninstalled)));
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
  void customRequestHeadersSettings() {
    const auto saved=backend->store.data;
    const auto previousPage=window->property("page"),previousTab=window->property("settingsTab");
    const auto previousSize=window->size();
    const auto restore=qScopeGuard([&] {
      backend->store.data=saved;backend->store.save();backend->configureGateway();backend->notify();
      backend->setFontScale(1);backend->setLanguage("zh");window->resize(previousSize);
      window->setProperty("page",previousPage);window->setProperty("settingsTab",previousTab);
    });
    window->resize(1200,900);window->setProperty("page","settings");window->setProperty("settingsTab",3);QTest::qWait(150);
    click("openUmekoDesktopSettingsButton");
    QVERIFY(find("openRequestHeaders")->isVisible());
    QVERIFY(!find("requestHeadersSettings")->isVisible());
    QVERIFY(find("desktopChatWidthHints")->isVisible());
    click("openRequestHeaders");
    QVERIFY(find("customRequestsPage")->isVisible());
    QVERIFY(!find("desktopChatWidthHints")->isVisible());
    QVERIFY(!find("openRequestHeaders")->isVisible());
    QVERIFY(find("requestHeaderValue_user-agent"));
    auto editor=find("requestHeadersSettings");QVERIFY(editor);
    const auto edit=[&](QString name,QString value) {
      auto field=find(name);QVERIFY(field);field->forceActiveFocus();
      QTest::keyClick(window,Qt::Key_A,Qt::ControlModifier);QInputMethodEvent typing;typing.setCommitString(value);QCoreApplication::sendEvent(field,&typing);QTest::keyClick(window,Qt::Key_Tab);
    };
    edit("requestHeaderValue_user-agent","My-Client/1.0");
    QVERIFY(editor->property("dirty").toBool());
    click("backFromRequestHeaders");
    QVERIFY(!editor->isVisible());QVERIFY(find("desktopChatWidthHints")->isVisible());
    QVERIFY(editor->property("dirty").toBool());
    click("openRequestHeaders");
    QCOMPARE(find("requestHeaderValue_user-agent")->property("text").toString(),QString("My-Client/1.0"));
    QVERIFY(QMetaObject::invokeMethod(editor,"save",Q_ARG(QVariant,false)));
    QCOMPARE(backend->requestHeaders()[1].toMap()["value"].toString(),QString("My-Client/1.0"));
    Store reloaded(backend->store.directory);QCOMPARE(reloaded.data["requestHeaders"].toList(),backend->requestHeaders());
    click("requestHeaderEnabled_user-agent");
    QVERIFY(QMetaObject::invokeMethod(editor,"save",Q_ARG(QVariant,false)));
    QVERIFY(!backend->requestHeaders()[1].toMap()["enabled"].toBool());
    QVERIFY(QMetaObject::invokeMethod(find("addRequestHeader"),"clicked"));QTest::qWait(50);
    QVERIFY(editor->property("dirty").toBool());
    QVERIFY(QMetaObject::invokeMethod(editor,"save",Q_ARG(QVariant,false))); // Blank name is rejected, draft stays visible.
    QCOMPARE(backend->requestHeaders().size(),5);QVERIFY(editor->property("dirty").toBool());
    QVERIFY(QMetaObject::invokeMethod(find("discardRequestHeaderChanges"),"clicked"));
    QVERIFY(!editor->property("dirty").toBool());
    for (const auto &language:QStringList{"zh","en"}) {
      backend->setLanguage(language);backend->setFontScale(language=="en"?1.6:1);window->resize(language=="en"?900:1200,900);QTest::qWait(150);
      for (const auto &name:QStringList{"requestHeaderName_user-agent","requestHeaderValue_user-agent","requestHeaderEnabled_authorization","saveRequestHeaders"}) {
        const auto item=find(name);QVERIFY(item);QVERIFY(item->isVisible());
        const auto bounds=item->mapRectToItem(editor,item->boundingRect());QVERIFY(bounds.left()>=0);QVERIFY(bounds.right()<=editor->width()+1);
      }
      QDir().mkpath(root+"/build/request-headers-ui");window->grabWindow().save(root+"/build/request-headers-ui/"+language+".png");
    }
    QVERIFY(QMetaObject::invokeMethod(editor,"save",Q_ARG(QVariant,true)));
    QCOMPARE(backend->requestHeaders()[1].toMap()["value"].toString(),QString("Umeko-Desktop"));
    const auto controlled=backend->umekoRequest({{"operation","settings.get"}})["config"].toMap()["requestHeaders"].toList();
    QVERIFY(!controlled.isEmpty());QCOMPARE(controlled[1].toMap()["value"].toString(),QString("[redacted]"));
    QVERIFY2(warnings.isEmpty(),qPrintable(warnings.join('\n')));
    click("backFromRequestHeaders");click("backFromUmekoDesktopButton");click("openUmekoDesktopSettingsButton");
    QVERIFY(find("openRequestHeaders")->isVisible());QVERIFY(!editor->isVisible());
    click("backFromUmekoDesktopButton");
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
      if(it.key().startsWith("agent_bus.")) {
        QVERIFY((QStringList{"agent_bus.overview","agent_bus.requests","agent_bus.dispatch","agent_bus.reply","agent_bus.create","agent_bus.creation_status","agent_bus.connections","agent_bus.set_connection"}.contains(it.key())));
        QCOMPARE(it.value().toMap()["target"].toString(),QString("global-agents"));
        continue; // Native, identity-bound dispatcher is exercised by globalAgentsWidgetAndNativeBridge.
      }
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
    const auto forkFile = backend->view->file;
    QVERIFY(call("sessions.delete", {{"id", fork}})["ok"].toBool());
    // Managed transcript cleanup waits for process shutdown and runs off-thread.
    QTRY_VERIFY_WITH_TIMEOUT(!backend->files->busy(), 10000);
    QVERIFY2(backend->files->failure().isEmpty(), qPrintable(report(backend->files->failure())));
    QVERIFY(!call("sessions.get", {{"id", fork}})["ok"].toBool());
    QVERIFY(!QFileInfo::exists(forkFile));
    QVERIFY(backend->metadata(fork).isEmpty());
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
  void customModelReasoningRequests() {
    configureAPI();
    const auto id = backend->config()["selectedProvider"].toString();
    auto provider = backend->provider(id);
    const auto original = provider["models"].toList();
    auto models = original;
    auto model = models[0].toMap(); model["reasoningLevels"] = QVariantList{"medium", "xhigh", "vendor-custom"}; models[0] = model;
    const auto save = [&](QVariantList values) {
      return backend->saveProvider(id, provider["name"].toString(), "openai", provider["baseUrl"].toString(), "", QString::fromUtf8(QJsonDocument::fromVariant(values).toJson()));
    };
    QVERIFY(save(models));
    QCOMPARE(backend->reasoningLevels(), (QStringList{"medium", "xhigh", "vendor-custom"}));
    QVERIFY(!backend->setReasoningEffort("high"));
    for (const auto &level : QStringList{"medium", "xhigh", "vendor-custom", "off"}) {
      QVERIFY(backend->setReasoningEffort(level)); backend->newSession();
      QCOMPARE(backend->reasoningEffort(), level);
      const auto before = requests.size(); backend->send("custom reasoning " + level);
      QTRY_VERIFY_WITH_TIMEOUT(requests.size() > before, 15000);
      QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(), 15000);
      const auto request = requests.last().toMap();
      if (level == "off") QVERIFY(!request.contains("reasoning_effort"));
      else QCOMPARE(request["reasoning_effort"].toString(), level);
    }
    Store reloaded(directory.path());
    bool persisted = false;
    for (const auto &p : reloaded.data["providers"].toList()) if (p.toMap()["id"] == id)
      persisted = U::reasoningLevels(p.toMap()["models"].toList()[0].toMap()) == backend->reasoningLevels();
    QVERIFY(persisted);
    QVERIFY(backend->setReasoningEffort("xhigh"));
    model["reasoningLevels"] = QVariantList{}; models[0] = model; QVERIFY(save(models));
    QVERIFY(backend->reasoningLevels().isEmpty()); QCOMPARE(backend->reasoningEffort(), QString("off"));
    QVERIFY(!backend->setReasoningEffort("xhigh"));
    auto restored = original; auto defaults = restored[0].toMap();
    defaults["reasoningLevels"] = QStringList{"low", "high", "max"}; restored[0] = defaults;
    QVERIFY(save(restored));
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
    QVERIFY(!window->findChild<QObject *>("reasoningEffortHint"));
    auto slider=find("reasoningEffortSlider");QVERIFY(slider);
    QCOMPARE(slider->property("value").toDouble(),0.0);
    QTest::mouseMove(window, point(find("reasoningControls")));
    QTest::qWait(300);
    QVERIFY(!window->findChild<QObject *>("reasoningEffortHint"));
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
  void compactionTaskStatusAndPermissionControls() {
    const auto oldAgent=backend->store.data["agent"].toMap();
    const auto restore=qScopeGuard([&]{backend->store.data["agent"]=oldAgent;backend->store.save();emit backend->changed();window->setProperty("page","chat");backend->newSession();});
    backend->newSession();auto session=backend->view;session->id=U::uuid();session->file=directory.filePath("compaction.jsonl");session->model="fixture";
    session->action="compact";session->phase="running";QVariantMap event{{"type","compaction_event"},{"version",2},{"eventId",U::uuid()},{"state","running"},{"mode","manual"},{"before",10000},{"after",QVariant()},{"afterEstimated",true},{"summary",""},{"error",""},{"entryId","start"},{"timestamp",U::stamp()}};
    backend->event(session,event);QCOMPARE(session->messages.size(),1);QVERIFY(backend->compacting());QTest::qWait(200);
    auto card=find("compactionEvent_0");QVERIFY(card && card->isVisible());QVERIFY(!find("assistantAvatar_0") || !find("assistantAvatar_0")->isVisible());
    event["state"]="success";event["entryId"]="end";event["after"]=2000;event["summary"]="Fixture summary";backend->event(session,event);
    QCOMPARE(session->messages.size(),1);QCOMPARE(session->messages.first().toMap()["entryId"].toString(),QString("start"));
    click("compactionEventToggle_0");QVERIFY(find("compactionEventDetails_0")->property("text").toString().contains("Fixture summary"));
    backend->event(session,{{"type","done"}});QVERIFY(!backend->compacting());QVERIFY(backend->turnStatus()["state"]!="finished");
    for(const auto &outcome:QStringList{"finished","failed","stopped"}) {
      session->action="prompt";session->phase="running";session->promptStarted=std::chrono::steady_clock::now()-std::chrono::seconds(5);session->failed=outcome=="failed";session->cancelled=outcome=="stopped";
      QCOMPARE(backend->turnStatus()["state"].toString(),outcome=="stopped"?QString("stopping"):QString("running"));
      backend->event(session,{{"type","done"}});QCOMPARE(backend->turnStatus()["state"].toString(),outcome);QVERIFY(backend->turnStatus()["elapsed"].toDouble()>=5);
      backend->markRead(session);QCOMPARE(backend->turnStatus()["state"].toString(),outcome);
    }
    window->setProperty("page","settings");window->setProperty("settingsTab",3);QTest::qWait(200);
    click("openPiSettingsButton");find("piFold_kernel")->setProperty("expanded",false);find("piFold_runtime")->setProperty("expanded",true);QTest::qWait(200);
    auto flick=find("settingsScroll")->property("contentItem").value<QObject*>();QVERIFY(flick);flick->setProperty("contentY",0);
    click("toolPermission_ask");QCOMPARE(backend->store.data["agent"].toMap()["toolPermissionMode"].toString(),QString("ask"));
    Store saved(directory.path());QCOMPARE(saved.data["agent"].toMap()["toolPermissionMode"].toString(),QString("ask"));
    click("toolPermission_auto");QCOMPARE(backend->store.data["agent"].toMap()["toolPermissionMode"].toString(),QString("auto"));
    backend->setToolPermissionMode("invalid");QCOMPARE(backend->store.data["agent"].toMap()["toolPermissionMode"].toString(),QString("auto"));
  }
  void appearanceFontShortcutsAndHiddenLayout() {
    const auto size=window->size();const auto scale=backend->fontScale();const auto family=backend->fontFamily(),spacing=backend->lineSpacing();
    const auto restore=qScopeGuard([&]{backend->setFontScale(scale);backend->setLineSpacing(spacing);backend->setFontFamily(family);window->resize(size);window->setProperty("page","chat");});
    window->resize(1480,1100);window->setProperty("page","settings");window->setProperty("settingsTab",5);backend->setFontScale(1);QTest::qWait(200);
    for(int value:{75,100,125}) {
      click("fontPreset_"+QString::number(value));QCOMPARE(qRound(backend->fontScale()*100),value);
      QCOMPARE(find("fontScaleValue")->property("value").toInt(),value);
      QCOMPARE(find("fontScaleLabel")->property("text").toString(),QString::number(value)+"%");
    }
    backend->setFontScale(.3);QCOMPARE(backend->fontScale(),.3);backend->setFontScale(1);QTest::qWait(200);
    auto prompt=find("promptInput"),about=find("aboutVersion");QVERIFY(prompt && about);QVERIFY(!prompt->isVisible() && !about->isVisible());
    const auto signal=[](QObject *item){return item->metaObject()->property(item->metaObject()->indexOfProperty("font")).notifySignal();};
    QSignalSpy promptChanges(prompt,signal(prompt)),aboutChanges(about,signal(about));
    QElapsedTimer timing;timing.start();
    for(int value=75;value<=125;++value){backend->setFontScale(value/100.);QCoreApplication::processEvents();}
    qInfo()<<"51 font-size updates (ms):"<<timing.elapsed()<<"hidden font changes:"<<promptChanges.size()<<aboutChanges.size();
    QCOMPARE(promptChanges.size(),0);QCOMPARE(aboutChanges.size(),0);
    window->setProperty("page","chat");QTest::qWait(150);QCOMPARE(prompt->property("font").value<QFont>().pixelSize(),16);
    backend->newSession();backend->view->messages={QVariantMap{{"role","assistant"},{"text","Paragraph one.\n\n- List item\n\n```python\nprint('code')\n```"}}};emit backend->chatChanged();
    auto visibleProse=[&]() -> QQuickItem* {QList<QQuickItem*> pending{window->contentItem()};while(!pending.isEmpty()){auto item=pending.takeLast();if(item->objectName()=="selectText_0_0" && item->isVisible() && item->property("text").toString().contains("Paragraph one"))return item;pending.append(item->childItems());}return nullptr;};
    QTRY_VERIFY_WITH_TIMEOUT(visibleProse(),5000);auto prose=visibleProse();
    auto document=prose->property("textDocument").value<QQuickTextDocument*>();QVERIFY(document);
    for(const auto &value:QStringList{"compact","standard","relaxed"}) {
      backend->setLineSpacing(value);QTest::qWait(50);
      QCOMPARE(document->textDocument()->begin().blockFormat().lineHeight(),value=="compact"?100.:value=="standard"?120.:150.);
    }
    backend->setFontFamily("serif");QTest::qWait(150);QCOMPARE(prose->property("font").value<QFont>().family(),QString("serif"));
    QCOMPARE(find("promptInput")->property("font").value<QFont>().family(),QString("serif"));
    QTest::qWait(300);Store saved(directory.path());QCOMPARE(saved.data["fontFamily"].toString(),QString("serif"));QCOMPARE(saved.data["ui"].toMap()["lineSpacing"].toString(),QString("relaxed"));
    backend->setFontFamily("系统默认");QVERIFY(backend->fontFamily().isEmpty());
    window->setProperty("page","settings");window->setProperty("settingsTab",5);QTest::qWait(150);
    QDir().mkpath(root+"/build/screenshots");QVERIFY(window->grabWindow().save(root+"/build/screenshots/native-font-settings.png"));
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
             13);
    QCOMPARE(find("sendGlyph")->width(), 34.0);
    QCOMPARE(find("attachFilesButton")->width(), 62.0);
    window->setProperty("page","chat");QTest::qWait(150);
    QCOMPARE(find("promptInput")->property("font").value<QFont>().pixelSize(),26);
    window->setProperty("page","settings");QTest::qWait(100);
    backend->setFontScale(.5);
    QTest::qWait(200);
    QCOMPARE(find("promptInput")->property("font").value<QFont>().pixelSize(),
             13);
    window->setProperty("page","chat");QTest::qWait(150);
    QCOMPARE(find("promptInput")->property("font").value<QFont>().pixelSize(),7);
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
    for (const auto &character:QStringList{"yanami","komari","kaju","tiara","whale-girl"}) {
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
    for (const auto &id : QStringList{"yanami", "komari", "kaju", "tiara", "whale-girl", "doubao"})
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
        {"assets", QVariantMap{{"picker", "portrait.png"},
                               {"welcome", "portrait.png"},
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
    for (const auto &id : QStringList{"yanami", "komari", "kaju", "tiara", "whale-girl", "doubao"}) {
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
  void tiaraCharacterAcrossSurfaces() {
    const auto previous = backend->characterStyle()["id"].toString();
    const auto options = QJsonDocument::fromVariant(backend->characterConfiguration()["overrides"]).toJson();
    const auto page = window->property("page"), tab = window->property("settingsTab");
    auto pet = qobject_cast<DesktopPet*>(backend->desktopExtensions->pet());
    QVERIFY(pet);
    const auto petConfig = pet->config();
    const auto restore = qScopeGuard([&] {
      backend->selectCharacter(previous);
      backend->configureCharacter(QString::fromUtf8(options));
      pet->ui("configure", petConfig);
      window->setProperty("page", page); window->setProperty("settingsTab", tab);
    });
    QVERIFY(backend->configureCharacter("{}"));
    QVERIFY(backend->selectCharacter("tiara"));
    QCOMPARE(backend->characterStyle()["name"].toString(), QString("马剃天爱星"));
    QCOMPARE(backend->characterStyle()["assets"].toMap().size(), 10);
    for (auto url : backend->characterStyle()["assets"].toMap()) {
      QImage image(QUrl(url.toString()).toLocalFile());
      QVERIFY(!image.isNull()); QVERIFY(image.hasAlphaChannel());
    }
    const auto original = backend->characterStyle()["subagents"].toList();
    QVERIFY(backend->selectSubagentCharacter(2, "tiara"));
    auto roles = backend->characterStyle()["subagents"].toList();
    QCOMPARE(roles.size(), 4); QCOMPARE(roles[2].toMap()["name"].toString(), QString("马剃天爱星"));
    QCOMPARE(roles[0], original[0]);
    for (auto key : {"avatar", "catch"})
      QVERIFY(QFileInfo(QUrl(roles[2].toMap()[key].toString()).toLocalFile()).isFile());
    QVERIFY(!backend->selectSubagentCharacter(-1, "tiara"));
    QVERIFY(!backend->selectSubagentCharacter(4, "tiara"));
    QVERIFY(!backend->selectSubagentCharacter(0, "missing"));
    QCOMPARE(backend->characterStyle()["subagents"].toList(), roles);
    QVERIFY(backend->selectSubagentCharacter(2, ""));
    QCOMPARE(backend->characterStyle()["subagents"].toList(), original);
    window->setProperty("page", "settings"); window->setProperty("settingsTab", 5);
    QTest::qWait(250); click("chooseCharacterButton");
    QTRY_VERIFY(find("character_tiara") && find("character_tiara")->isVisible());
    QVERIFY(!find("deleteCharacter_tiara")->isVisible());
    click("subagentCharacterTab");
    QTRY_VERIFY(find("subagentCharacterSlot")->isVisible());
    click("subagentCharacterSlot");
    QTest::keyClick(window, Qt::Key_End);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE(find("subagentCharacterSlot")->property("currentIndex").toInt(), 3);
    click("character_tiara");
    QCOMPARE(backend->characterStyle()["id"].toString(), QString("tiara"));
    QCOMPARE(backend->characterNames()[3], QString("马剃天爱星"));
    QDir().mkpath(root + "/build/screenshots");
    QVERIFY(window->grabWindow().save(root + "/build/screenshots/tiara-subagent-picker.png"));
    click("resetSubagentCharacter");
    QCOMPARE(backend->characterStyle()["subagents"].toList(), original);
    click("mainCharacterTab");
    QVERIFY(window->grabWindow().save(root + "/build/screenshots/tiara-character-picker.png"));
    click("closeCharacterDialog");
    window->setProperty("page", "chat"); backend->newSession();
    QTRY_VERIFY(find("welcomeMascot")->property("loaded").toBool()); QTest::qWait(400);
    QVERIFY(window->grabWindow().save(root + "/build/screenshots/tiara-welcome.png"));
    bool found = false;
    for (auto value : pet->appearances()) if (value.toMap()["id"] == "tiara") {
      found = true; QVERIFY(value.toMap()["builtin"].toBool());
    }
    QVERIFY(found); QVERIFY(!pet->ui("removeAppearance", {{"id", "tiara"}})["ok"].toBool());
    const auto folder = root + "/umeko-plugins/desktop-pet/assets/tiara/";
    for (const auto &state : QStringList{"idle", "thinking", "working", "waiting", "done", "error"}) {
      QImageReader reader(folder + state + ".webp");
      QCOMPARE(reader.imageCount(), 6); auto image = reader.read();
      QVERIFY(!image.isNull()); QCOMPARE(image.size(), QSize(384, 576));
      QVERIFY(image.hasAlphaChannel()); QCOMPARE(image.pixelColor(0,0).alpha(), 0);
    }
    QVERIFY(pet->ui("configure", {{"appearance", "tiara"}, {"visible", true}, {"paused", false}})["ok"].toBool());
    QTRY_VERIFY_WITH_TIMEOUT(window->findChild<QObject*>("desktopPetWindow"), 5000);
    auto petWindow = qobject_cast<QQuickWindow*>(window->findChild<QObject*>("desktopPetWindow"));
    QVERIFY(petWindow); QTRY_VERIFY(petWindow->isVisible());
    auto animation = petWindow->findChild<QObject*>("desktopPetAnimation"); QVERIFY(animation);
    QTRY_VERIFY(animation->property("ready").toBool());
    QVERIFY(animation->property("source").toUrl().toString().contains("/tiara/"));
    const auto frame = animation->property("currentFrame").toInt();
    QTRY_VERIFY(animation->property("currentFrame").toInt() != frame);
    QVERIFY(petWindow->grabWindow().save(root + "/build/screenshots/tiara-desktop-pet.png"));
  }
  void doubaoCharacterAcrossSurfaces() {
    const auto previous = backend->characterStyle()["id"].toString();
    const auto options = QJsonDocument::fromVariant(
        backend->characterConfiguration()["overrides"]).toJson();
    const auto previousPage = window->property("page");
    const auto previousTheme = backend->store.data.value("theme", "light").toString();
    const auto restore = qScopeGuard([&] {
      backend->selectCharacter(previous);
      backend->configureCharacter(QString::fromUtf8(options));
      backend->setTheme(previousTheme);
      window->setProperty("page", previousPage);
    });
    QVERIFY(backend->selectCharacter("doubao"));
    QCOMPARE(backend->characterStyle()["name"].toString(), QString("豆包"));
    QVERIFY(!backend->removeCharacter("doubao"));
    const auto assets = backend->characterStyle()["assets"].toMap();
    QCOMPARE(assets.size(), 11);
    QSet<QString> sources;
    for (auto it = assets.cbegin(); it != assets.cend(); ++it) {
      const auto source = QUrl(it.value().toString());
      const auto path = source.isLocalFile() ? source.toLocalFile() : ":" + source.path();
      QImage image(path);
      QVERIFY2(!image.isNull(), qPrintable(path));
      if (it.value() == assets["picker"]) {
        QCOMPARE(image, QImage(root + "/art/characters/doubao-v1/source/picker-pretty.png"));
      } else {
        QCOMPARE(image.size(), QSize(768, 768));
        QVERIFY(image.hasAlphaChannel());
        QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
      }
      sources.insert(path);
    }
    QCOMPARE(sources.size(), 7);
    for (const auto &slot : QStringList{"avatarSmile", "avatarWink", "avatarHappy"})
      QCOMPARE(assets[slot], assets["picker"]);
    QVERIFY(assets["reply"] != assets["picker"]);
    for (const auto &value : backend->characterPacks())
      if (value.toMap()["id"] == "doubao")
        QVERIFY(value.toMap()["portrait"].toString().endsWith("/picker.png"));
    QVERIFY(backend->selectSubagentCharacter(1, "doubao"));
    const auto role = backend->characterStyle()["subagents"].toList()[1].toMap();
    QCOMPARE(role["name"].toString(), QString("豆包"));
    for (const auto &key : QStringList{"avatar", "catch"}) {
      const auto expected = QUrl(assets[key == "avatar" ? "avatarSmile" : "catch"].toString());
      const auto path = expected.isLocalFile() ? expected.toLocalFile() : ":" + expected.path();
      const auto actual = QImage(QUrl(role[key].toString()).toLocalFile());
      QVERIFY(!actual.isNull());
      QCOMPARE(actual, QImage(path));
    }
    bool offeredToGlobalAgents = false;
    for (const auto &value : backend->subagentCharacterPacks())
      if (value.toMap()["id"] == "doubao") {
        offeredToGlobalAgents = true;
        QVERIFY(value.toMap()["builtin"].toBool());
        QVERIFY(value.toMap()["portrait"].toString().endsWith("/picker.png"));
      }
    QVERIFY(offeredToGlobalAgents);
    backend->setTheme("light");
    window->setProperty("page", "settings");
    window->setProperty("settingsTab", 5);
    QTest::qWait(200);
    click("chooseCharacterButton");
    QTRY_VERIFY(find("character_doubao") && find("character_doubao")->isVisible());
    QVERIFY(!find("deleteCharacter_doubao")->isVisible());
    QDir().mkpath(root + "/build/screenshots");
    QTest::qWait(250);
    QVERIFY(window->grabWindow().save(root + "/build/screenshots/doubao-picker.png"));
    click("closeCharacterDialog");
    window->setProperty("page", "chat");
    backend->newSession();
    QTRY_VERIFY(find("welcomeMascot")->property("loaded").toBool());
    QTest::qWait(300);
    QVERIFY(window->grabWindow().save(root + "/build/screenshots/doubao-welcome.png"));
    QVERIFY(QMetaObject::invokeMethod(find("welcomeMascot"), "tap"));
    const auto line = find("welcomeMascot")->property("reactionText").toString();
    QVERIFY(backend->characterConfiguration()["config"].toMap()["scenes"].toMap()["welcome"].toMap()["tap"].toList().contains(line));
    QVERIFY(QMetaObject::invokeMethod(find("welcomeMascot"), "reset"));
  }
  void doubaoPetAndRareScenes() {
    QDir().mkpath(root + "/build/screenshots");
    const auto previous = backend->characterStyle()["id"].toString();
    auto pet = qobject_cast<DesktopPet*>(backend->desktopExtensions->pet());
    QVERIFY(pet);
    const auto petConfig = pet->config();
    const auto restore = qScopeGuard([&] {
      backend->selectCharacter(previous);
      pet->ui("configure", petConfig);
    });
    QVERIFY(backend->selectCharacter("doubao"));
    const auto scene = backend->characterStyle()["easterEgg"].toMap();
    QCOMPARE(scene["title"].toString(), QString("稳稳接住你"));
    const auto sceneUrl = QUrl(scene["source"].toString());
    const auto scenePath = sceneUrl.isLocalFile() ? sceneUrl.toLocalFile() : ":" + sceneUrl.path();
    QImage sceneImage(scenePath);
    QVERIFY(!sceneImage.isNull());
    QVERIFY(sceneImage.hasAlphaChannel());
    bool found = false;
    for (const auto &value : pet->appearances()) if (value.toMap()["id"] == "doubao") {
      found = true;
      QVERIFY(value.toMap()["builtin"].toBool());
    }
    QVERIFY(found);
    QVERIFY(!pet->ui("removeAppearance", {{"id", "doubao"}})["ok"].toBool());
    const auto folder = root + "/umeko-plugins/desktop-pet/assets/doubao/";
    for (const auto &state : QStringList{"idle", "thinking", "working", "waiting", "done", "error", "idle-rare"}) {
      QImageReader reader(folder + state + ".webp");
      QVERIFY2(reader.imageCount() > 1, qPrintable(state));
      QCOMPARE(reader.size(), QSize(360, 360));
      for (int index = 0; index < reader.imageCount(); ++index) {
        const auto image = reader.read();
        QVERIFY(!image.isNull());
        QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
      }
    }
    QVERIFY(pet->ui("configure", {{"appearance", "doubao"}, {"visible", true}, {"paused", false}})["ok"].toBool());
    QTRY_VERIFY(window->findChild<QQuickWindow*>("desktopPetWindow"));
    auto petWindow = window->findChild<QQuickWindow*>("desktopPetWindow");
    QTRY_VERIFY(petWindow->findChild<QObject*>("desktopPetAnimation"));
    auto animation = petWindow->findChild<QObject*>("desktopPetAnimation");
    QTRY_VERIFY(animation->property("ready").toBool());
    QVERIFY(animation->property("source").toUrl().toString().contains("/doubao/"));
    const auto frame = animation->property("currentFrame").toInt();
    QTRY_VERIFY(animation->property("currentFrame").toInt() != frame);
    QVERIFY(petWindow->grabWindow().save(root + "/build/screenshots/doubao-pet.png"));
    pet->action = pet->target = "idle";
    pet->interaction.clear();
    pet->drawIdleScene(0);
    QVERIFY(pet->state()["rareIdle"].toBool());
    QTRY_VERIFY(animation->property("source").toUrl().toString().endsWith("/idle-rare.webp"));
    QTRY_VERIFY(animation->property("ready").toBool());
    QVERIFY(petWindow->grabWindow().save(root + "/build/screenshots/doubao-rare-pet.png"));
    pet->ui("hide", {});
    QTRY_VERIFY(!find("desktopPetWindowLoader")->property("active").toBool());
  }
  void whaleGirlCharacterAcrossSurfaces() {
    const auto previous = backend->characterStyle()["id"].toString();
    const auto options = QJsonDocument::fromVariant(
        backend->characterConfiguration()["overrides"]).toJson();
    const auto previousPage = window->property("page");
    const auto restore = qScopeGuard([&] {
      backend->selectCharacter(previous);
      backend->configureCharacter(QString::fromUtf8(options));
      window->setProperty("page", previousPage);
    });
    QVERIFY(backend->selectCharacter("whale-girl"));
    QCOMPARE(backend->characterStyle()["name"].toString(), QString("鲸鱼娘"));
    const auto assets = backend->characterStyle()["assets"].toMap();
    QCOMPARE(assets.size(), 10);
    for (auto value : assets) {
      const auto source = QUrl(value.toString());
      const auto path = source.isLocalFile() ? source.toLocalFile()
                                             : ":" + source.path();
      QImage image(path);
      QVERIFY2(!image.isNull(), qPrintable(path));
      QCOMPARE(image.size(), QSize(768, 768));
      QVERIFY(image.hasAlphaChannel());
      QCOMPARE(image.pixelColor(0, 0).alpha(), 0);
    }
    QVERIFY(!backend->removeCharacter("whale-girl"));
    window->setProperty("page", "chat"); backend->newSession();
    QTRY_VERIFY(find("welcomeMascot")->property("loaded").toBool());
    QDir().mkpath(root + "/build/screenshots");
    QTest::qWait(200);
    QVERIFY(window->grabWindow().save(root + "/build/screenshots/whale-girl-welcome.png"));
    QVERIFY(QMetaObject::invokeMethod(find("welcomeMascot"), "tap"));
    QVERIFY(find("welcomeMascot")->property("reactionText").toString().startsWith("人，"));
    QVERIFY(QMetaObject::invokeMethod(find("welcomeMascot"), "reset"));
    window->setProperty("page", "settings");
    window->setProperty("settingsTab", 5); QTest::qWait(200);
    click("chooseCharacterButton");
    QTRY_VERIFY(find("character_whale-girl") && find("character_whale-girl")->isVisible());
    QVERIFY(!find("deleteCharacter_whale-girl")->isVisible());
    QVERIFY(window->grabWindow().save(root + "/build/screenshots/whale-girl-picker.png"));
    click("closeCharacterDialog");
    QVERIFY(backend->selectSubagentCharacter(3, "whale-girl"));
    QVERIFY(backend->selectCharacter("tiara"));
    QCOMPARE(backend->characterNames()[3], QString("鲸鱼娘"));
  }
  void kazuhikoAndRareScenes() {
    const auto previous = backend->characterStyle()["id"].toString();
    const auto options = QJsonDocument::fromVariant(backend->characterConfiguration()["overrides"]).toJson();
    auto pet = qobject_cast<DesktopPet*>(backend->desktopExtensions->pet());
    QVERIFY(pet);
    const auto petConfig = pet->config();
    const auto restore = qScopeGuard([&] {
      backend->selectCharacter(previous);
      backend->configureCharacter(QString::fromUtf8(options));
      pet->ui("configure", petConfig);
    });
    bool found = false;
    for (auto value : backend->characterPacks()) QVERIFY(value.toMap()["id"] != "kazuhiko");
    for (auto value : backend->subagentCharacterPacks())
      if (value.toMap()["id"] == "kazuhiko") found = true;
    QVERIFY(found); QVERIFY(!backend->selectCharacter("kazuhiko"));
    QVERIFY(!backend->removeCharacter("kazuhiko"));
    QVERIFY(backend->selectSubagentCharacter(0, "kazuhiko"));
    QCOMPARE(backend->characterNames()[0], QString("温水和彦"));
    for (auto key : {"avatar", "catch"})
      QVERIFY(!QImage(QUrl(backend->characterStyle()["subagents"].toList()[0].toMap()[key].toString()).toLocalFile()).isNull());
    QVERIFY(backend->selectCharacter("tiara"));
    window->setProperty("page", "chat"); backend->newSession();
    QVERIFY(!backend->characterStyle()["easterEgg"].toMap().isEmpty());
    const auto token = backend->conversationToken();
    QVERIFY(!token.isEmpty());
    auto welcome = find("welcomeMascot"); QVERIFY(welcome);
    const auto probe = [&](QString code) {
      QQmlExpression expression(qmlContext(welcome), welcome, code);
      auto result = expression.evaluate();
      if (expression.hasError()) qWarning() << expression.error();
      if (result.metaType() == QMetaType::fromType<QJSValue>())
        result = result.value<QJSValue>().toVariant();
      return result;
    };
    QVERIFY(probe("Character.rareCover('boundary-excluded', function(){return 0.005})").toMap().isEmpty());
    QVERIFY(!probe("Character.rareCover('boundary-included', function(){return 0.004999})").toMap().isEmpty());
    QVERIFY(!probe("Character.rareCover('boundary-included', function(){throw Error('Cache rerolled')})").toMap().isEmpty());
    backend->newSession();
    QVERIFY(backend->conversationToken() != token);
    probe("Character.rareCoverCache = Object.assign({}, Character.rareCoverCache, "
          "{[JSON.stringify([Character.id, backend.conversationToken])]: Character.style.easterEgg})");
    QTRY_VERIFY(!welcome->property("rareGreeting").toString().isEmpty());
    QTRY_VERIFY(welcome->property("loaded").toBool());
    QTest::qWait(200);
    QVERIFY(window->grabWindow().save(root + "/build/screenshots/tiara-rare-cover.png"));
    probe("Character.rareCoverCache = ({})");
    QVERIFY(pet->ui("configure", {{"appearance","tiara"},{"visible",true},{"paused",false}})["ok"].toBool());
    pet->action = pet->target = "idle"; pet->interaction.clear();
    pet->drawIdleScene(.004999);
    QVERIFY(pet->state()["rareIdle"].toBool()); QVERIFY(pet->state()["animated"].toBool());
    QVERIFY(pet->state()["source"].toString().endsWith("/idle-rare.webp"));
    QTRY_VERIFY(window->findChild<QQuickWindow*>("desktopPetWindow"));
    QPointer<QQuickWindow> petWindow = window->findChild<QQuickWindow*>("desktopPetWindow");
    QTRY_VERIFY(petWindow->findChild<QObject*>("desktopPetAnimation"));
    auto animation = petWindow->findChild<QObject*>("desktopPetAnimation");
    QTRY_VERIFY(animation->property("ready").toBool());
    QTRY_VERIFY(animation->property("source").toUrl().toString().endsWith("/idle-rare.webp"));
    auto frame = animation->property("currentFrame").toInt();
    QTRY_VERIFY(animation->property("currentFrame").toInt() != frame);
    QVERIFY(petWindow->grabWindow().save(root + "/build/screenshots/tiara-rare-pet.png"));
    pet->ui("configure", {{"paused",true}});
    QTRY_VERIFY(!animation->property("playing").toBool());
    pet->drawIdleScene(.5); QVERIFY(pet->state()["rareIdle"].toBool());
    pet->ui("configure", {{"paused",false}});
    pet->drawIdleScene(.005); QVERIFY(!pet->state()["rareIdle"].toBool());
    pet->drawIdleScene(0); QVERIFY(pet->state()["rareIdle"].toBool());
    pet->ui("hide", {}); QVERIFY(!pet->state()["rareIdle"].toBool());
    pet->drawIdleScene(0); QVERIFY(!pet->state()["rareIdle"].toBool());
    // Hiding the pet unloads the whole window. Inspect its owning Loader;
    // the old petWindow and animation can already have been destroyed here.
    auto loader = find("desktopPetWindowLoader");
    QVERIFY(loader); QTRY_VERIFY(!loader->property("active").toBool());
    QTRY_VERIFY(petWindow.isNull());
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
  void inputMethodCaretUpdates() {
    InputMethodUpdates updates(qApp);
    QQuickView composer;
    composer.engine()->rootContext()->setContextProperty("backend", backend.get());
    composer.setResizeMode(QQuickView::SizeRootObjectToView);
    QQmlComponent component(composer.engine());
    const auto url = QUrl::fromLocalFile(root + "/tests/fixtures/input-method.qml");
    component.setData("import QtQuick\n"
                      "import \"../../app/qml\" as Umeko\n"
                      "Item {\n"
                      "  Umeko.Composer {\n"
                      "    objectName: \"composer\"; x: 20; width: parent.width - 40\n"
                      "    anchors.bottom: parent.bottom; anchors.bottomMargin: 20\n"
                      "  }\n"
                      "}", url);
    QVERIFY2(component.isReady(), qPrintable(component.errors().isEmpty() ? QString() : component.errors().first().toString()));
    composer.setContent(url, &component, component.create());
    composer.resize(840, 400);
    composer.show();
    composer.requestActivate();
    auto *rootItem = composer.rootObject();
    QVERIFY(rootItem);
    auto locate = [](QQuickItem *root, const QString &name) {
      QList<QQuickItem *> items{root};
      while (!items.isEmpty()) {
        auto *item = items.takeLast();
        if (item->objectName() == name)
          return item;
        items.append(item->childItems());
      }
      return static_cast<QQuickItem *>(nullptr);
    };
    auto *prompt = locate(rootItem, "promptInput");
    auto *composerItem = locate(rootItem, "composer");
    QVERIFY(prompt && composerItem);
    QSignalSpy cursor(qApp->inputMethod(), &QInputMethod::cursorRectangleChanged);
    QSignalSpy anchor(qApp->inputMethod(), &QInputMethod::anchorRectangleChanged);
    auto flush = [&] {
      QCoreApplication::sendPostedEvents();
      QCoreApplication::processEvents();
      QCoreApplication::sendPostedEvents();
      QCoreApplication::processEvents();
    };
    auto check = [&](QQuickItem *item) {
      const auto actual = qApp->inputMethod()->cursorRectangle();
      const auto mapped = item->mapRectToScene(item->inputMethodQuery(Qt::ImCursorRectangle).toRectF());
      const auto caret = item->mapRectToScene(item->property("cursorRectangle").toRectF());
      for (const auto &expected : {mapped, caret}) {
        QString message;
        QDebug(&message) << actual << "!=" << expected;
        QVERIFY2(qAbs(actual.x() - expected.x()) <= 1 &&
                 qAbs(actual.y() - expected.y()) <= 1 &&
                 qAbs(actual.width() - expected.width()) <= 1 &&
                 qAbs(actual.height() - expected.height()) <= 1,
                 qPrintable(message));
      }
      QVERIFY(actual.width() > 0 && actual.height() > 0);
    };
    QMetaObject::invokeMethod(composerItem, "focusInput");
    flush();
    QVERIFY(prompt->hasActiveFocus());
    prompt->setFocus(false);
    flush();
    QVERIFY(!prompt->hasActiveFocus());
    const auto before = cursor.count();
    const auto anchorBefore = anchor.count();
    QMetaObject::invokeMethod(composerItem, "focusInput");
    flush();
    check(prompt);
    QVERIFY(prompt->hasActiveFocus());
    QVERIFY(prompt->property("text").toString().isEmpty());
    QVERIFY(cursor.count() > before && anchor.count() > anchorBefore);
    const auto beforeResize = cursor.count();
    composer.resize(740, 520);
    flush();
    check(prompt);
    QVERIFY(cursor.count() > beforeResize);
    const auto beforeText = cursor.count();
    prompt->setProperty("text", "中");
    prompt->setProperty("cursorPosition", 1);
    flush();
    check(prompt);
    QVERIFY(cursor.count() > beforeText);
    prompt->setProperty("text", QString("line 0\nline 1\nline 2\nline 3\nline 4\nline 5\nline 6\nline 7\nline 8\nline 9\nline 10\nline 11\nline 12\nline 13\nline 14\nline 15\nline 16\nline 17\nline 18\nline 19\nline 20\nline 21\nline 22\nline 23\nline 24\nline 25\nline 26\nline 27\nline 28\nline 29"));
    prompt->setProperty("cursorPosition", 20);
    flush();
    auto *scroll = prompt->parentItem();
    while (scroll && !scroll->property("contentY").isValid())
      scroll = scroll->parentItem();
    QVERIFY(scroll);
    const auto contentY = scroll->property("contentY").toDouble();
    const auto beforeScroll = cursor.count();
    scroll->setProperty("contentY", contentY + 12);
    flush();
    QVERIFY(scroll->property("contentY").toDouble() > contentY);
    check(prompt);
    QVERIFY(cursor.count() > beforeScroll);
    prompt->setProperty("text", "");
    flush();

    EditorSdk sdk;
    QQuickView widget;
    widget.engine()->rootContext()->setContextProperty("backend", backend.get());
    widget.setResizeMode(QQuickView::SizeRootObjectToView);
    widget.setInitialProperties({{"sdk", QVariant::fromValue<QObject *>(&sdk)}});
    widget.setSource(QUrl::fromLocalFile(root + "/umeko-plugins/skill-manager/qml/Main.qml"));
    QVERIFY2(widget.status() == QQuickView::Ready,
             qPrintable(widget.errors().isEmpty() ? QString() : widget.errors().first().toString()));
    widget.resize(500, 800);
    widget.show();
    widget.requestActivate();
    flush();
    auto *field = locate(widget.rootObject(), "skillManagerSearch");
    QVERIFY(field);
    field->forceActiveFocus();
    flush();
    QVERIFY(field->hasActiveFocus());
    field->setFocus(false);
    flush();
    QVERIFY(!field->hasActiveFocus());
    const auto beforeWidget = cursor.count();
    const auto widgetAnchorBefore = anchor.count();
    field->forceActiveFocus();
    flush();
    check(field);
    QVERIFY(cursor.count() > beforeWidget && anchor.count() > widgetAnchorBefore);
    QMetaObject::invokeMethod(composerItem, "focusInput");
    composer.requestActivate();
    flush();
    check(prompt);
    QVERIFY(qApp->focusWindow() == &composer);
    widget.close();
    composer.close();
  }
  void interactiveProviderLogin() {
    auto manager = qobject_cast<Providers *>(backend->providerPlugins());
    const auto archive = directory.filePath("interactive-login.zip");
    const QByteArray code = R"PY(from pathlib import Path
from umeko_provider_sdk import Provider,ProviderError
class Interactive(Provider):
 def __init__(self,context):
  super().__init__(context); self.model_calls=0
 def authenticate(self, credentials):
  self.context.secrets.set('session',{'token':'synthetic-pending-token'})
  if credentials['username']=='provider-close':
   self.close()
   raise ProviderError('Sign-in interrupted: '+credentials['username'])
  self.context.report_status('Waiting for browser confirmation')
  while not self.context.request_cancelled.wait(.05): pass
  raise ProviderError('Sign-in interrupted: '+credentials['username'])
 def list_models(self, refresh=False):
  self.model_calls+=1
  Path(self.context.state_dir/'models-called').write_text('called')
  return []
def create_provider(context): return Interactive(context)
)PY";
    QVERIFY(package(archive, false, false, "example-interactive", "0.1.0", {}, code));
    QVERIFY(manager->importZip(QUrl::fromLocalFile(archive).toString()));
    const auto id = manager->rows().last().toMap()["id"].toString();
    const auto other = manager->addAccount("example-interactive", "0.1.0") ? manager->rows().last().toMap()["id"].toString() : QString();
    QVERIFY(!other.isEmpty() && other != id);
    auto row = [manager](const QString &identifier) {
      for (const auto &value : manager->rows())
        if (value.toMap()["id"] == identifier)
          return value.toMap();
      return QVariantMap();
    };
    QSignalSpy notices(backend.get(), &Backend::toast);
    const auto previousPage = window->property("page");
    const auto previousTab = window->property("settingsTab");
    const auto language = backend->language();
    auto restore = qScopeGuard([&] {
      manager->cancelLogin(id);
      manager->cancelLogin(other);
      backend->setLanguage(language);
      window->setProperty("page", previousPage);
      window->setProperty("settingsTab", previousTab);
    });
    backend->setLanguage("en");
    window->setProperty("page", "settings");
    window->setProperty("settingsTab", 0);
    manager->login(id, "{\"username\":\"fixture-secret\"}");
    manager->login(other, "{\"username\":\"other-secret\"}");
    QTRY_COMPARE_WITH_TIMEOUT(row(id)["status"].toString(), QString("Waiting for browser confirmation"), 15000);
    QVERIFY(row(id)["loggingIn"].toBool() && row(other)["loggingIn"].toBool());
    bool bounded = false;
    for (auto process : manager->findChildren<QProcess *>())
      for (auto timer : process->findChildren<QTimer *>())
        bounded |= timer->interval() == 600000 && timer->isActive();
    QVERIFY(bounded);
    const auto before = notices.count();
    QTRY_VERIFY_WITH_TIMEOUT(find("loginPlugin_" + id) != nullptr, 5000);
    auto button = find("loginPlugin_" + id);
    QTRY_COMPARE_WITH_TIMEOUT(button->property("text").toString(), QString("Cancel sign-in"), 5000);
    QVERIFY(button->isEnabled());
    auto scroll = find("settingsScroll");
    QVERIFY(scroll);
    auto flick = qobject_cast<QQuickItem *>(scroll->property("contentItem").value<QObject *>());
    QVERIFY(flick);
    scroll->forceActiveFocus();
    QMetaObject::invokeMethod(flick, "cancelFlick");
    bool revealed = false;
    for (int attempt = 0; attempt < 12 && !revealed; ++attempt) {
      QTest::qWait(100);
      button = find("loginPlugin_" + id);
      QVERIFY(button);
      flick->ensurePolished();
      button->ensurePolished();
      const auto center = button->boundingRect().center();
      const auto local = button->mapToItem(flick, center);
      const auto scene = button->mapToScene(center);
      if (flick->contains(local) && QRectF(0, 0, window->width(), window->height()).contains(scene)) {
        revealed = true;
        break;
      }
      const auto y = flick->property("contentY").toReal() + local.y() - flick->height() / 2;
      flick->setProperty("contentY", qBound(0.0, y, qMax(0.0, flick->property("contentHeight").toReal() - flick->height())));
    }
    QVERIFY(revealed);
    click("loginPlugin_" + id);
    QTRY_VERIFY_WITH_TIMEOUT(!row(id)["busy"].toBool(), 5000);
    QCOMPARE(row(id)["status"].toString(), QString("Sign-in interrupted: ***"));
    QVERIFY(!row(id)["loggingIn"].toBool());
    QVERIFY(row(other)["loggingIn"].toBool());
    QVERIFY(backend->store.secret("account:" + id).isEmpty());
    QCOMPARE(notices.count(), before);
    QVERIFY(!QFileInfo::exists(directory.filePath("provider-plugins/state/" + U::sha(id.toUtf8()) + "/models-called")));
    manager->cancelLogin(other);
    QTRY_VERIFY_WITH_TIMEOUT(!row(other)["busy"].toBool(), 5000);
    QCOMPARE(notices.count(), before);
    const auto providerClosed = manager->addAccount("example-interactive", "0.1.0") ? manager->rows().last().toMap()["id"].toString() : QString();
    QVERIFY(!providerClosed.isEmpty());
    manager->login(providerClosed, "{\"username\":\"provider-close\"}");
    QTRY_VERIFY_WITH_TIMEOUT(!row(providerClosed)["busy"].toBool(), 5000);
    QCOMPARE(row(providerClosed)["status"].toString(), QString("Sign-in interrupted: ***"));
    QCOMPARE(notices.count(), before);
    QVERIFY(backend->store.secret("account:" + providerClosed).isEmpty());
    QVERIFY(!QFileInfo::exists(directory.filePath("provider-plugins/state/" + U::sha(providerClosed.toUtf8()) + "/models-called")));
    QTRY_VERIFY_WITH_TIMEOUT(([&]{for(auto process:manager->findChildren<QProcess*>())if(process->state()!=QProcess::NotRunning)return false;return true;})(),20000); // Background capability probes must finish before package removal.
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
    QTRY_VERIFY_WITH_TIMEOUT(([&]{for(auto process:manager->findChildren<QProcess*>())if(process->state()!=QProcess::NotRunning)return false;return true;})(),20000); // Background capability probes must finish before package removal.
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
    QTRY_VERIFY_WITH_TIMEOUT(([&]{for(auto process:manager->findChildren<QProcess*>())if(process->state()!=QProcess::NotRunning)return false;return true;})(),20000); // Background capability probes must finish before package removal.
    QVERIFY(manager->uninstall("example-reinstall", "0.1.0"));
    const auto recovered = install(archive);
    QVERIFY(!recovered.isEmpty());
    QFile code(folder + "0.1.0/scripts/provider.py");
    QVERIFY(code.open(QIODevice::ReadOnly));
    QVERIFY(!code.readAll().contains("Reinstalled:"));
    code.close();
    manager->remove(recovered);
    manager->remove(otherVersion);
    QTRY_VERIFY_WITH_TIMEOUT(([&]{for(auto process:manager->findChildren<QProcess*>())if(process->state()!=QProcess::NotRunning)return false;return true;})(),20000); // Background capability probes must finish before package removal.
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
    QTRY_VERIFY_WITH_TIMEOUT(([&]{for(auto process:manager->findChildren<QProcess*>())if(process->state()!=QProcess::NotRunning)return false;return true;})(),20000); // Background capability probes must finish before package removal.
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
  void widgetPageIncubationFinishesWithoutRenderFrames() {
    auto manager=backend->desktopExtensions;manager->closePage();
    auto automatic=engine->incubationController();QQmlIncubationController paused;
    const auto restore=qScopeGuard([&]{manager->closePage();engine->setIncubationController(automatic);window->show();window->requestActivate();});
    window->hide();engine->setIncubationController(&paused);
    QElapsedTimer elapsed;elapsed.start();
    QVERIFY(manager->openPage("scheduled-tasks"));
    QTRY_VERIFY_WITH_TIMEOUT(find("desktopPluginPageLoader")&&find("desktopPluginPageLoader")->property("status").toInt()==1,5000);
    QVERIFY(find("scheduledTasksWidget"));
    qInfo()<<"Widget creation without render frames (ms):"<<elapsed.elapsed();
  }
  void scheduledTasksLocalTimeGlobalScopeAndNativeDelivery() {
    configureAPI();backend->newSession();window->setProperty("page","chat");window->resize(1440,950);
    auto manager=backend->desktopExtensions;auto bus=manager->globalAgents();
    const auto originalSettings=manager->settings;
    QMetaObject::Connection trace;
    if(qEnvironmentVariableIsSet("UMEKO_SCHEDULED_TRACE"))trace=connect(manager,&DesktopPlugins::pageChanged,this,[&]{qInfo()<<"TIMER_PAGE"<<manager->page()<<"session"<<manager->m_session["token"]<<"view"<<backend->view->key;});
    const auto stopTrace=qScopeGuard([&]{disconnect(trace);});
    auto source=backend->view;QTemporaryDir workspace;QVERIFY(workspace.isValid());backend->setWorkspace(workspace.path());
    QStringList tasks,agents;globalBridge=true;
    const auto invoke=[&](QString method,QVariantMap args=QVariantMap{},std::shared_ptr<Session> caller=std::shared_ptr<Session>{}) {
      QSignalSpy replies(manager,&DesktopPlugins::response);
      const auto id=manager->callFor("scheduled-tasks",method,QString::fromUtf8(QJsonDocument::fromVariant(args).toJson(QJsonDocument::Compact)),false,20000,
          QVariantMap{{"token",(caller?caller:source)->key},{"id",(caller?caller:source)->id},{"workspace",(caller?caller:source)->workspace}});
      QElapsedTimer elapsed;elapsed.start();
      while(!id.isEmpty()&&elapsed.elapsed()<20000){for(const auto &row:replies)if(row[0]==id)return row[1].toMap();QTest::qWait(20);}
      return QVariantMap{{"ok",false},{"error","Timer call timed out: "+method}};
    };
    const auto restore=qScopeGuard([&]{
      for(const auto &id:tasks)invoke("delete",{{"task_id",id}});
      for(const auto &id:agents)if(bus->agents.contains(id))bus->ui("delete",{{"id",id}});
      globalBridge=false;manager->closePage();manager->settings=originalSettings;manager->save();backend->newSession();
    });
    QVERIFY(manager->enabled("scheduled-tasks"));QTRY_VERIFY_WITH_TIMEOUT(manager->workers.value("scheduled-tasks")&&manager->workers["scheduled-tasks"]->ready,15000);
    const QString draft="保留未发送草稿";source->draft=draft;
    const auto savedAttachments=source->attachments;
    const QVariantMap interval{{"frequency","interval"},{"interval_value",0.25},{"interval_unit","minutes"}};
    const auto privateReply=invoke("save",{{"name","本会话计时器"},{"message","Native timer main delivery"},{"target","main"},{"enabled",false},{"schedule",interval}});
    QVERIFY2(privateReply["ok"].toBool(),qPrintable(privateReply["error"].toString()));
    const auto local=privateReply["result"].toMap()["task_id"].toString();tasks<<local;
    QVERIFY(invoke("run",{{"task_id",local}})["ok"].toBool());
    const auto runs=[&](QString id){return invoke("history",{{"task_id",id}})["result"].toMap()["runs"].toList();};
    QTRY_VERIFY_WITH_TIMEOUT(runs(local).size()==1&&runs(local).first().toMap()["state"]=="delivered",15000);
    QTRY_VERIFY_WITH_TIMEOUT(!source->busy(),15000);QVERIFY(!source->id.isEmpty());
    const auto created=bus->ui("create",{{"name","全局日报助手"},{"model",source->model}});
    QVERIFY2(created["ok"].toBool(),qPrintable(created["error"].toString()));const auto agent=created["result"].toMap()["id"].toString();agents<<agent;
    QVERIFY(bus->ui("setBridge",{{"id","session:"+source->id},{"agentId",agent},{"enabled",true}})["ok"].toBool());
    const QVariantMap calendar{{"frequency","once"},{"first_date","2030-01-01"},{"local_time","04:00:00"},{"zone","America/Los_Angeles"}};
    const auto globalReply=invoke("save",{{"name","全局计时器 · 本地时间"},{"message","Native timer global delivery"},{"target","agent:"+agent},{"enabled",true},{"schedule",calendar}});
    QVERIFY2(globalReply["ok"].toBool(),qPrintable(globalReply["error"].toString()));const auto shared=globalReply["result"].toMap()["task_id"].toString();tasks<<shared;
    QVERIFY(manager->openPage("scheduled-tasks"));QTRY_VERIFY(find("scheduledTasksWidget"));
    QTRY_VERIFY_WITH_TIMEOUT(find("scheduledTaskGlobal_"+shared)&&find("scheduledTaskGlobal_"+shared)->isVisible(),15000);
    const auto next=find("scheduledTaskNext_"+shared)->property("text").toString();
    const auto expected=QDateTime::fromSecsSinceEpoch(1893499200).toLocalTime().toString("yyyy-MM-dd HH:mm:ss");
    QVERIFY2(next.contains(expected),qPrintable(next+" != "+expected));
    if(QTimeZone::systemTimeZoneId()=="Asia/Shanghai")QVERIFY(next.contains("2030-01-01 20:00:00"));
    QTRY_VERIFY_WITH_TIMEOUT(!find("webPane")->property("moving").toBool(),5000);
    QTRY_VERIFY(find("scheduledNewOrBack")->isEnabled());
    click("scheduledNewOrBack");QTRY_COMPARE(find("scheduledTasksWidget")->property("page").toString(),QString("edit"));QTRY_VERIFY(find("scheduledTaskZone"));
    QCOMPARE(find("scheduledTaskZone")->property("text").toString(),QString::fromUtf8(QTimeZone::systemTimeZoneId()));
    click("scheduledNewOrBack");QTRY_COMPARE(find("scheduledTasksWidget")->property("page").toString(),QString("list"));
    QVERIFY(invoke("run",{{"task_id",shared}})["ok"].toBool());
    QTRY_VERIFY_WITH_TIMEOUT(runs(shared).size()==1&&runs(shared).first().toMap()["state"]=="delivered",20000);
    const auto history=runs(shared);QVERIFY(!history.first().toMap()["session_id"].toString().isEmpty());
    QVERIFY(invoke("enable",{{"task_id",local}})["ok"].toBool());
    backend->newSession();auto other=backend->view;
    auto visible=invoke("list",{},other)["result"].toMap()["tasks"].toList();QCOMPARE(visible.size(),1);QCOMPARE(visible.first().toMap()["id"].toString(),shared);
    QVERIFY(!invoke("delete",{{"task_id",local}},other)["ok"].toBool());
    // The visible global timer occupies a slot even in an unconnected new chat.
    QStringList extras;
    for(int i=0;i<4;++i){const auto result=invoke("save",{{"name",QString("Private %1").arg(i)},{"message","Quota fixture"},{"target","main"},{"enabled",false},{"schedule",interval}},other);
      QVERIFY(result["ok"].toBool());extras<<result["result"].toMap()["task_id"].toString();}
    QVERIFY(!invoke("save",{{"name","Sixth"},{"message","Must fail"},{"target","main"},{"enabled",false},{"schedule",interval}},other)["ok"].toBool());
    for(const auto &id:extras)QVERIFY(invoke("delete",{{"task_id",id}},other)["ok"].toBool());
    QTRY_VERIFY_WITH_TIMEOUT(runs(local).size()>=3&&runs(local).first().toMap()["state"]=="delivered",40000);
    const auto periodic=runs(local);QCOMPARE(periodic[0].toMap()["state"].toString(),QString("delivered"));
    QCOMPARE(periodic[0].toMap()["scheduled_at"].toLongLong()-periodic[1].toMap()["scheduled_at"].toLongLong(),15LL);
    QVERIFY(backend->view==other);QCOMPARE(source->draft,draft);QCOMPARE(source->attachments,savedAttachments);
    QVERIFY(invoke("disable",{{"task_id",local}})["ok"].toBool());
    manager->closePage();QVERIFY(bus->ui("delete",{{"id",agent}})["ok"].toBool());
    // Observe persistence without calling list (which also refreshes metadata).
    QProcess inspector;const auto db=manager->root+"/state/scheduled-tasks/application/schedules.sqlite3";
    QTest::qWait(500);
    inspector.start(backend->providers->resolvePython(),{"-B","-c","import sqlite3,sys;print(sqlite3.connect(sys.argv[1]).execute('select enabled,next_at from schedules where id=?',(sys.argv[2],)).fetchone())",db,shared});
    QVERIFY(inspector.waitForFinished(5000));QCOMPARE(inspector.readAllStandardOutput().trimmed(),QByteArray("(0, None)"));
    QVERIFY(manager->openPage("scheduled-tasks"));
    QTRY_VERIFY2_WITH_TIMEOUT(find("scheduledTaskTargetName_"+shared),qPrintable(QString::fromUtf8(QJsonDocument::fromVariant(manager->page()).toJson())),15000);
    QTRY_COMPARE(find("scheduledTaskTargetName_"+shared)->property("text").toString(),backend->language()=="en"?QString("None"):QString("无"));
    QVERIFY(!find("scheduledTaskToggle_"+shared)->isEnabled());QCOMPARE(runs(shared),history);
    QVERIFY(!invoke("enable",{{"task_id",shared}},other)["ok"].toBool());
    backend->openSession(source->id);QVERIFY(manager->openPage("scheduled-tasks"));QTRY_VERIFY_WITH_TIMEOUT(find("scheduledTaskEdit_"+local),15000);
    const auto screenshots=root+"/build/screenshots";QDir().mkpath(screenshots);window->grabWindow().save(screenshots+"/scheduled-tasks-native.png");
    for(const auto &warning:warnings)QVERIFY2(!warning.contains("scheduled-tasks/qml"),qPrintable(warning));
    if(!qEnvironmentVariable("UMEKO_SCHEDULED_REVIEW_DIR").isEmpty()) {
      const auto output=qEnvironmentVariable("UMEKO_SCHEDULED_REVIEW_DIR");QDir().mkpath(output);
      backend->setLanguage("zh");backend->setTheme("white");window->setTitle("Umeko C++ · #147 全局计时器预览");
      const auto added=bus->ui("create",{{"name","日报助手 · 演示"},{"model",source->model}})["result"].toMap()["id"].toString();agents<<added;
      QVERIFY(bus->ui("setBridge",{{"id","session:"+source->id},{"agentId",added},{"enabled",true}})["ok"].toBool());
      const auto result=invoke("save",{{"name","全局计时器 · 东八区 20:00"},{"message","【演示】整理项目日报。"},{"target","agent:"+added},{"enabled",true},{"schedule",calendar}});
      QVERIFY(result["ok"].toBool());tasks<<result["result"].toMap()["task_id"].toString();
      QTest::qWait(2200);window->grabWindow().save(output+"/preview.png");
      U::writeJson(output+"/ready.json",QVariantMap{{"pid",QCoreApplication::applicationPid()},{"profile",directory.path()},{"source",root}});
      window->raise();window->requestActivate();QEventLoop review;QTimer stop;stop.setInterval(250);
      connect(&stop,&QTimer::timeout,&review,[&]{if(QFileInfo::exists(output+"/stop"))review.quit();});stop.start();review.exec();
    }
  }
  void desktopPluginPagesAndSessionBoundary() {
    auto manager=qobject_cast<DesktopPlugins*>(backend->desktopPlugins());
    QVERIFY(manager);
    QSet<QString> builtins;
    for(const auto &row:manager->rows())builtins.insert(row.toMap()["id"].toString());
    QCOMPARE(builtins,QSet<QString>({"file-tree","git-viewer","terminal","desktop-pet","global-agents","pi-manager","change-review","skill-manager","widget-market","system-prompt","scheduled-tasks"}));
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
    // Unscoped view events reach only the open Widget. Explicit session targets
    // must still match both the chat and workspace; never weaken that boundary.
    QSignalSpy viewEvents(m,&DesktopPlugins::event);
    auto publish=[&](QVariantMap args,QString source="terminal") {
      m->hostEvent(source,{{"event","widget.event"},{"arguments",args}});
    };
    QVariantMap viewArgs{{"topic","fixture.frame"},{"data",QVariantMap{{"text","live"}}}};
    publish(viewArgs);QCOMPARE(viewEvents.size(),1);QVERIFY(!viewArgs.contains("session"));
    QCOMPARE(viewEvents.last()[0].toString(),QString("fixture.frame"));
    QCOMPARE(viewEvents.last()[1].toMap()["text"].toString(),QString("live"));
    auto scoped=viewArgs;scoped["session"]=m->m_session;
    publish(scoped);QCOMPARE(viewEvents.size(),2);
    auto stale=m->m_session;stale["token"]="stale-chat";scoped["session"]=stale;
    publish(scoped);QCOMPARE(viewEvents.size(),2);
    stale=m->m_session;stale["workspace"]="stale-workspace";scoped["session"]=stale;
    publish(scoped);QCOMPARE(viewEvents.size(),2);
    scoped["session"]=QVariantMap{};publish(scoped);QCOMPARE(viewEvents.size(),2);
    publish(viewArgs,"file-tree");QCOMPARE(viewEvents.size(),2);
    auto oversized=viewArgs;oversized["data"]=QVariantMap{{"text",QString(65537,'x')}};
    publish(oversized);QCOMPARE(viewEvents.size(),2);
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
  void terminalLiveOutputWithoutWindowActivation() {
    auto m=qobject_cast<DesktopPlugins*>(backend->desktopPlugins());QVERIFY(m);
    QTemporaryDir workspace;QVERIFY(workspace.isValid());
    backend->setWorkspace(QUrl::fromLocalFile(workspace.path()).toString());
    // Other Widget tests can leave an asynchronously unloading page and a
    // debounced session refresh. Start only after both have completed.
    m->closePage();
    QTRY_VERIFY(!find("terminalPluginPage"));
    QTRY_VERIFY(!m->contextRefresh.isActive());
    const auto previous=m->enabled("terminal");
    auto restore=qScopeGuard([&]{m->closePage();m->setEnabled("terminal",previous);});
    window->setProperty("page","chat");window->resize(1480,960);
    m->setEnabled("terminal",true);QVERIFY(m->openPage("terminal"));
    QTRY_VERIFY_WITH_TIMEOUT(find("terminalPluginPage"),10000);
    auto page=find("terminalPluginPage");
    QTRY_VERIFY_WITH_TIMEOUT(!page->property("activeId").toString().isEmpty(),10000);
    const auto id=page->property("activeId").toString();
    QTRY_VERIFY(find("terminalScreen_"+id));
    auto terminal=qobject_cast<TerminalView*>(find("terminalScreen_"+id));QVERIFY(terminal);
    auto contents=[&]{terminal->selectAll();return terminal->selectedText();};
    QTRY_VERIFY_WITH_TIMEOUT(!contents().trimmed().isEmpty(),10000);
    auto process=m->workers.value("terminal")->process;
    QTest::qWait(800); // Let the pane animation finish; never reactivate the window.
    QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,point(terminal));
    QTRY_VERIFY(terminal->hasActiveFocus());
    QSignalSpy inputs(terminal,&TerminalView::input);
    // Keep the marker out of echoed command text so only executed output passes.
    const QString command=
#ifdef Q_OS_WIN
        "Write-Output ('UMEKO_LIVE_'+'INPUT')";
#else
        "printf 'UMEKO_LIVE_%s\\n' INPUT";
#endif
    QKeyEvent input(QEvent::KeyPress,0,Qt::NoModifier,command);
    QCoreApplication::sendEvent(window,&input);QTest::keyClick(window,Qt::Key_Return);
    QTRY_VERIFY(!inputs.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(contents().contains("UMEKO_LIVE_INPUT"),10000);
    // Output continues without additional input, focus changes or snapshots.
    const QString delayed=
#ifdef Q_OS_WIN
        "Write-Output ('UMEKO_STREAM_'+1); Start-Sleep -Milliseconds 300; Write-Output ('UMEKO_STREAM_'+2)";
#else
        "printf 'UMEKO_STREAM_%s\\n' 1; sleep 0.3; printf 'UMEKO_STREAM_%s\\n' 2";
#endif
    QKeyEvent stream(QEvent::KeyPress,0,Qt::NoModifier,delayed);
    QCoreApplication::sendEvent(window,&stream);QTest::keyClick(window,Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(contents().contains("UMEKO_STREAM_1"),10000);
    QTRY_VERIFY_WITH_TIMEOUT(contents().contains("UMEKO_STREAM_2"),10000);
    QTest::mouseClick(window,Qt::LeftButton,Qt::NoModifier,point(terminal));
    QDir().mkpath(root+"/build/screenshots");
    window->grabWindow().save(root+"/build/screenshots/terminal-live.png");
    // Queue a final frame refresh, then close without yielding to the event loop.
    QVERIFY(QMetaObject::invokeMethod(page,"select",Q_ARG(QVariant,id)));
    m->closePage();m->setEnabled("terminal",previous);
    QTRY_VERIFY(!find("terminalPluginPage"));
    if(!previous)QTRY_VERIFY_WITH_TIMEOUT(process.isNull()||process->state()==QProcess::NotRunning,2000);
    restore.dismiss();
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
  void changeReviewLoopWidget() {
    backend->newSession(); window->setProperty("page", "chat");
    QTemporaryDir workspace; QVERIFY(workspace.isValid());
    backend->setWorkspace(QUrl::fromLocalFile(workspace.path()).toString());
    auto file = workspace.filePath("example.py");
    auto write = [&](QByteArray text) { QFile f(file); if (!f.open(QIODevice::WriteOnly)) return false; return f.write(text) == text.size(); };
    QVERIFY(write("before = 1\n"));
    auto manager = backend->desktopExtensions;
    QVERIFY(manager->entries.contains("change-review"));
    auto oldEnabled = manager->enabled("change-review");
    auto oldConfig = manager->settings["config"];
    auto cleanup = qScopeGuard([&] {manager->closePage(); manager->setEnabled("change-review",false); manager->settings["config"]=oldConfig; manager->save(); manager->setEnabled("change-review",oldEnabled); backend->newSession();});
    manager->setEnabled("change-review",true);
    QSignalSpy responses(manager,&DesktopPlugins::response), managed(manager,&DesktopPlugins::managed);
    auto await = [&](QString request) {QElapsedTimer elapsed;elapsed.start();while(elapsed.elapsed()<15000){QCoreApplication::processEvents();for(auto row:responses)if(row[0].toString()==request)return row[1].toMap();QTest::qWait(10);}return QVariantMap{};};
    auto session = backend->view;
    manager->observe(session->key,{{"type","loop_review"},{"phase","begin"},{"loopId","native-review-fixture"},{"id","begin-fixture"}});
    QTRY_VERIFY_WITH_TIMEOUT(manager->reviewRequests.isEmpty(),15000);
    QVERIFY(write("after = 2\n"));
    manager->observe(session->key,{{"type","loop_review"},{"phase","finish"},{"loopId","native-review-fixture"},{"reason","cancelled"},{"id","finish-fixture"}});
    QTRY_VERIFY_WITH_TIMEOUT(manager->reviewRequests.isEmpty(),15000);
    QTRY_COMPARE(manager->m_page,QString("change-review"));
    QTRY_VERIFY(find("changeReviewWidget"));
    auto diff=await(manager->callFor("change-review","diff",R"({"path":"example.py"})"));
    QVERIFY2(diff["ok"].toBool(),qPrintable(diff["error"].toString()));
    auto text=diff["result"].toMap()["text"].toString();
    QVERIFY(text.contains("-before = 1")); QVERIFY(text.contains("+after = 2"));
    auto worker=manager->workers.value("change-review");
    auto configRequest=manager->saveConfig("change-review",R"({"autoOpen":false})"); QVERIFY(!configRequest.isEmpty());
    QTRY_VERIFY(!manager->busy()); QTRY_VERIFY(!managed.isEmpty());
    QCOMPARE(manager->workers.value("change-review"),worker);
    auto status=await(manager->callFor("change-review","status","{}"));
    QCOMPARE(status["result"].toMap()["total"].toInt(),1);
    manager->closePage();
    manager->observe(session->key,{{"type","loop_review"},{"phase","begin"},{"loopId","native-manual"},{"id","begin-manual"}});
    QTRY_VERIFY_WITH_TIMEOUT(manager->reviewRequests.isEmpty(),15000);
    QVERIFY(write("manual = 3\n"));
    manager->observe(session->key,{{"type","loop_review"},{"phase","finish"},{"loopId","native-manual"},{"id","finish-manual"}});
    QTRY_VERIFY_WITH_TIMEOUT(manager->reviewRequests.isEmpty(),15000); QVERIFY(manager->m_page.isEmpty());
    QVERIFY(manager->openPage("change-review")); QTRY_VERIFY(find("changeReviewWidget"));
    QVERIFY(!QFileInfo::exists(workspace.filePath(".git")));
  }
  void globalAgentEmptyStateCreation() {
    configureAPI();
    backend->newSession();
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    backend->setWorkspace(QUrl::fromLocalFile(workspace.path()).toString());
    window->setProperty("page", "chat");
    window->resize(1640, 1040);
    auto manager = backend->desktopExtensions;
    auto g = manager->globalAgents();
    QVERIFY(g);
    QVERIFY(g->agents.isEmpty());
    QString id;
    auto cleanup = qScopeGuard([&] {
      if (!id.isEmpty()) g->ui("delete", {{"id", id}});
      manager->closePage();
    });
    QVERIFY(manager->openPage("global-agents"));
    QTRY_VERIFY(find("globalAgentsPage"));
    auto page = find("globalAgentsPage");
    auto dialog = page->findChild<QObject *>("createGlobalAgentDialog");
    QVERIFY(dialog);
    QTRY_VERIFY(!page->property("state").value<QJSValue>()
                     .property("defaultCharacter").toString().isEmpty());
    QTRY_VERIFY(!find("webPane")->property("moving").toBool());
    QVERIFY(find("globalAgentsList"));
    QVERIFY(!find("globalAgentsList")->isVisible());
    // Send a real pointer click, rather than invoking form() directly: an
    // empty ListView used to sit above this button and swallow the press.
    click("createFirstGlobalAgentButton");
    QTRY_VERIFY(dialog->property("opened").toBool());
    QDir().mkpath(root + "/build/global-agents-ui");
    window->grabWindow().save(root + "/build/global-agents-ui/empty-state-create.png");
    click("cancelGlobalCreation");
    QTRY_VERIFY(!dialog->property("opened").toBool());
    click("newGlobalAgentButton");
    QTRY_VERIFY(dialog->property("opened").toBool());
    click("confirmCreateGlobalAgent");
    QTRY_COMPARE(g->agents.size(), 1);
    id = g->agents.first().toMap()["id"].toString();
    QTRY_COMPARE(page->property("selected").toString(), id);
    QVERIFY(QMetaObject::invokeMethod(page, "openAgent", Q_ARG(QVariant, QString{})));
    QTRY_VERIFY(find("globalAgentsList")->isVisible());
    QCOMPARE(find("globalAgentsList")->property("count").toInt(), 1);
    QVERIFY(!find("createFirstGlobalAgentButton")->isVisible());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      point(find("globalAgentsList"), 40, 44));
    QTRY_COMPARE(page->property("selected").toString(), id);
    QVERIFY(g->ui("delete", {{"id", id}})["ok"].toBool());
    id.clear();
    QVERIFY(QMetaObject::invokeMethod(page, "openAgent", Q_ARG(QVariant, QString{})));
    QTRY_VERIFY(find("createFirstGlobalAgentButton")->isVisible());
    QVERIFY(!find("globalAgentsList")->isVisible());
    click("createFirstGlobalAgentButton");
    QTRY_VERIFY(dialog->property("opened").toBool());
    click("cancelGlobalCreation");
  }
  void globalAgentCreationBoundary() {
    configureAPI(); backend->newSession();
    auto manager=backend->desktopExtensions;auto g=manager->globalAgents();
    auto preference=g->allowAutomaticCreation;auto count=g->agents.size();
    auto enabled=manager->enabled("global-agents");manager->setEnabled("global-agents",true);
    auto created=g->ui("create",{{"name","Boundary fixture"}});QVERIFY(created["ok"].toBool());
    auto id=created["result"].toMap()["id"].toString();
    auto cleanup=qScopeGuard([&]{g->ui("delete",{{"id",id}});g->allowAutomaticCreation=preference;g->save();manager->setEnabled("global-agents",enabled);});
    g->allowAutomaticCreation=true;auto caller=g->ensure(id);
    auto overview=g->execute("agent_bus.overview",{},caller->key);
    QVERIFY(!overview["creation"].toMap()["canCreate"].toBool());
    QVERIFY(!overview["creation"].toMap()["automaticAllowed"].toBool());
    auto guide=backend->umekoRequest({{"operation","help"},{"arguments",QVariantMap{{"topic","agents"}}}},caller->key);
    QVERIFY2(guide["document"].toString().contains("agent_bus.create"),qPrintable(guide["error"].toString()));
    QVERIFY_EXCEPTION_THROWN(g->execute("agent_bus.create",{{"requestKey","recursive-fixture"}},caller->key), std::runtime_error);
    QCOMPARE(g->agents.size(),count+1);
    QVERIFY(g->execute("agent_bus.overview",{},backend->view->key)["creation"].toMap()["canCreate"].toBool());
  }
  void globalAgentDefaultsAndIdentity() {
    configureAPI();
    auto g=backend->desktopExtensions->globalAgents();QVERIFY(g);QStringList owned;
    auto cleanup=qScopeGuard([&]{for(const auto &id:owned)g->ui("delete",{{"id",id}});});
    auto kazuhiko=g->ui("create",{{"character","kazuhiko"}});QVERIFY(kazuhiko["ok"].toBool());
    const auto kazuhikoId=kazuhiko["result"].toMap()["id"].toString();owned<<kazuhikoId;
    QCOMPARE(g->agents[kazuhikoId].toMap()["name"].toString(),QString("温水和彦"));
    backend->selectCharacter("kaju");auto base=backend->characterStyle()["name"].toString();
    auto first=g->ui("create",{{"name"," "}});QVERIFY(first["ok"].toBool());auto id=first["result"].toMap()["id"].toString();owned<<id;
    QCOMPARE(g->agents[id].toMap()["name"].toString(),base);
    auto second=g->ui("create",{});QVERIFY(second["ok"].toBool());auto other=second["result"].toMap()["id"].toString();owned<<other;
    QCOMPARE(g->agents[id].toMap()["name"].toString(),base+"-1");QCOMPARE(g->agents[other].toMap()["name"].toString(),base+"-2");
    auto self=g->ensure(other);auto result=g->execute("agent_bus.overview",{},self->key);
    QCOMPARE(result["self"].toMap()["id"].toString(),"agent:"+other);QCOMPARE(result["self"].toMap()["name"].toString(),base+"-2");
    int own=0;for(auto value:result["targets"].toList()){auto row=value.toMap();if(row["isSelf"].toBool()){++own;QCOMPARE(row["id"].toString(),"agent:"+other);QVERIFY(!row["canDispatch"].toBool());}}
    QCOMPARE(own,1);QVERIFY(g->runtimeConfig(self)["agent"].toMap()["instructions"].toString().contains("\"id\":\"agent:"+other+"\""));
    QVERIFY(g->ui("update",{{"id",id},{"name",""}})["ok"].toBool());QCOMPARE(g->agents[id].toMap()["name"].toString(),base+"-1");
  }
  void globalAgentModelCreation() {
    configureAPI();backend->newSession();window->resize(1640,1040);
    auto manager=backend->desktopExtensions;auto g=manager->globalAgents();QVERIFY(g);
    auto oldPreference=g->allowAutomaticCreation;auto oldRequests=g->creationRequests;QStringList owned;
    auto cleanup=qScopeGuard([&]{for(auto id:owned)g->ui("delete",{{"id",id}});g->allowAutomaticCreation=oldPreference;g->creationRequests=oldRequests;g->changed();g->save();manager->closePage();backend->newSession();});
    QVERIFY(g->ui("setAutomaticCreation",{{"enabled",false}})["ok"].toBool());
    auto caller=backend->view;caller->title="提出创建的会话";const auto before=g->agents.size();
    auto call=[&](QString operation,QVariantMap args,QString key=QString{}){return backend->umekoRequest({{"operation",operation},{"arguments",args}},key.isEmpty()?caller->key:key);};
    auto proposal=call("agent_bus.create",{{"requestKey","native-proposal"},{"name","待确认佳树"},{"character","kaju"},{"instructions","只协调已授权任务"}});
    QVERIFY2(proposal["ok"].toBool(),qPrintable(proposal["error"].toString()));auto draft=proposal;auto request=draft["requestId"].toString();
    QCOMPARE(draft["status"].toString(),QString("awaiting_approval"));QCOMPARE(g->agents.size(),before);
    QTRY_VERIFY(find("globalAgentsPage"));QTRY_VERIFY(find("globalAgentName")&&find("globalAgentName")->isVisible());
    auto page=find("globalAgentsPage");QTRY_COMPARE(page->property("proposalId").toString(),request);
    QCOMPARE(find("globalAgentName")->property("text").toString(),QString("待确认佳树"));
    QCOMPARE(find("globalAgentInstructions")->property("text").toString(),QString("只协调已授权任务"));
    QCOMPARE(page->property("chosenCharacter").toString(),QString("kaju"));QVERIFY(find("globalAgentModel")->property("currentIndex").toInt()>=0);
    QDir().mkpath(root+"/build/global-agents-ui");QTest::qWait(300);window->grabWindow().save(root+"/build/global-agents-ui/native-creation-proposal.png");
    find("globalAgentName")->setProperty("text","用户确认的佳树");click("confirmCreateGlobalAgent");
    QTRY_COMPARE(g->agents.size(),before+1);auto done=call("agent_bus.creation_status",{{"requestId",request}});QVERIFY(done["ok"].toBool());auto created=done;owned<<created["id"].toString();
    QCOMPARE(created["status"].toString(),QString("created"));QCOMPARE(created["name"].toString(),QString("用户确认的佳树"));QCOMPARE(created["character"].toString(),QString("kaju"));
    QCOMPARE(call("agent_bus.create",{{"requestKey","native-proposal"}})["id"],created["id"]);QCOMPARE(g->agents.size(),before+1);
    QVERIFY(!g->ui("setAutomaticCreation",{{"enabled",true}},true)["ok"].toBool());
    QVERIFY(!g->ui("confirmCreation",{{"requestId",request}},true)["ok"].toBool());
    QVERIFY(!call("agent_bus.create",{{"requestKey","forged"},{"enabled",true}})["ok"].toBool());
    // Switching the visible conversation cannot change the owner of a request.
    backend->newSession();auto other=backend->view;
    QVERIFY(!call("agent_bus.creation_status",{{"requestId",request}},other->key)["ok"].toBool());
    QVERIFY(QMetaObject::invokeMethod(page,"openAgent",Q_ARG(QVariant,QString{})));QTest::qWait(750);
    auto cancelled=call("agent_bus.create",{{"requestKey","native-cancel"}});
    QTRY_COMPARE(page->property("proposalId").toString(),cancelled["requestId"].toString());
    auto resolved=page->property("chosenCharacter").toString();QVERIFY(!resolved.isEmpty());
    QCOMPARE(call("agent_bus.create",{{"requestKey","native-cancel"}})["character"].toString(),resolved);
    click("cancelGlobalCreation");QTRY_COMPARE(g->creationRequests[g->creationIndex(cancelled["requestId"].toString())].toMap()["status"].toString(),QString("cancelled"));QCOMPARE(g->agents.size(),before+1);
    QVERIFY(!g->ui("confirmCreation",{{"requestId",cancelled["requestId"]}})["ok"].toBool());
    click("allowAutomaticGlobalCreation");QTRY_VERIFY(g->allowAutomaticCreation);
    auto automatic=call("agent_bus.create",{{"requestKey","native-direct"}});QVERIFY2(automatic["ok"].toBool(),qPrintable(automatic["error"].toString()));auto autoResult=automatic;owned<<autoResult["id"].toString();
    QCOMPARE(autoResult["status"].toString(),QString("created"));QVERIFY(!autoResult["name"].toString().isEmpty());
    bool valid=false;for(auto value:backend->subagentCharacterPacks())valid|=value.toMap()["id"]==autoResult["character"];QVERIFY(valid);
    QCOMPARE(call("agent_bus.create",{{"requestKey","native-direct"}})["id"],autoResult["id"]);QCOMPARE(g->agents.size(),before+2);
    QVERIFY(g->allowed(caller,autoResult["targetId"].toString()));QVERIFY(!g->allowed(other,autoResult["targetId"].toString()));QVERIFY(g->records.isEmpty()||!g->records.contains(autoResult["id"].toString()));
    QTest::qWait(800);QVERIFY(page->property("proposalId").toString().isEmpty());window->grabWindow().save(root+"/build/global-agents-ui/native-automatic-creation.png");
    g->save();{GlobalAgents reloaded(manager);reloaded.start();QVERIFY(reloaded.allowAutomaticCreation);QCOMPARE(reloaded.execute("agent_bus.creation_status",{{"requestId",autoResult["requestId"]}},caller->key)["id"],autoResult["id"]);reloaded.close();}
    click("allowAutomaticGlobalCreation");QTRY_VERIFY(!g->allowAutomaticCreation);
    auto pending=call("agent_bus.create",{{"requestKey","native-interrupted"}});g->save();
    {GlobalAgents reloaded(manager);reloaded.start();QCOMPARE(reloaded.execute("agent_bus.creation_status",{{"requestId",pending["requestId"]}},caller->key)["status"].toString(),QString("interrupted"));QVERIFY(reloaded.snapshot("")["creationDraft"].toMap().isEmpty());reloaded.close();}
    auto index=g->creationIndex(pending["requestId"].toString());auto expired=g->creationRequests[index].toMap();expired["deadline"]=0;g->creationRequests[index]=expired;g->tick();
    QCOMPARE(call("agent_bus.creation_status",{{"requestId",pending["requestId"]}})["status"].toString(),QString("expired"));QVERIFY(!g->ui("confirmCreation",{{"requestId",pending["requestId"]}})["ok"].toBool());
  }
  void globalAgentConnectionsAndQueue() {
    configureAPI();backend->newSession();window->resize(1640,1040);auto manager=backend->desktopExtensions;auto g=manager->globalAgents();QVERIFY(g);
    auto oldBridges=g->bridges;auto oldRequests=g->requests;auto preference=g->allowAutomaticCreation;QStringList owned;
    auto cleanup=qScopeGuard([&]{for(auto id:owned)g->ui("delete",{{"id",id}});g->bridges=oldBridges;g->requests=oldRequests;g->allowAutomaticCreation=preference;g->changed();g->save();g->timer.start();manager->closePage();backend->newSession();});
    g->timer.stop();g->allowAutomaticCreation=true;auto source=backend->view;source->title="消息来源会话";
    auto created=g->execute("agent_bus.create",{{"requestKey","queue-review"},{"name","排队测试员"}},source->key);auto id=created["id"].toString();owned<<id;
    auto other=g->ui("create",{{"name","尚未连接"}})["result"].toMap()["id"].toString();owned<<other;
    auto visible=g->execute("agent_bus.overview",{},source->key)["targets"].toList();int globals=0;for(auto value:visible)if(value.toMap()["kind"]=="global"){++globals;QCOMPARE(value.toMap()["id"].toString(),"agent:"+id);QVERIFY(value.toMap()["canDispatch"].toBool());}QCOMPARE(globals,1);
    auto target=g->ensure(id);auto links=g->execute("agent_bus.connections",{},target->key);QVERIFY(links["canManage"].toBool());
    QVERIFY_EXCEPTION_THROWN(g->execute("agent_bus.set_connection",{{"sessionId",g->identity(source)},{"enabled",true},{"agentId",other}},target->key),std::runtime_error);
    target->phase="working";target->promptStarted=std::chrono::steady_clock::now()-std::chrono::seconds(73);
    auto stats=g->activity(id);QCOMPARE(stats["activity"].toString(),QString("busy: 1min13s"));
    auto send=[&](QString text){return g->execute("agent_bus.dispatch",{{"targetId","agent:"+id},{"text",text}},source->key)["requestId"].toString();};
    auto first=send("第一条消息"),second=send("第二条消息"),third=send("需要优先的消息");g->tick();QCOMPARE(g->pendingQueue(id).size(),3);
    QVERIFY(manager->openPage("global-agents"));QTRY_VERIFY(find("globalAgentsPage"));auto page=find("globalAgentsPage");QTest::qWait(750);
    QVERIFY(QMetaObject::invokeMethod(page,"openAgent",Q_ARG(QVariant,id)));QTRY_VERIFY(find("global_bus_steerQueued_2"));click("global_bus_steerQueued_2");
    QTRY_COMPARE(g->pendingQueue(id).first().toMap()["id"].toString(),third);click("global_bus_removeQueued_2");QTRY_COMPARE(g->requests[g->requestIndex(second)].toMap()["status"].toString(),QString("cancelled"));
    QTRY_COMPARE(g->pendingQueue(id).size(),2);click("global_bus_editQueued_0");QTRY_VERIFY(find("globalBusQueuePreviewText"));QVERIFY(find("globalBusQueuePreviewText")->property("text").toString().contains("需要优先的消息"));QTest::keyClick(window,Qt::Key_Escape);movePointer(QPoint(window->width()/2,window->height()/2));QDir().mkpath(root+"/build/global-agents-ui");QTest::qWait(500);window->grabWindow().save(root+"/build/global-agents-ui/native-bus-queue.png");
    // Revoking just this pair cancels its queue without opening the unrelated global.
    auto disabled=g->execute("agent_bus.set_connection",{{"sessionId",g->identity(source)},{"enabled",false}},target->key);QVERIFY(!disabled["enabled"].toBool());QVERIFY(g->pendingQueue(id).isEmpty());
    QCOMPARE(g->requests[g->requestIndex(first)].toMap()["status"].toString(),QString("cancelled"));QVERIFY(!g->allowed(source,"agent:"+other));
    QVERIFY_EXCEPTION_THROWN(g->execute("agent_bus.dispatch",{{"targetId","agent:"+id},{"text","revoked"}},source->key),std::runtime_error);
    g->execute("agent_bus.set_connection",{{"sessionId",g->identity(source)},{"enabled",true}},target->key);QVERIFY(g->allowed(source,"agent:"+id));
    QVERIFY(QMetaObject::invokeMethod(page,"openAgent",Q_ARG(QVariant,QString{})));click("agentBusTabButton");QTest::qWait(750);
    page->setProperty("busSessionId",g->identity(source));QTRY_VERIFY(find("agentBusCheckbox_"+g->identity(source)+"_"+id));
    QVERIFY(find("agentBusCheckbox_"+g->identity(source)+"_"+id)->property("checked").toBool());QVERIFY(!find("agentBusCheckbox_"+g->identity(source)+"_"+other)->property("checked").toBool());
    QTest::qWait(400);window->grabWindow().save(root+"/build/global-agents-ui/native-bus-pairs.png");
  }
  void globalAgentsWidgetAndNativeBridge() {
    releaseReplies();nativeTool=false;desktopTool=false;mediaTool=false;upstreamError=false;limitResponse=false;slow=false;
    auto previousAgent=backend->store.data["agent"].toMap();auto testAgent=previousAgent;testAgent["agentDir"]=directory.filePath("global-bridge-agent");testAgent["safeMode"]=true;testAgent["autoCompaction"]=false;backend->store.data["agent"]=testAgent;
    auto restoreAgent=qScopeGuard([&]{backend->store.data["agent"]=previousAgent;});
    configureAPI();backend->newSession();backend->selectCharacter("komari");
    auto workspace=directory.filePath("global-bridge-workspace");QDir().mkpath(workspace);backend->setWorkspace(workspace);
    window->setProperty("page","chat");window->resize(1640,1040);
    auto manager=backend->desktopExtensions;auto g=manager->globalAgents();QVERIFY(g);globalBridge=true;
    auto reset=qScopeGuard([&]{releaseReplies();globalBridge=false;for(auto id:g->agents.keys())g->ui("delete",{{"id",id}});manager->closePage();backend->newSession();});
    QVERIFY(manager->openPage("global-agents"));QTRY_VERIFY(find("globalAgentsPage"));QTest::qWait(1000);
    auto created=g->ui("create",{{"name","小鞠 · 协调员"},{"instructions","Delegate then summarize returned results"}});QVERIFY2(created["ok"].toBool(),qPrintable(created["error"].toString()));
    auto id=created["result"].toMap()["id"].toString();QCOMPARE(g->agents[id].toMap()["character"].toString(),backend->characterStyle()["id"].toString());
    QVERIFY(!g->ui("create",{{"name","forged"}},true)["ok"].toBool());
    auto target=g->identity(backend->view);auto coordinator=g->ensure(id);
    auto denied=backend->umekoRequest({{"operation","agent_bus.dispatch"},{"arguments",QVariantMap{{"targetId",target},{"text","no access"}}}},coordinator->key);QVERIFY(!denied["ok"].toBool());
    QVERIFY(g->ui("setBridge",{{"id",target},{"agentId",id},{"enabled",true}})["ok"].toBool());
    QVERIFY(QMetaObject::invokeMethod(find("globalAgentsPage"),"openAgent",Q_ARG(QVariant,id)));QTest::qWait(800);
    holdReplies=true;
    QVERIFY(g->ui("send",{{"id",id},{"text","coordinate "+target}})["ok"].toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!heldReplies.isEmpty(),10000);
    QTRY_VERIFY_WITH_TIMEOUT(!coordinator->optimistic,10000);
    int echoes=0;for(auto value:coordinator->messages)if(value.toMap()["role"]=="user"&&value.toMap()["text"]=="coordinate "+target)++echoes;
    QCOMPARE(echoes,1);QVERIFY(!coordinator->optimistic);QTest::qWait(800);
    echoes=0;for(auto value:g->snapshot(id)["messages"].toList())if(value.toMap()["role"]=="user")++echoes;QCOMPARE(echoes,1);
    releaseReplies();
    QTRY_VERIFY2_WITH_TIMEOUT(!g->requests.isEmpty()&&g->requests.last().toMap()["status"]=="completed",qPrintable(QString::fromUtf8(QJsonDocument::fromVariant(QVariantMap{{"requests",g->requests},{"source",coordinator->messages},{"target",backend->view->messages},{"approval",backend->view->approval}}).toJson())),30000);
    QTRY_VERIFY_WITH_TIMEOUT(QJsonDocument::fromVariant(g->snapshot(id)["messages"]).toJson().contains("GLOBAL_BRIDGE_SUMMARY_COMPLETE"),30000);
    QVERIFY(!coordinator->file.isEmpty());QVERIFY(!backend->view->file.isEmpty());QVERIFY(coordinator->file!=backend->view->file);
    for(auto v:backend->sessionIndex)QVERIFY(v.toMap()["id"]!=coordinator->id);
    bool restricted=false;for(auto v:requests){auto tools=v.toMap()["tools"].toList();if(tools.size()==1&&tools.first().toMap()["function"].toMap()["name"]=="umeko_desktop")restricted=true;}QVERIFY(restricted);
    // Results cannot be forged by a different session or replayed after completion.
    auto requestId=g->requests.last().toMap()["id"].toString();
    QVERIFY(!backend->umekoRequest({{"operation","agent_bus.reply"},{"arguments",QVariantMap{{"requestId",requestId},{"result","forged"}}}},coordinator->key)["ok"].toBool());
    QVERIFY(!backend->umekoRequest({{"operation","sessions.list"}},coordinator->key)["ok"].toBool());
    g->save();auto saved=U::readJson(g->registry).toMap()["agents"].toList();QVERIFY(!saved.isEmpty());QCOMPARE(saved.first().toMap()["sessionFile"].toString(),coordinator->file);
    // Backend reply text can precede both turn completion and the Widget's
    // next 650ms snapshot. A fixed sleep is not a rendering readiness signal.
    QTRY_VERIFY_WITH_TIMEOUT(!backend->view->busy()&&!coordinator->busy(),10000);
    QVERIFY(find("globalAgentsPage"));
    auto lastIndex=g->snapshot(id)["messages"].toList().size()-1;
    QTRY_VERIFY(find("widgetAssistantAvatar_"+QString::number(lastIndex)));
    QTRY_VERIFY(find("widgetCopyMessage_"+QString::number(lastIndex)));
    auto renderedCode=[&]() -> QQuickItem* {
      auto row=find("widgetChatMessage_"+QString::number(lastIndex));if(!row)return nullptr;
      QList<QQuickItem*> items{row};while(!items.isEmpty()){auto item=items.takeLast();if(item->objectName().startsWith("codeBlock_")&&item->property("text").toString().contains("print(1)"))return item;items.append(item->childItems());}return nullptr;
    };
    // Keep the exact rich-code assertion and the normal QTRY deadline. Look up
    // the current delegate each time because snapshots may replace its data.
    QTRY_VERIFY2(renderedCode(),qPrintable(QString::fromUtf8(QJsonDocument::fromVariant(g->snapshot(id)).toJson())));
    QDir().mkpath(root+"/build/global-agents-ui");window->grabWindow().save(root+"/build/global-agents-ui/native-conversation.png");
    // The same conversation remains authorized while its project is collapsed.
    auto project=backend->ensureProject(workspace);backend->renameProject(project["id"].toString(),"总线验收项目");
    QVERIFY(QMetaObject::invokeMethod(find("globalAgentsPage"),"openAgent",Q_ARG(QVariant,QString{})));QTest::qWait(750);
    QCOMPARE(find("globalSubagentsHeading")->property("text").toString(),QString("全局子智能体"));
    click("agentBusTabButton");QTRY_COMPARE(find("globalAgentsPage")->property("section").toString(),QString("bridge"));
    // The full suite leaves many projects in the virtualized list. Locate the
    // owned fixture through the same project search available to the user.
    find("agentBusSearch")->setProperty("text","总线验收项目");QTRY_COMPARE(find("agentBusProjectTree")->property("count").toInt(),2);
    auto projectKey=QDir::fromNativeSeparators(U::cleanPath(workspace));
#ifdef Q_OS_WIN
    projectKey=projectKey.toLower();
#endif
    auto folder="agentBusProject_"+projectKey;
    auto chat="agentBusConversation_"+g->identity(backend->view);auto toggle="agentBusCheckbox_"+g->identity(backend->view)+"_"+id;
    QTRY_VERIFY(find(folder));QTRY_VERIFY(find(chat));click(chat);QTRY_VERIFY(find(toggle));QVERIFY(find(toggle)->property("checked").toBool());
    window->grabWindow().save(root+"/build/global-agents-ui/native-bus-connections.png");click("agentBusBackButton");
    find("agentBusSearch")->setProperty("text","");
    auto tree=find("agentBusProjectTree");
    auto treeModel=qobject_cast<QAbstractItemModel*>(tree->property("model").value<QObject*>());QVERIFY(treeModel);
    auto role=treeModel->roleNames().key("rowData",-1);QVERIFY(role>=0);int projectIndex=-1;
    for(int i=0;i<treeModel->rowCount();++i){auto value=treeModel->data(treeModel->index(i,0),role);auto row=value.metaType().id()==qMetaTypeId<QJSValue>()?value.value<QJSValue>().toVariant().toMap():value.toMap();if(row["key"]==projectKey){projectIndex=i;break;}}
    QVERIFY(projectIndex>=0);QVERIFY(QMetaObject::invokeMethod(tree,"forceLayout"));
    QVERIFY(QMetaObject::invokeMethod(tree,"positionViewAtIndex",Q_ARG(int,projectIndex),Q_ARG(int,0)));QTest::qWait(200);
    QTRY_VERIFY(find(folder));
    click(folder);QTRY_VERIFY(!find(chat));QVERIFY(g->allowed(backend->view,"agent:"+id));
    click(folder);QTRY_VERIFY(find(chat));
    find("agentBusSearch")->setProperty("text","总线验收项目");QTRY_COMPARE(find("agentBusProjectTree")->property("count").toInt(),2);
    window->grabWindow().save(root+"/build/global-agents-ui/native-agent-bus.png");
    click("globalSubagentsHelpButton");QTRY_VERIFY(find("globalSubagentsHelpText"));
    QCOMPARE(find("globalSubagentsHelpTitle")->property("text").toString(),QString("Agent Bus（智能体总线）"));
    QVERIFY(find("globalSubagentsHelpText")->property("text").toString().contains("勾选"));
    QTest::keyClick(window,Qt::Key_Escape);QTest::qWait(200);
    click("agentBusRequestsTabButton");click("globalSubagentsHelpButton");QTRY_VERIFY(find("globalSubagentsHelpText"));
    QCOMPARE(find("globalSubagentsHelpTitle")->property("text").toString(),QString("请求"));
    QVERIFY(find("globalSubagentsHelpText")->property("text").toString().contains("返回结果"));
    QVERIFY(find("globalSubagentsHelpText")->property("text").toString().contains("不会撤销"));
    window->grabWindow().save(root+"/build/global-agents-ui/native-requests-help.png");
    QTest::keyClick(window,Qt::Key_Escape);

    // A real ordinary native Pi session can initiate a bus message and receive
    // the global subagent's correlated result in its original chat.
    QTRY_VERIFY_WITH_TIMEOUT(!backend->view->busy(),10000);QTRY_VERIFY_WITH_TIMEOUT(!coordinator->busy(),10000);
    auto before=g->requests.size();backend->send("coordinate agent:"+id);
    QTRY_VERIFY_WITH_TIMEOUT(g->requests.size()>before&&g->requests.last().toMap()["source"]==g->identity(backend->view)&&g->requests.last().toMap()["status"]=="completed",30000);
    QTRY_VERIFY_WITH_TIMEOUT(lastText().contains("GLOBAL_BRIDGE_SUMMARY_COMPLETE"),30000);
    QTRY_VERIFY_WITH_TIMEOUT(!coordinator->busy(),10000);
    auto queued=backend->umekoRequest({{"operation","agent_bus.dispatch"},{"arguments",QVariantMap{{"targetId",g->identity(backend->view)},{"text","cancel this request"}}}},coordinator->key);QVERIFY(queued["ok"].toBool());
    QVERIFY(g->ui("setBridge",{{"id",g->identity(backend->view)},{"agentId",id},{"enabled",false}})["ok"].toBool());QCOMPARE(g->requests.last().toMap()["status"].toString(),QString("cancelled"));
    // A rendered final chunk precedes the native done event. Drain both ends
    // and update the observer before later tests subscribe to pet reactions.
    QTRY_VERIFY_WITH_TIMEOUT(!backend->view->busy(),10000);
    QTRY_VERIFY_WITH_TIMEOUT(!coordinator->busy()&&g->outbox.isEmpty(),10000);
    if(auto pet=qobject_cast<DesktopPet*>(manager->pet()))pet->sample();
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
  void nativePetReminderWidgetAndBubble() {
    auto pet=qobject_cast<DesktopPet*>(backend->desktopExtensions->pet());QVERIFY(pet);
    const auto original=pet->config();QString id;
    const auto cleanup=qScopeGuard([&]{if(!id.isEmpty())pet->ui("deleteItem",{{"id",id}});pet->ui("configure",original);window->setProperty("page","chat");window->requestActivate();});
    QVERIFY(pet->ui("configure",{{"visible",true},{"paused",true},{"reminderNotificationsEnabled",true}})["ok"].toBool());
    QTRY_VERIFY_WITH_TIMEOUT(window->findChild<QQuickWindow*>("desktopPetWindow"),5000);
    const auto result=pet->ui("createItem",{{"title","离线提醒验收"},{"notes","无需模型和网络"},{"dueAt",QDateTime::currentDateTimeUtc().addSecs(-1).toString(Qt::ISODate)}});
    QVERIFY2(result["ok"].toBool(),qPrintable(result["error"].toString()));id=result["result"].toMap()["item"].toMap()["id"].toString();
    QTRY_COMPARE_WITH_TIMEOUT(pet->reminderCard().size(),1,3000);
    auto petWindow=window->findChild<QQuickWindow*>("desktopPetWindow");
    QTRY_VERIFY_WITH_TIMEOUT(petWindow->findChild<QQuickItem*>("desktopPetReminderCard")->isVisible(),3000);
    QCOMPARE(petWindow->findChild<QObject*>("desktopPetReminderCardTitle")->property("text").toString(),QString("离线提醒验收"));
    const auto revision=pet->reminderCard().first().toMap()["revision"].toInt();
    pet->completeReminder(id,revision-1);QCOMPARE(pet->reminderCard().size(),1); // Stale UI cannot complete a revised item.
    pet->openReminders(id,false);
    QTRY_VERIFY_WITH_TIMEOUT(find("desktopPetReminderTitle"),5000);
    QTRY_COMPARE_WITH_TIMEOUT(find("desktopPetReminderTitle")->property("text").toString(),QString("离线提醒验收"),5000);
    QTRY_VERIFY_WITH_TIMEOUT(find("desktopPetReminderList"),5000);
    QDir().mkpath(root+"/build/screenshots");QVERIFY(window->grabWindow().save(root+"/build/screenshots/native-pet-reminders.png"));
    pet->completeReminder(id,revision);QTRY_VERIFY(pet->reminderCard().isEmpty());
    QCOMPARE(pet->ui("listItems",{{"status","completed"}})["result"].toMap()["items"].toList().size(),1);
  }
  void desktopPetImportsDataWithoutCode() {
    auto pet=qobject_cast<DesktopPet*>(backend->desktopExtensions->pet());QVERIFY(pet);
    QSet<QString> builtins;
    for (const auto &row : pet->appearances()) if (row.toMap()["builtin"].toBool()) builtins.insert(row.toMap()["id"].toString());
    QVERIFY(builtins.contains("yanami"));QVERIFY(builtins.contains("komari"));
    QVERIFY(pet->ui("selectAppearance",{{"id","komari"}})["ok"].toBool());
    QCOMPARE(pet->state()["appearance"].toString(),"komari");
    QVERIFY(pet->state()["source"].toString().contains("/assets/komari/"));
    QVERIFY(!pet->ui("removeAppearance",{{"id","komari"}})["ok"].toBool());
    for (const auto &state : QStringList{"idle","thinking","working","waiting","done","error"}) {
      QImageReader reader(root + "/umeko-plugins/desktop-pet/assets/komari/" + state + ".webp");
      QVERIFY(reader.canRead());QVERIFY(reader.imageCount()>1);QCOMPARE(reader.size(),QSize(384,576));QVERIFY(!reader.read().isNull());
    }
    QTemporaryDir temp;QImage image(64,64,QImage::Format_ARGB32);image.fill(QColor("#4488ff"));auto source=temp.filePath("sample.png");QVERIFY(image.save(source));
    QSignalSpy imported(pet,&DesktopPet::imported);auto result=pet->ui("importAppearance",{{"path",source}});QVERIFY(result["ok"].toBool());QTRY_VERIFY_WITH_TIMEOUT(!imported.isEmpty(),10000);
    auto actual=imported.last()[1].toMap();QVERIFY2(actual["ok"].toBool(),qPrintable(actual["error"].toString()));auto id=actual["result"].toMap()["appearance"].toString();QVERIFY(id.startsWith("custom-"));QVERIFY(pet->ui("selectAppearance",{{"id",id}})["ok"].toBool());QVERIFY(pet->state()["source"].toString().endsWith(".png"));QVERIFY(!pet->state()["animated"].toBool());
    QVERIFY(pet->ui("removeAppearance",{{"id",id}})["ok"].toBool());QCOMPARE(pet->config()["appearance"].toString(),"yanami");QVERIFY(!pet->ui("removeAppearance",{{"id","yanami"}})["ok"].toBool());
    const auto shadow=temp.filePath("shadow");QVERIFY(QDir().mkpath(shadow));QVERIFY(QFile::copy(source,shadow+"/sample.png"));
    QVERIFY(U::writeJson(shadow+"/pet.json",QVariantMap{{"format","umeko-pet"},{"version",1},{"id","komari"},{"name","Shadow"},{"states",QVariantMap{{"idle","sample.png"}}}}));
    imported.clear();QVERIFY(pet->ui("importAppearance",{{"path",shadow+"/pet.json"}})["ok"].toBool());QTRY_VERIFY_WITH_TIMEOUT(!imported.isEmpty(),10000);QVERIFY(!imported.last()[1].toMap()["ok"].toBool());
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
  void checkpointLinearHistoryDemo() {
    backend->newSession();
    QTemporaryDir workspace; QVERIFY(workspace.isValid());
    backend->setWorkspace(QUrl::fromLocalFile(workspace.path()).toString());
    auto manager = backend->desktopExtensions;
    const auto oldConfig = manager->settings["config"];
    const auto oldEnabled = manager->enabled("change-review");
    const auto cleanup = qScopeGuard([&] {manager->closePage();manager->settings["config"]=oldConfig;manager->save();manager->setEnabled("change-review",oldEnabled);backend->newSession();});
    manager->setEnabled("change-review",true);
    QVERIFY(manager->openPage("change-review"));
    QTRY_VERIFY_WITH_TIMEOUT(manager->workers.value("change-review") && manager->workers.value("change-review")->ready,10000);
    QSignalSpy managed(manager,&DesktopPlugins::managed), responses(manager,&DesktopPlugins::response);
    auto configuration = manager->saveConfig("change-review",R"({"autoOpen":false,"checkpoints":true})");
    QVERIFY(!configuration.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!managed.isEmpty(),10000);
    QTRY_COMPARE_WITH_TIMEOUT(manager->busy(),false,10000);
    QDir().mkpath(workspace.filePath("docs"));
    auto write = [&](QByteArray code,QByteArray docs) {
      QFile source(workspace.filePath("src.py"));QVERIFY(source.open(QIODevice::WriteOnly));QVERIFY(source.write(code)==code.size());source.close();
      QFile guide(workspace.filePath("docs/usage.md"));QVERIFY(guide.open(QIODevice::WriteOnly));QVERIFY(guide.write(docs)==docs.size());
    };
    auto await = [&](QString id) {
      QElapsedTimer elapsed;elapsed.start();
      while(elapsed.elapsed()<20000) {QCoreApplication::processEvents();for(const auto &row:responses)if(row[0].toString()==id)return row[1].toMap();QTest::qWait(10);}
      return QVariantMap{};
    };
    auto timeline = [&] {return await(manager->callFor("change-review","timeline","{}"))["result"].toMap();};
    auto turn = [&](QString id,QString title,QByteArray code,QByteArray docs) {
      manager->observe(backend->view->key,{{"type","loop_review"},{"phase","begin"},{"loopId",id},{"title",title},{"id","begin-"+id}});
      QTRY_VERIFY_WITH_TIMEOUT(manager->reviewRequests.isEmpty(),15000);
      write(code,docs);
      manager->observe(backend->view->key,{{"type","loop_review"},{"phase","finish"},{"loopId",id},{"title",title},{"reason","completed"},{"id","finish-"+id}});
      QTRY_VERIFY_WITH_TIMEOUT(manager->reviewRequests.isEmpty(),15000);
      QVERIFY(await(manager->callFor("change-review","status","{}"))["ok"].toBool());
    };
    write("version = 'A'\n","# A\n");
    turn("demo-b","实现配置加载并更新文档","version = 'B'\n","# B\n\nTwo files changed.\n");
    turn("demo-c","增加请求重试并记录版本历史","version = 'C'\n","# C\n\nRetry and history.\n");
    auto result = timeline();
    QCOMPARE(result["entries"].toList().size(),3);
    const auto c = result["entries"].toList()[0].toMap()["checkpointId"].toString();
    const auto b = result["entries"].toList()[1].toMap()["checkpointId"].toString();
    const auto a = result["entries"].toList()[2].toMap()["checkpointId"].toString();
    QVERIFY(!a.isEmpty() && !b.isEmpty() && !c.isEmpty());
    QCOMPARE(result["currentId"].toString(),c);
    auto host = qobject_cast<WidgetHost *>(manager->hostServices());
    QSignalSpy hostResponses(manager,&DesktopPlugins::hostResponse);
    auto lock = [&](QString checkpoint) {
      const auto id=U::uuid().remove('-');host->requestFor("change-review",{{"id",id},{"operation","checkpoints.acquire"},{"arguments",QVariantMap{{"id",checkpoint}}},{"session",manager->m_session}});
      QElapsedTimer elapsed;elapsed.start();
      while(elapsed.elapsed()<5000) {QCoreApplication::processEvents();for(const auto &row:hostResponses)if(row[1].toString()==id){if(!row[2].toMap()["ok"].toBool())return QString{};return row[2].toMap()["result"].toMap()["token"].toString();}QTest::qWait(10);}
      return QString{};
    };
    auto release = [&](QString token) {host->requestFor("change-review",{{"id",U::uuid().remove('-')},{"operation","checkpoints.release"},{"arguments",QVariantMap{{"token",token}}},{"session",manager->m_session}});};
    auto restore = [&](QString checkpoint) {
      auto plan=await(manager->callFor("change-review","restorePreview",QJsonDocument::fromVariant(QVariantMap{{"checkpointId",checkpoint}}).toJson(QJsonDocument::Compact)));
      QVERIFY(plan["ok"].toBool());QCOMPARE(plan["result"].toMap()["conflicts"].toInt(),0);
      const auto token=lock(checkpoint);QVERIFY(!token.isEmpty());
      auto restored=await(manager->callFor("change-review","restore",QJsonDocument::fromVariant(QVariantMap{{"checkpointId",checkpoint},{"lockToken",token}}).toJson(QJsonDocument::Compact)));
      release(token);QVERIFY(restored["ok"].toBool());QVERIFY(restored["result"].toMap()["ok"].toBool());
    };
    restore(b);QCOMPARE(timeline()["currentId"].toString(),b);
    QCOMPARE(timeline()["futureCount"].toInt(),1);
    QVERIFY(window->grabWindow().save(root+"/build/screenshots/checkpoint-current-and-future.png"));
    restore(c);QCOMPARE(timeline()["currentId"].toString(),c);
    restore(b);
    turn("demo-d","加入缓存和结构化日志","version = 'D'\n","# D\n\nCache and logs.\n");
    result=timeline();QCOMPARE(result["currentId"].toString(),result["entries"].toList().first().toMap()["checkpointId"].toString());
    QCOMPARE(result["entries"].toList().size(),3);
    for(const auto &entry:result["entries"].toList())QVERIFY(entry.toMap()["checkpointId"].toString()!=c);
    const auto current=result["currentId"].toString();
    auto widget=find("changeReviewWidget");QVERIFY(widget);
    QTRY_VERIFY_WITH_TIMEOUT(widget->property("timelineRows").toList().size()==3,15000);
    click("changeReviewTimelineRow_"+current);
    QTRY_VERIFY_WITH_TIMEOUT(find("changeReviewRow_src.py"),15000);
    QVERIFY(find("changeReviewRow_docs"));click("changeReviewRow_docs");
    QTRY_VERIFY_WITH_TIMEOUT(find("changeReviewRow_docs/usage.md"),5000);
    click("changeReviewRow_src.py");
    QTRY_VERIFY_WITH_TIMEOUT(widget->property("diff").toString().contains("-version = 'B'") && widget->property("diff").toString().contains("+version = 'D'"),15000);
    QCOMPARE(widget->property("viewMode").toString(),QString("diff"));
    QVERIFY(window->grabWindow().save(root+"/build/screenshots/checkpoint-linear-demo.png"));
    manager->closePage();QVERIFY(manager->openPage("change-review"));
    QTRY_COMPARE_WITH_TIMEOUT(timeline()["currentId"].toString(),current,15000);
    QFile proof(workspace.filePath("src.py"));QVERIFY(proof.open(QIODevice::ReadOnly));QCOMPARE(proof.readAll(),QByteArray("version = 'D'\n"));
  }
  void checkpointBackgroundJobsBlockLocks() {
    backend->newSession();
    QTemporaryDir workspace; QVERIFY(workspace.isValid());
    backend->setWorkspace(QUrl::fromLocalFile(workspace.path()).toString());
    auto manager = backend->desktopExtensions;
    const auto oldEnabled = manager->enabled("change-review");
    const auto cleanup = qScopeGuard([&] {manager->closePage();manager->setEnabled("change-review",oldEnabled);backend->newSession();});
    manager->setEnabled("change-review",true);
    QVERIFY(manager->openPage("change-review"));
    auto host = qobject_cast<WidgetHost *>(manager->hostServices());
    QTRY_VERIFY_WITH_TIMEOUT(manager->workers.value("change-review") && manager->workers.value("change-review")->ready,10000);
    QSignalSpy responses(manager,&DesktopPlugins::hostResponse);
    auto process = new QProcess(backend.get());
    backend->view->process = process;
    backend->view->ready = true;
    backend->view->stale = false;
    auto reply = [&](QVariantMap event) {
      const auto id=U::uuid().remove('-');
      host->requestFor("change-review",{{"id",id},{"operation","checkpoints.acquire"},{"arguments",QVariantMap{{"id",U::uuid().remove('-')}}},{"session",manager->m_session}});
      QCoreApplication::processEvents();
      event["type"]="widget_catalog";event["id"]=id;
      host->observe(backend->view->key,event);
      QElapsedTimer elapsed;elapsed.start();
      while(elapsed.elapsed()<5000) {QCoreApplication::processEvents();for(const auto &row:responses)if(row[1].toString()==id)return row[2].toMap();QTest::qWait(10);}
      return QVariantMap{};
    };
    auto result=reply({{"ok",true},{"value",QVariantMap{{"jobs",QVariantList{QVariantMap{{"cwd",workspace.filePath("nested")},{"status","running"}}}}}}});
    QVERIFY(!result["ok"].toBool());
    QVERIFY(result["error"].toString().contains("后台任务"));
    QVERIFY(!manager->checkpointLocked(workspace.path()));
    result=reply({{"ok",false},{"error","catalog failure"}});
    QVERIFY(!result["ok"].toBool());
    QVERIFY(!manager->checkpointLocked(workspace.path()));
    result=reply({{"ok",true},{"value",QVariantMap{{"jobs",QVariantList{}}}}});
    QVERIFY(result["ok"].toBool());
    const auto token=result["result"].toMap()["token"].toString();
    QVERIFY(!token.isEmpty());
    QVERIFY(manager->checkpointLocked(workspace.path()));
    const auto id=U::uuid().remove('-');
    host->requestFor("change-review",{{"id",id},{"operation","checkpoints.release"},{"arguments",QVariantMap{{"token",token}}},{"session",manager->m_session}});
    QTRY_VERIFY_WITH_TIMEOUT(!manager->checkpointLocked(workspace.path()),5000);
  }
  void checkpointHostLock() {
    backend->newSession();
    QTemporaryDir workspace; QVERIFY(workspace.isValid());
    backend->setWorkspace(QUrl::fromLocalFile(workspace.path()).toString());
    auto manager = backend->desktopExtensions;
    const auto oldEnabled = manager->enabled("change-review");
    const auto cleanup = qScopeGuard([&] {manager->closePage();manager->setEnabled("change-review",oldEnabled);backend->newSession();});
    manager->setEnabled("change-review",true);
    QVERIFY(manager->openPage("change-review"));
    auto host = qobject_cast<WidgetHost *>(manager->hostServices());
    QTRY_VERIFY_WITH_TIMEOUT(manager->workers.value("change-review") && manager->workers.value("change-review")->ready,10000);
    QSignalSpy responses(manager,&DesktopPlugins::hostResponse);
    auto call = [&](QString operation,QVariantMap args) {
      const auto id = U::uuid().remove('-');
      const auto request = host->requestFor("change-review",{{"id",id},{"operation",operation},{"arguments",args},{"session",manager->m_session}});
      if (request.isEmpty()) return QVariantMap{};
      QElapsedTimer elapsed;elapsed.start();
      while(elapsed.elapsed()<5000) {QCoreApplication::processEvents();for(const auto &row:responses)if(row[0].toString()=="change-review" && row[1].toString()==id)return row[2].toMap();QTest::qWait(10);}
      return QVariantMap{};
    };
    auto acquired = call("checkpoints.acquire",{{"id",U::uuid().remove('-')}});
    QVERIFY(acquired["ok"].toBool());
    const auto token = acquired["result"].toMap()["token"].toString();
    QVERIFY(!token.isEmpty());
    QVERIFY(manager->checkpointLocked(workspace.path()));
    const auto before = backend->messages().size();
    backend->send("must not start while restoring");
    QCOMPARE(backend->messages().size(),before);
    QVERIFY(!backend->lastSendError().isEmpty());
    auto second = call("checkpoints.acquire",{{"id",U::uuid().remove('-')}});
    QVERIFY(!second["ok"].toBool());
    QVERIFY(call("checkpoints.release",{{"token",token}})["ok"].toBool());
    QVERIFY(!manager->checkpointLocked(workspace.path()));
    QVERIFY(!call("checkpoints.release",{{"token",token}})["ok"].toBool());
    acquired = call("checkpoints.acquire",{{"id",U::uuid().remove('-')}});
    QVERIFY(acquired["ok"].toBool());
    host->stop("change-review");
    QVERIFY(!manager->checkpointLocked(workspace.path()));
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
  void composerNarrowWidthTransitions() {
    configureAPI();
    const auto scale=backend->fontScale();
    const auto restore=qScopeGuard([&]{backend->setFontScale(scale);});
    QQmlComponent component(engine.get(),QUrl::fromLocalFile(root+"/app/qml/Composer.qml"));
    QVERIFY2(component.isReady(),qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.createWithInitialProperties({
        {"parent",QVariant::fromValue(window->contentItem())},{"width",850}}));
    auto item=qobject_cast<QQuickItem*>(object.get());QVERIFY(item);
    auto child=[&](const char*name){return object->findChild<QQuickItem*>(name);};
    const auto geometry=[](QQuickItem*i){return i->mapRectToScene(QRectF(0,0,i->width(),i->height()));};
    for(double factor:{1.,1.5,2.})for(int width:{850,320,220,120,40}){
      backend->setFontScale(factor);item->setWidth(width);QTest::qWait(40);
      auto model=child("modelSelector"),send=child("composerSendControls"),footer=child("composerToolbar");
      QVERIFY(model && send && footer);
      const auto m=geometry(model),s=geometry(send),f=geometry(footer);
      const auto diagnostic=QString("width=%1 scale=%2 model=[%3,%4] send=[%5,%6] footer=[%7,%8]")
          .arg(width).arg(factor).arg(m.left()).arg(m.right()).arg(s.left()).arg(s.right()).arg(f.left()).arg(f.right());
      QVERIFY2(m.right()<=s.left()+.5 && s.right()<=f.right()+.5,qPrintable(diagnostic));
      for(const char*name:{"composerProviderName","composerModelName","composerModelChevron"}){
        auto label=child(name);QVERIFY(label);
        if(geometry(label).right()>m.right()+.5)for(auto ancestor=label;ancestor && ancestor!=model->parentItem();ancestor=ancestor->parentItem())
          qInfo()<<"Composer boundary ancestor:"<<ancestor->objectName()<<"x"<<ancestor->x()<<"width"<<ancestor->width()<<"visible"<<ancestor->isVisible()<<"scene"<<geometry(ancestor);
        QVERIFY2(geometry(label).right()<=m.right()+.5,qPrintable(QString(name)+" "+diagnostic));
      }
    }
  }
  void composerClosingPaneKeepsControlsInBounds() {
    configureAPI();backend->newSession();window->setProperty("page","chat");
    const auto old=window->size();const auto scale=backend->fontScale();
    const auto collapsed=window->property("sidebarCollapsed");
    auto pane=find("webPane");QVERIFY(pane);
    const auto restore=qScopeGuard([&]{QMetaObject::invokeMethod(pane,"dismiss");backend->setFontScale(scale);window->resize(old);window->setProperty("sidebarCollapsed",collapsed);});
    window->setProperty("sidebarCollapsed",false);backend->setFontScale(2.);window->resize(720,850);
    pane->setProperty("opened",true);
    QTRY_VERIFY_WITH_TIMEOUT(pane->width()>300 && !pane->property("moving").toBool(),3000);
    QVERIFY(QMetaObject::invokeMethod(pane,"dismiss"));
    // Exercise the shrinking pane, not just its settled zero-width state.
    QElapsedTimer closing;closing.start();
    while(closing.elapsed()<600){
      QTest::qWait(16);
      auto model=find("modelSelector"),send=find("composerSendControls"),footer=find("composerToolbar");
      QVERIFY(model && send && footer);
      const auto rect=[](QQuickItem*i){return i->mapRectToScene(QRectF(0,0,i->width(),i->height()));};
      const auto m=rect(model),s=rect(send),f=rect(footer);
      const auto diagnostic=QString("closing=%1ms pane=%2 footer=%3 model=[%4,%5] send=[%6,%7] collapsed=%8")
          .arg(closing.elapsed()).arg(pane->width()).arg(footer->width()).arg(m.left()).arg(m.right()).arg(s.left()).arg(s.right()).arg(window->property("sidebarLayoutCollapsed").toBool());
      QVERIFY2(footer->width()>0,qPrintable(diagnostic));
      QVERIFY2(m.right()<=s.left()+.5 && s.right()<=f.right()+.5,qPrintable(diagnostic));
    }
  }
  void composerResponsiveFooter() {
    configureAPI();backend->newSession();window->setProperty("page","chat");
    const auto old=window->size();const auto scale=backend->fontScale();
    const auto restore=qScopeGuard([&]{backend->setFontScale(scale);window->resize(old);});
    const auto geometry=[](QQuickItem *i){return i->mapRectToScene(QRectF(0,0,i->width(),i->height()));};
    for(const auto factor:QList<double>{1.,1.5,2.})for(const auto width:QList<int>{720,940,1280}) {
      backend->setFontScale(factor);window->resize(width,850);QTest::qWait(250);
      auto model=find("modelSelector"),send=find("composerSendControls"),utility=find("composerUtilityControls"),footer=find("composerToolbar");
      QVERIFY(model && send && utility && footer);
      const auto m=geometry(model),s=geometry(send),u=geometry(utility),f=geometry(footer);
      qInfo()<<"Composer geometry:"<<"requested"<<width<<"actual"<<window->width()<<"scale"<<factor<<"footer"<<f<<"model"<<m<<"send"<<s
             <<"pane"<<find("webPane")->width()<<"pane opened"<<find("webPane")->property("opened")<<"model labels"<<geometry(find("composerModelName"))<<geometry(find("composerModelChevron"));
      QVERIFY2(!m.intersects(s),qPrintable(QString("model/send overlap at %1 scale %2").arg(width).arg(factor)));
      QVERIFY(!m.intersects(u));QVERIFY(m.left()>=f.left()-.5);QVERIFY(m.right()<=s.left()+.5);QVERIFY(s.right()<=f.right()+.5);
      auto provider=find("composerProviderName"),name=find("composerModelName"),chevron=find("composerModelChevron");
      QVERIFY(!provider->isVisible() || geometry(provider).right()<=m.right()+.5);
      QVERIFY(geometry(name).right()<=m.right()+.5);QVERIFY(geometry(chevron).right()<=m.right()+.5);
    }
    backend->setFontScale(1.);window->resize(1060,760);QTest::qWait(200);
    QVERIFY(window->grabWindow().save(root+"/build/screenshots/composer-fixed.png"));
  }
  void nativeDshAcpLifecycleAndHistory() {
    configureAPI();backend->newSession();window->setProperty("page","chat");
    const auto old=backend->store.data.value("dsh");
    const auto restore=qScopeGuard([&]{backend->retire(backend->view);backend->store.data["dsh"]=old;backend->newSession();qunsetenv("FAKE_ACP_NATIVE_MODEL");});
    const auto folder=directory.filePath("fake-dsh/lib");QDir().mkpath(folder);
    QFile input(root+"/tests/fixtures/fake-acp.mjs");QVERIFY(input.open(QIODevice::ReadOnly));auto peer=input.readAll();
    peer.replace("options: [{ name: 'First', value: 'opaque:model:first' }", "options: [{ name: 'First', value: process.env.FAKE_ACP_NATIVE_MODEL || 'opaque:model:first' }");
    peer.replace("import { createInterface }", "import { readFileSync } from 'node:fs';\nimport { createInterface }");
    peer.replace("lines.on('line', line => {", "lines.on('line', async line => {");
    peer.replace("case 'session/prompt': {", R"JS(case 'session/prompt': {
      if(params.prompt.at(-1).text==='proxy-request') {
        const patch=JSON.parse(readFileSync(process.env.DSH_HOME+'/profiles/acp/cordis.patch.yml','utf8'));
        const selection=patch.find(p=>p.id==='acp').config;const config=patch.find(p=>p.id==='llm-pi-ai').config.providers[selection.provider];
        const response=await fetch(config.baseURL+'/chat/completions',{method:'POST',headers:{'Content-Type':'application/json',Authorization:'Bearer '+process.env[config.apiKeyEnv]},body:JSON.stringify({model:selection.model,messages:[{role:'user',content:'Native DSH proxy route'}],max_tokens:256,stream:false})});
        const body=await response.json();
        update({sessionUpdate:'agent_message_chunk',content:{type:'text',text:body.choices?.[0]?.message?.content || JSON.stringify(body)}});result(message.id,{stopReason:'end_turn'});break;
      }
)JS");
    const auto script=folder+"/bin.js";QFile output(script);QVERIFY(output.open(QIODevice::WriteOnly));output.write(peer);output.close();
    QVERIFY(U::writeJson(directory.filePath("fake-dsh/package.json"),QVariantMap{{"name","@deepseek-ai/dsh"},{"version","0.2.2-rc.1"},{"type","module"}}));
    backend->dshRuntime->setPath(script);backend->dshRuntime->check();QTRY_VERIFY_WITH_TIMEOUT(!backend->dshRuntime->busy(),15000);QVERIFY2(backend->dshRuntime->ready(),qPrintable(backend->dshRuntime->status()));
    const auto pair=QJsonDocument(QJsonArray{backend->view->provider,backend->view->model}).toJson(QJsonDocument::Compact);qputenv("FAKE_ACP_NATIVE_MODEL",pair);
    QVERIFY(backend->setSessionBackend("dsh"));QVERIFY(backend->canChooseBackend());
    backend->send("Native ACP fixture");QVERIFY(!backend->canChooseBackend());QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),15000);
    QVERIFY2(lastText().contains("Native ACP fixture"),qPrintable(lastText()));QCOMPARE(backend->view->backend,QString("dsh"));QCOMPARE(backend->view->usage["tokens"].toInt(),25);
    QVERIFY(!backend->compactionAvailable());QVERIFY(!backend->canGrantDangerousApproval());
    const auto requestCount=requests.size();backend->send("proxy-request");QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),10000);QVERIFY2(lastText().contains("Native DSH proxy route"),qPrintable(lastText()));QVERIFY(requests.size()>requestCount);
    const auto s=backend->view;QVERIFY(!s->acpSessionId.isEmpty());QVERIFY(!s->dshOwn);
    auto patch=U::readJson(s->dshHome+"/profiles/acp/cordis.patch.yml",QVariantList{}).toList();bool routed=false;
    for(const auto &v:patch)if(v.toMap()["id"]=="llm-pi-ai") {auto providers=v.toMap()["config"].toMap()["providers"].toMap();auto account=providers[s->provider].toMap();QCOMPARE(account["baseURL"].toString(),QString("http://127.0.0.1:%1/v1").arg(backend->gatewayPort));QCOMPARE(account["apiKeyEnv"].toString(),QString("UMEKO_DSH_PROXY_TOKEN"));routed=true;}
    QVERIFY(routed);QVERIFY(!QString::fromUtf8(QJsonDocument::fromVariant(patch).toJson()).contains(backend->privateToken));
    backend->send("permission");QTRY_VERIFY_WITH_TIMEOUT(!backend->pendingApproval().isEmpty(),5000);QCOMPARE(backend->pendingApproval()["backend"].toString(),QString("dsh"));backend->respondApproval(false);QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),5000);QVERIFY(lastText().contains("permission:deny"));
    backend->setToolPermissionMode("ask");backend->send("permission-mode");QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),5000);QCOMPARE(lastText(),QString("ask"));backend->setToolPermissionMode("auto");
    backend->send("wait");QTRY_VERIFY_WITH_TIMEOUT(s->ready && s->busy(),5000);QTest::qWait(100);backend->send("queued message");QCOMPARE(s->queue.size(),1);backend->stop();QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),5000);QVERIFY(s->dshInterrupted);QVERIFY(s->draft.contains("queued message"));
    QTRY_VERIFY_WITH_TIMEOUT(find("dshInterruptionNotice_"+QString::number(backend->chat.rowCount()-1)),5000);
    QVERIFY(!find("assistantAvatar_"+QString::number(backend->chat.rowCount()-1))->isVisible());
    const auto prior=s->messages.size();backend->send("prompt-blocks");QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),5000);QVERIFY(lastText().contains("用户已停止上一条请求"));QVERIFY(lastText().contains("prompt-blocks"));QVERIFY(!s->dshInterrupted);QVERIFY(s->messages.size()>prior);
    backend->dshWriter.waitForFinished();backend->flushDsh();backend->dshWriter.waitForFinished();const auto id=s->id;backend->retire(s);backend->newSession();backend->records.remove(s->key);backend->openSession(id);
    QTRY_COMPARE_WITH_TIMEOUT(backend->activeSession(),id,5000);QCOMPARE(backend->view->backend,QString("dsh"));QCOMPARE(backend->view->acpSessionId,s->acpSessionId);QCOMPARE(backend->view->messages,s->messages);QVERIFY(!backend->view->process);
    backend->send("restored session");QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),10000);QVERIFY(lastText().contains("restored session"));
    backend->retire(backend->view);backend->newSession();backend->dshRuntime->setUseOwnConfig(true);QVERIFY(backend->setSessionBackend("dsh"));backend->send("own config");QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),10000);QVERIFY(lastText().contains("own config"));QVERIFY(backend->dshOwnConfig());QVERIFY(!backend->dshModelProviders().isEmpty());QVERIFY(backend->selectDshModel("opaque:model:second"));QTRY_COMPARE_WITH_TIMEOUT(backend->dshCurrentModelValue(),QString("opaque:model:second"),5000);
    const auto ownId=backend->view->id;backend->renameSession(ownId,"ACP renamed");QCOMPARE(backend->view->title,QString("ACP renamed"));
    backend->deleteSession(ownId);QTRY_VERIFY_WITH_TIMEOUT(backend->view->id!=ownId,5000);
    QVERIFY(!QFileInfo::exists(directory.filePath("dsh/transcripts/"+ownId+".json")));
  }
  void nativeDshOfficialProxyFixture() {
    const auto runtime=qEnvironmentVariable("UMEKO_TEST_DSH");if(runtime.isEmpty())QSKIP("Official dsh is optional; the ACP peer fixture always runs");
    configureAPI();backend->newSession();window->setProperty("page","chat");
    const auto old=backend->store.data.value("dsh");
    const auto restore=qScopeGuard([&]{backend->retire(backend->view);backend->store.data["dsh"]=old;backend->newSession();});
    backend->dshRuntime->setPath(runtime);backend->dshRuntime->setUseOwnConfig(false);QVERIFY(backend->setSessionBackend("dsh"));
    const auto count=requests.size();backend->send("Official DSH via native fixture proxy");QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),45000);
    QVERIFY2(lastText().startsWith("Got: "),qPrintable(lastText()));QVERIFY(requests.size()>count);QVERIFY(!backend->view->acpSessionId.isEmpty());
    bool delivered=false;for(int i=count;i<requests.size();++i)if(QString::fromUtf8(QJsonDocument::fromVariant(requests[i]).toJson()).contains("Official DSH via native fixture proxy"))delivered=true;QVERIFY(delivered);
  }
  void nativeWidgetPromptInjectionAndCallerScope() {
    configureAPI();backend->newSession();window->setProperty("page","chat");
    auto manager=backend->desktopExtensions;
    const auto oldEntries=manager->entries, oldSettings=manager->settings;
    const auto restore=qScopeGuard([&]{manager->entries=oldEntries;manager->settings=oldSettings;backend->retire(backend->view);backend->newSession();});
    const auto s=backend->view;
    auto widget=manager->entry("file-tree");
    QVERIFY(!widget["agent"].toMap().contains("prompt")); // The shipped file manager has no prompt injection.
    widget["agent"]=QVariantMap{{"prompt","Native Widget prompt fixture."}};
    manager->entries["file-tree"]=widget;
    auto service=manager->entry("global-agents");service["agent"]=QVariantMap{{"prompt","Application Widget instructions."}};
    manager->entries["global-agents"]=service;
    manager->settings["enabled"]=QVariantMap{{"file-tree",true},{"global-agents",true},{"system-prompt",false},{"scheduled-tasks",false}};
    manager->settings["sessions"]=QVariantMap{{"draft:"+s->key,QVariantList{"file-tree"}}};
    QCOMPARE(manager->promptSources(s->key).size(),2);
    QVERIFY(manager->promptSources("missing-token").isEmpty());
    auto background=std::make_shared<Session>();background->key="background-prompt-fixture";background->id="background-prompt-id";
    backend->records[background->key]=background;
    const auto removeBackground=qScopeGuard([&]{backend->records.remove(background->key);});
    QCOMPARE(manager->promptSources(background->key).size(),2); // Built-ins default active in an unconfigured chat.
    auto backgroundChoices=manager->settings["sessions"].toMap();backgroundChoices[background->id]=QVariantList{};manager->settings["sessions"]=backgroundChoices;
    QCOMPARE(manager->promptSources(background->key).size(),1);
    auto enabled=manager->settings["enabled"].toMap();enabled["system-prompt"]=true;manager->settings["enabled"]=enabled;
    auto promptChoices=manager->settings["sessions"].toMap();promptChoices[background->id]=QVariantList{"system-prompt"};manager->settings["sessions"]=promptChoices;
    QVERIFY(manager->systemPromptEnabled(background->key));QVERIFY(!manager->systemPromptEnabled(s->key));
    enabled["system-prompt"]=false;manager->settings["enabled"]=enabled;
    background->globalAgent="coordinator";
    auto choices=manager->settings["sessions"].toMap();choices[background->id]=QVariantList{"file-tree"};manager->settings["sessions"]=choices;
    QCOMPARE(manager->promptSources(background->key).size(),1);
    QVERIFY(!manager->workers.contains("file-tree"));
    backend->send("Check declared Widget prompt");QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),15000);
    const auto prompt=[&]{QString text;for(const auto &value:requests.last().toMap()["messages"].toList())if(value.toMap()["role"]=="system")text+=value.toMap()["content"].toString();return text;};
    QVERIFY(prompt().contains("Native Widget prompt fixture."));QVERIFY(prompt().contains("Application Widget instructions."));
    const auto process=s->process;
    auto manifest=manager->entry("file-tree");manifest["agent"]=QVariantMap{{"prompt","Updated Widget prompt fixture."}};manager->entries["file-tree"]=manifest;
    backend->send("Check updated Widget prompt");QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),15000);
    QCOMPARE(s->process,process);QVERIFY(prompt().contains("Updated Widget prompt fixture."));QVERIFY(!prompt().contains("Native Widget prompt fixture."));
    choices=manager->settings["sessions"].toMap();choices[s->id]=QVariantList{};manager->settings["sessions"]=choices;
    backend->send("Check inactive Widget prompt");QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),15000);
    QCOMPARE(s->process,process);QVERIFY(!prompt().contains("Updated Widget prompt fixture."));QVERIFY(prompt().contains("Application Widget instructions."));
    manager->entries.remove("global-agents");
    backend->send("Check removed application Widget");QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),15000);
    QVERIFY(!prompt().contains("Application Widget instructions."));
    if (qEnvironmentVariableIsSet("UMEKO_WIDGET_PROMPT_REVIEW")) {
      // Review the shipped manifests, without the temporary test contributions above.
      manager->entries=oldEntries;manager->settings=oldSettings;
      backend->retire(backend->view);backend->newSession();
      manager->setEnabled("scheduled-tasks",false);
      QVERIFY(manager->promptSources(backend->view->key).isEmpty());
      backend->setTheme("white");backend->setLanguage("zh");window->resize(1500,980);
      manager->setEnabled("system-prompt",true);manager->setSessionEnabled("system-prompt",true);
      QVERIFY(manager->openPage("system-prompt"));
      QTRY_VERIFY_WITH_TIMEOUT(find("systemPromptWidget") && find("systemPromptWidget")->property("initialized").toBool(),15000);
      QTRY_VERIFY_WITH_TIMEOUT(find("systemPromptWidget")->property("available").toBool() && !backend->busy(),15000);
      auto page=find("systemPromptWidget");
      QQmlExpression treeExpression(qmlContext(page),page,"JSON.stringify(tree)");
      auto tree=QJsonDocument::fromJson(treeExpression.evaluate().toString().toUtf8()).array().toVariantList();
      QVariantMap collapsed;QString selected;
      for (const auto &v:tree) {
        const auto group=v.toMap();
        if (group["labelKey"]!="group:widgets") collapsed[group["id"].toString()]=true;
        else {QVERIFY(group["children"].toList().isEmpty());selected=group["id"].toString();}
      }
      QVERIFY(!selected.isEmpty());page->setProperty("collapsed",collapsed);page->setProperty("selectedId",selected);
      window->setTitle(QString::fromUtf8("Umeko C++ · Widget 提示词验收"));QTest::qWait(500);
      QDir().mkpath(root+"/build/screenshots");QVERIFY(window->grabWindow().save(root+"/build/screenshots/widget-prompt-preview.png"));
      qInfo()<<"WIDGET_PROMPT_REVIEW_READY"<<directory.path();
      QEventLoop loop;connect(window,&QWindow::visibleChanged,&loop,[&loop](bool visible){if(!visible)loop.quit();});loop.exec();
    }
  }
  void nativeSystemPromptSourcesAndHostBoundary() {
    configureAPI();backend->newSession();window->setProperty("page","chat");auto manager=backend->desktopExtensions;
    manager->setEnabled("system-prompt",true);manager->setSessionEnabled("system-prompt",true);
    QVERIFY(manager->openPage("system-prompt"));QTRY_VERIFY_WITH_TIMEOUT(manager->workers.contains("system-prompt") && manager->workers["system-prompt"]->ready,15000);
    QTRY_VERIFY_WITH_TIMEOUT(find("systemPromptWidget") && find("systemPromptWidget")->property("initialized").toBool(),15000);
    QTRY_VERIFY_WITH_TIMEOUT(find("systemPromptWidget")->property("available").toBool() && !backend->busy() && manager->session()["leafId"].toString()==backend->view->searchLeaf,15000);
    // Busy/state notifications for the same context must not start another
    // prompt inspection, which can otherwise race the following Save request.
    QSignalSpy idleReplies(manager,&DesktopPlugins::hostResponse);
    emit manager->sessionChanged();
    QTest::qWait(350);
    QCOMPARE(idleReplies.count(),0);
    backend->send("Refresh the open prompt page after this turn");
    QTRY_VERIFY_WITH_TIMEOUT(!backend->busy() && lastText().contains("Refresh the open prompt page"),15000);
    QTRY_COMPARE_WITH_TIMEOUT(idleReplies.count(),1,15000);
    QVERIFY(idleReplies.last()[2].toMap()["ok"].toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!find("systemPromptWidget")->property("working").toBool() && !backend->busy(),5000);
    emit manager->sessionChanged();
    QTest::qWait(350);
    QCOMPARE(idleReplies.count(),1);
    QSignalSpy replies(manager,&DesktopPlugins::hostResponse);
    auto request=manager->requestHost("systemPrompt.inspect","{}");QVERIFY(!request.isEmpty());
    QVariantMap response;const auto received=[&]{for(const auto &value:replies)if(value[1].toString()==request){response=value[2].toMap();return true;}return false;};
    QTRY_VERIFY_WITH_TIMEOUT(received(),15000);QVERIFY2(response["ok"].toBool(),qPrintable(response["error"].toString()));auto snapshot=response;
    QVERIFY(snapshot["effectiveCharacters"].toInt()>0);QVERIFY(!snapshot["tree"].toList().isEmpty());
    std::function<QString(QVariantList)> findSource=[&](QVariantList rows){for(const auto &value:rows){auto item=value.toMap();if(item["labelKey"]=="umeko:language")return item["id"].toString();auto id=findSource(item["children"].toList());if(!id.isEmpty())return id;}return QString();};
    const auto id=findSource(snapshot["tree"].toList());QVERIFY(!id.isEmpty());QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),5000);
    request=manager->requestHost("systemPrompt.save",QString::fromUtf8(QJsonDocument(QJsonObject{{"id",id},{"text","Native source override fixture"}}).toJson(QJsonDocument::Compact)));QTRY_VERIFY_WITH_TIMEOUT(received(),15000);QVERIFY2(response["ok"].toBool(),qPrintable(response["error"].toString()));QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),5000);
    manager->closePage();backend->send("Check prompt override");QTRY_VERIFY_WITH_TIMEOUT(!backend->busy(),10000);
    bool seen=false;for(const auto &r:requests)for(const auto &m:r.toMap()["messages"].toList())if(m.toMap()["role"]=="system" && m.toMap()["content"].toString().contains("Native source override fixture"))seen=true;QVERIFY(seen);
    QVERIFY(manager->openPage("system-prompt"));QTRY_VERIFY_WITH_TIMEOUT(manager->workers["system-prompt"]->ready && !backend->busy(),15000);
    auto host=qobject_cast<WidgetHost*>(manager->hostServices());QVERIFY(host);
    request=host->requestFor("system-prompt",{{"operation","systemPrompt.save"},{"arguments",QVariantMap{{"id",id},{"text","forged worker"}}},{"session",manager->session()}});QTRY_VERIFY_WITH_TIMEOUT(received(),5000);QVERIFY(!response["ok"].toBool());QVERIFY(response["error"].toString().contains("built-in System Prompt page"));
    QTest::qWait(300);QVERIFY(window->grabWindow().save(root+"/build/screenshots/system-prompt-native.png"));manager->closePage();
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
