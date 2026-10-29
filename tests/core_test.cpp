#include "chatmodel.hpp"
#include "common.hpp"
#include "store.hpp"
#include "document_preview.hpp"
#include "webview_support.hpp"
#include <QtTest>

class CoreTests : public QObject {
  Q_OBJECT
private slots:
  void staticHtmlPreviewBoundary() {
    configureWebView(QDir::currentPath());
    QTemporaryDir temp;
    QDir().mkpath(temp.filePath("public"));
    QFile file(temp.filePath("public/index.html"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("<!doctype html><title>Native "
               "preview</title><script>document.title='Ready'</script>");
    file.close();
    QFile secret(temp.filePath("outside.txt"));
    QVERIFY(secret.open(QIODevice::WriteOnly));
    secret.write("do not serve");
    secret.close();
    Store store(temp.filePath("profile"));
    DocumentPreview preview(nullptr, &store);
    preview.load(file.fileName(), "file");
    QTRY_VERIFY(!preview.state()["busy"].toBool());
    QCOMPARE(preview.state()["fileType"].toString(), QString("html"));
    if (!webviewConfiguration()["available"].toBool()) {
      QCOMPARE(preview.state()["mode"].toString(), QString("source"));
      return;
    }
    const QUrl url(preview.state()["previewUrl"].toString());
    QVERIFY(url.isValid());
    QNetworkAccessManager network;
    auto get = [&](QUrl target) {
      auto reply = network.get(QNetworkRequest(target));
      QEventLoop loop;
      QObject::connect(reply, &QNetworkReply::finished, &loop,
                       &QEventLoop::quit);
      QTimer::singleShot(3000, &loop, &QEventLoop::quit);
      loop.exec();
      auto result = QPair{
          reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(),
          reply->readAll()};
      reply->deleteLater();
      return result;
    };
    auto response = get(url);
    QCOMPARE(response.first, 200);
    QVERIFY(response.second.contains("Native preview"));
    QCOMPARE(get(url.resolved(QUrl("../outside.txt"))).first, 404);
    auto hidden = url;
    hidden.setPath(url.path().section('/', 0, -2) + "/.env");
    QCOMPARE(get(hidden).first, 404);
    preview.cancel();
    QCOMPARE(get(url).first, 404);
  }
  void childProjectionAndDurableResume() {
    QVariantList tools;
    for (int i = 0; i < 7; ++i)
      tools << QVariantMap{{"id", "child-" + QString::number(i)},
                           {"name", "subagent"},
                           {"subagent", QVariantMap{{"id", QString::number(i)},
                                                    {"status", "running"}}}};
    auto projected = ChatModel::project({QVariantMap{
        {"role", "assistant"}, {"text", "Delegated"}, {"tools", tools}}});
    QCOMPARE(projected.size(), 1);
    const auto activity = projected.first().toMap()["activity"].toList();
    QCOMPARE(activity.last().toMap()["kind"].toString(), QString("subagents"));
    QCOMPARE(activity.last().toMap()["agents"].toList().size(), 7);
    QTemporaryDir temp;
    QFile file(temp.filePath("parent.jsonl"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    auto append = [&](QVariantMap row) {
      file.write(
          QJsonDocument::fromVariant(row).toJson(QJsonDocument::Compact) +
          '\n');
    };
    append({{"type", "session"}, {"cwd", temp.path()}});
    const QVariantMap call{{"type", "toolCall"},
                           {"id", "launch"},
                           {"name", "subagent"},
                           {"arguments", QVariantMap{}}};
    const QVariantMap message{{"role", "assistant"},
                              {"content", QVariantList{call}}};
    append({{"type", "message"}, {"id", "a"}, {"message", message}});
    auto child = QVariantMap{
        {"id", "child:one"},
        {"name", "subagent"},
        {"subagent", QVariantMap{{"id", "one"}, {"status", "running"}}}};
    append({{"type", "custom"},
            {"id", "b"},
            {"parentId", "a"},
            {"customType", "umeko-subagent"},
            {"data", QVariantMap{{"version", 1},
                                 {"tool", child},
                                 {"toolCallId", "launch"},
                                 {"ownerEntryId", "a"}}}});
    append({{"type", "message"},
            {"id", "c"},
            {"parentId", "b"},
            {"message", QVariantMap{{"role", "assistant"},
                                    {"content", "Resuming the same child"}}}});
    child["subagent"] = QVariantMap{{"id", "one"}, {"status", "completed"}};
    append({{"type", "custom"},
            {"id", "d"},
            {"parentId", "c"},
            {"customType", "umeko-subagent"},
            {"data", QVariantMap{{"version", 1},
                                 {"tool", child},
                                 {"toolCallId", "resume"},
                                 {"ownerEntryId", "c"}}}});
    file.close();
    auto rows = U::preview(file.fileName()).first;
    QCOMPARE(rows.size(), 2);
    auto original = rows.first().toMap()["tools"].toList();
    QCOMPARE(original.size(), 1);
    QCOMPARE(original.first().toMap()["subagent"].toMap()["status"].toString(),
             QString("completed"));
    QVERIFY(rows.last().toMap()["tools"].toList().isEmpty());
  }
  void documentPresentationModes() {
    QTemporaryDir temp;
    Store settings(temp.path());
    QFile file(temp.filePath("report.md"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("# Report\n\n**Complete**");
    file.close();
    DocumentPreview preview(nullptr, &settings);
    preview.load(file.fileName(), "file");
    QTRY_VERIFY(!preview.state()["busy"].toBool());
    QCOMPARE(preview.state()["fileType"].toString(), QString("markdown"));
    QCOMPARE(preview.state()["mode"].toString(), QString("rendered"));
    QVERIFY(!preview.state()["html"].toString().isEmpty());
    preview.setMode("markdown", "source");
    QCOMPARE(preview.state()["mode"].toString(), QString("source"));
    preview.cancel();
    preview.load(file.fileName(), "file");
    QTRY_VERIFY(!preview.state()["busy"].toBool());
    QCOMPARE(preview.state()["mode"].toString(), QString("source"));
    QCOMPARE(settings.data["ui"]
                 .toMap()["documentPreviewModes"]
                 .toMap()["markdown"]
                 .toString(),
             QString("source"));
  }
  void profileMigration() {
    QTemporaryDir temp;
    const auto old = temp.path() + "/old",
               target = temp.path() + "/home/.umeko/desktop-cpp";
    QDir().mkpath(old + "/provider-plugins/packages/example/1.0.0");
    QDir().mkpath(old + "/pi/sessions");
    QVERIFY(U::writeJson(old + "/settings.json",
                         QVariantMap{{"workspace", old + "/workspace"},
                                     {"activeProject", "old-id"},
                                     {"projects", QVariantList{QVariantMap{{"id", "old-id"}, {"path", old + "/workspace"}, {"name", "My project"}, {"pinned", true}}}}}));
    QVERIFY(U::writeJson(
        old + "/sessions.json",
        QVariantList{QVariantMap{{"file", old + "/pi/sessions/one.jsonl"}}}));
    QFile log(old + "/pi/sessions/one.jsonl");
    QVERIFY(log.open(QIODevice::WriteOnly));
    const auto header = QJsonDocument(QJsonObject{{"type", "session"},
                                                  {"cwd", old + "/workspace"}})
                            .toJson(QJsonDocument::Compact);
    const QByteArray body =
        "{\"type\":\"message\",\"text\":\"preserve user content\"}\n";
    log.write(header + '\n' + body);
    log.close();
    QVERIFY(Store::migrateProfile(old, target));
    const auto migrated = U::readJson(target + "/settings.json").toMap();
    auto projectKey = U::cleanPath(target + "/workspace");
#ifdef Q_OS_WIN
    projectKey = projectKey.toLower();
#endif
    const auto projectId = U::sha(projectKey.toUtf8()).left(16);
    QCOMPARE(migrated["activeProject"].toString(), projectId);
    const auto project = migrated["projects"].toList().first().toMap();
    QCOMPARE(project["id"].toString(), projectId);
    QCOMPARE(project["path"].toString(), target + "/workspace");
    QCOMPARE(project["name"].toString(), QString("My project"));
    QVERIFY(project["pinned"].toBool());
    QCOMPARE(U::readJson(target + "/settings.json")
                 .toMap()["credentialNamespace"]
                 .toString(),
             U::sha(U::cleanPath(old).toUtf8()).left(24));
    QCOMPARE(U::readJson(target + "/sessions.json")
                 .toList()
                 .first()
                 .toMap()["file"]
                 .toString(),
             target + "/pi/sessions/one.jsonl");
    QFile moved(target + "/pi/sessions/one.jsonl");
    QVERIFY(moved.open(QIODevice::ReadOnly));
    QCOMPARE(
        QJsonDocument::fromJson(moved.readLine()).object()["cwd"].toString(),
        target + "/workspace");
    QCOMPARE(moved.readAll(), body);
    QVERIFY(QFileInfo::exists(old + "/settings.json"));
    QVERIFY(!Store::migrateProfile(old, target));
  }
  void repairsMovedAndDuplicatedProjectsOnStartup() {
    QTemporaryDir temp;
    const auto path = temp.path() + "/workspace";
    QDir().mkpath(path);
    const QVariantList projects{
        QVariantMap{{"id", "old-path-id"}, {"path", path}, {"name", "My project"}},
        QVariantMap{{"id", "new-path-id"}, {"path", path + "/."}, {"name", "workspace"}, {"pinned", true}}};
    QVERIFY(U::writeJson(temp.filePath("settings.json"),
                         QVariantMap{{"projects", projects}, {"activeProject", "new-path-id"}, {"credentialNamespace", "preserve-credentials"}}));
    const QByteArray history = "{\"id\":\"same-session\",\"content\":\"untouched\"}\n";
    QFile log(temp.filePath("history.jsonl"));
    QVERIFY(log.open(QIODevice::WriteOnly)); log.write(history); log.close();
    Store repaired(temp.path());
    const auto rows = repaired.data["projects"].toList();
    QCOMPARE(rows.size(), 1);
    const auto project = rows.first().toMap();
    QCOMPARE(project["name"].toString(), QString("My project"));
    QVERIFY(project["pinned"].toBool());
    QCOMPARE(repaired.data["activeProject"], project["id"]);
    QCOMPARE(repaired.data["credentialNamespace"].toString(), QString("preserve-credentials"));
    QCOMPARE(Store(temp.path()).data["projects"].toList(), rows);
    QVERIFY(log.open(QIODevice::ReadOnly)); QCOMPARE(log.readAll(), history);
  }
  void outputDefaultMigration() {
    QTemporaryDir temp;
    Store store(temp.path());
    QCOMPARE(store.data["agent"].toMap()["maxTokens"].toInt(), 32768);
    for (int limit : {8192, 4096}) {
      auto legacy = store.data;
      auto agent = legacy["agent"].toMap();
      agent.remove("outputLimitDefaultVersion");
      agent["maxTokens"] = limit;
      legacy["agent"] = agent;
      QVERIFY(U::writeJson(temp.path() + "/settings.json", legacy));
      Store migrated(temp.path());
      QCOMPARE(migrated.data["agent"].toMap()["maxTokens"].toInt(),
               limit == 8192 ? 32768 : limit);
    }
    auto agent = store.data["agent"].toMap();
    agent["maxTokens"] = 8192;
    store.data["agent"] = agent;
    QVERIFY(store.save());
    QCOMPARE(Store(temp.path()).data["agent"].toMap()["maxTokens"].toInt(),
             8192);
  }
  void webDeliveryCards() {
    const auto rows = U::mediaView(QVariantList{
      QVariantMap{{"kind", "web"}, {"path", "http://127.0.0.1:8791/review.html"}, {"title", "Preview"}},
      QVariantMap{{"kind", "web"}, {"path", "javascript:alert(1)"}},
      QVariantMap{{"kind", "web"}, {"path", "https://user:password@example.com/"}},
      QVariantMap{{"kind", "web"}, {"path", "https://example.com/page"}}});
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.first().toMap()["url"].toString(), QString("http://127.0.0.1:8791/review.html"));
    ChatModel model;
    model.update(QVariantList{QVariantMap{{"role", "assistant"}, {"text", ""}, {"tools", QVariantList{QVariantMap{{"id", "web"}, {"name", "submit_to_chatbox"}, {"media", rows}}}}}});
    const auto activity = model.rows.first().toMap()["activity"].toList();
    QCOMPARE(activity.last().toMap()["kind"].toString(), QString("media"));
    QCOMPARE(activity.last().toMap()["items"].toList().size(), 2);
  }
  void boundedDocumentPreviews() {
    QTemporaryDir temp;
    auto write = [&](QString name, QByteArray text) { QFile file(temp.filePath(name)); if (!file.open(QIODevice::WriteOnly)) return QString(); file.write(text); return file.fileName(); };
    auto code = DocumentPreview::read(write("example.py", "def hello():\n    return 42\n"), "file");
    QCOMPARE(code["kind"].toString(), QString("code"));
    QVERIFY(code["highlight"].toBool()); QVERIFY(!code["truncated"].toBool());
    auto markdown = DocumentPreview::read(write("readme.md", "# Hello\n\n**world**\n\n![image](https://example.invalid/slow.png)"), "file");
    QVERIFY(markdown["html"].toString().contains("Hello"));
    QVERIFY(!markdown["html"].toString().contains("<img"));
    auto complex = DocumentPreview::read(write("complex.md", QByteArray(15000, '|')), "file");
    QVERIFY(complex["simple"].toBool()); QVERIFY(complex["html"].toString().isEmpty());
    const auto big = write("huge.py", QByteArray(270000, 'x'));
    QFile huge(big); QVERIFY(huge.open(QIODevice::ReadWrite)); QVERIFY(huge.resize(512ll * 1024 * 1024)); huge.close();
    auto limited = DocumentPreview::read(big, "file");
    QVERIFY(limited["truncated"].toBool()); QVERIFY(limited["text"].toString().size() <= 48002);
    QVERIFY(!limited["highlight"].toBool());
    auto lines = DocumentPreview::read(write("lines.txt", QByteArray("line\n").repeated(2000)), "file");
    QVERIFY(lines["truncated"].toBool()); QVERIFY(lines["text"].toString().count('\n') < 1000);
    auto binary = DocumentPreview::read(write("binary.txt", QByteArray("a\0b", 3)), "text");
    QVERIFY(!binary["error"].toString().isEmpty());
    QVERIFY(!DocumentPreview::read(temp.filePath("missing.md"), "markdown")["error"].toString().isEmpty());
  }
  void webAttachmentsRoundTrip() {
    const QString url = "https://example.com/page?q=1#section";
    const auto prompt = U::attachmentPrompt("Compare this", QVariantList{
      QVariantMap{{"kind", "web"}, {"path", url}},
      QVariantMap{{"kind", "file"}, {"path", "/tmp/report.txt"}}});
    const auto parsed = U::splitAttachments(prompt);
    QCOMPARE(parsed.first, QString("Compare this"));
    QCOMPARE(parsed.second.size(), 2);
    const auto web = parsed.second.first().toMap();
    QCOMPARE(web["url"].toString(), url);
    QCOMPARE(web["name"].toString(), QString("example.com"));
    const auto malformed = U::attachmentPrompt("Keep visible", QVariantList{
      QVariantMap{{"kind", "web"}, {"path", "javascript:alert(1)"}}});
    QCOMPARE(U::splitAttachments(malformed).first, malformed);
  }
  void context() {
    QCOMPARE(U::contextSuggestion("kimi-k3")["tokens"].toInt(), 1048576);
    QCOMPARE(U::contextSuggestion("kimi/k3-256")["tokens"].toInt(), 262144);
    QCOMPARE(U::contextSuggestion("gateway-model-128k")["tokens"].toInt(),
             131072);
    QCOMPARE(U::parseContext("1.5M"), 1572864);
    QCOMPARE(U::parseContext("256K"), 262144);
    QCOMPARE(U::parseContext(true), 0);
    QCOMPARE(U::parseContext("20"), 0);
    QCOMPARE(U::parseContext("999999M"), 0);
  }
  void vision() {
    QVERIFY(U::visionSuggestion("gpt-4o")["vision"].toBool());
    QVERIFY(U::visionSuggestion("kimi-k3-256")["vision"].toBool());
    QVERIFY(!U::visionSuggestion("whisper-audio")["vision"].toBool());
    auto m = U::normalizeModel({{"upstream", "gpt-4o"},
                                {"vision", false},
                                {"visionAuto", false},
                                {"contextWindow", "256K"},
                                {"contextWindowAuto", false}});
    QVERIFY(!m["vision"].toBool());
    QCOMPARE(m["contextWindow"].toInt(), 262144);
  }
  void grouping() {
    QVariantList messages{
        QVariantMap{{"role", "user"}, {"text", "Inspect"}},
        QVariantMap{{"role", "assistant"},
                    {"text", "Checking"},
                    {"tools", QVariantList{QVariantMap{{"id", "one"}}}}},
        QVariantMap{{"role", "assistant"},
                    {"tools", QVariantList{QVariantMap{{"id", "two"}}}}},
        QVariantMap{{"role", "assistant"},
                    {"thinking", "Compare"},
                    {"text", "Complete"}}};
    ChatModel m;
    m.update(messages);
    QCOMPARE(m.rowCount(), 2);
    auto activity = m.rows[1].toMap()["activity"].toList();
    QCOMPARE(activity[1].toMap()["tools"].toList().size(), 2);
    QSignalSpy resets(&m, &QAbstractItemModel::modelReset);
    auto last = messages.last().toMap();
    last["text"] = "Streamed update";
    messages.last() = last;
    m.update(messages);
    QCOMPARE(resets.count(), 0);
    QCOMPARE(m.rows.last().toMap()["text"].toString(),
             QString("Streamed update"));
  }
  void lengthContinuations() {
    const QVariantList messages{
        QVariantMap{{"role", "assistant"},
                    {"text", "Inspect"},
                    {"tools", QVariantList{QVariantMap{{"id", "one"}}}}},
        QVariantMap{{"role", "assistant"},
                    {"thinking", "plan"},
                    {"text", "```rust\nfn ma"},
                    {"stopReason", "length"}},
        QVariantMap{{"role", "assistant"},
                    {"thinking", "check"},
                    {"text", "in() {}\n```"},
                    {"stopReason", "stop"}}};
    ChatModel model;
    model.update(messages);
    QCOMPARE(model.rows.size(), 1);
    const auto row = model.rows.last().toMap();
    QCOMPARE(row["text"].toString(), QString("```rust\nfn main() {}\n```"));
    const auto activity = row["activity"].toList();
    QCOMPARE(activity.size(), 4);
    QCOMPARE(activity[1].toMap()["kind"].toString(), QString("tools"));
    QCOMPARE(activity[2].toMap()["text"].toString(), QString("plancheck"));
    QCOMPARE(activity[3].toMap()["text"], row["text"]);
  }
  void questionNavigationIndex() {
    ChatModel model;
    QSignalSpy changed(&model, &ChatModel::questionsChanged);
    QVariantList messages{
        QVariantMap{{"role", "user"}, {"text", "First\nquestion"}},
        QVariantMap{{"role", "assistant"},
                    {"text", "inspect"},
                    {"tools", QVariantList{QVariantMap{{"id", "read"}}}}},
        QVariantMap{{"role", "assistant"}, {"text", "answer"}},
        QVariantMap{
            {"role", "user"},
            {"text", ""},
            {"attachments", QVariantList{QVariantMap{{"name", "report.pdf"}}}}},
        QVariantMap{{"role", "assistant"}, {"text", ""}}};
    model.update(messages);
    QCOMPARE(model.questions().size(), 2);
    QCOMPARE(model.questions()[0].toMap()["title"].toString(),
             QString("First question"));
    QCOMPARE(model.questions()[1].toMap()["row"].toInt(), 2);
    QCOMPARE(model.questions()[1].toMap()["title"].toString(),
             QString("report.pdf"));
    for (int i = 0; i < 50; ++i) {
      messages.last() = QVariantMap{{"role", "assistant"},
                                    {"text", QString("Stream %1").arg(i)}};
      model.update(messages);
    }
    QCOMPARE(changed.size(), 1);
    model.update({});
    QVERIFY(model.questions().isEmpty());
  }
  void storage() {
    QTemporaryDir temporary;
    Store store(temporary.path());
    auto ui = store.data["ui"].toMap();
    ui["sidebarWidth"] = 320;
    ui["sidebarCollapsed"] = true;
    store.data["ui"] = ui;
    QVERIFY(store.save());
    Store other(temporary.path());
    QCOMPARE(other.data["ui"].toMap()["sidebarWidth"].toInt(), 320);
    QVERIFY(other.data["providers"].toList().isEmpty());
    QVERIFY(other.data["server"].toMap()["keys"].toList().isEmpty());
  }
  void attachment() {
    QVariantList items{QVariantMap{{"name", "example.txt"},
                                   {"path", "/workspace/example.txt"},
                                   {"kind", "file"}}};
    auto prompt = U::attachmentPrompt("Review", items);
    auto split = U::splitAttachments(prompt);
    QCOMPARE(split.first, QString("Review"));
    QCOMPARE(split.second[0].toMap()["path"].toString(),
             QString("/workspace/example.txt"));
  }
  void nativeHistory() {
    QTemporaryDir temporary;
    auto file = temporary.filePath("session.jsonl");
    QFile output(file);
    QVERIFY(output.open(QIODevice::WriteOnly));
    output.write(
        "{\"type\":\"session\",\"cwd\":\"/"
        "workspace\"}\n{\"type\":\"message\",\"id\":\"u\",\"parentId\":null,"
        "\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\","
        "\"text\":\"Hello\"}]}}\n{\"type\":\"message\",\"id\":\"a\","
        "\"parentId\":\"u\",\"message\":{\"role\":\"assistant\",\"content\":[{"
        "\"type\":\"thinking\",\"thinking\":\"Compare\"},{\"type\":\"text\","
        "\"text\":\"Done\"}]}}\n");
    output.close();
    auto preview = U::preview(file);
    QCOMPARE(preview.first.size(), 2);
    QCOMPARE(preview.first[1].toMap()["thinking"].toString(),
             QString("Compare"));
  }
};
QTEST_MAIN(CoreTests)
#include "core_test.moc"
