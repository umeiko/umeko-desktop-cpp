#include "chatmodel.hpp"
#include "common.hpp"
#include "store.hpp"
#include <QtTest>

class CoreTests : public QObject {
  Q_OBJECT
private slots:
  void profileMigration() {
    QTemporaryDir temp;
    const auto old = temp.path() + "/old",
               target = temp.path() + "/home/.umeko/desktop-cpp";
    QDir().mkpath(old + "/provider-plugins/packages/example/1.0.0");
    QDir().mkpath(old + "/pi/sessions");
    QVERIFY(U::writeJson(old + "/settings.json",
                         QVariantMap{{"workspace", old + "/workspace"}}));
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
