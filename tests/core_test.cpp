#include "chatmodel.hpp"
#include "common.hpp"
#include "store.hpp"
#include <QtTest>

class CoreTests : public QObject {
  Q_OBJECT
private slots:
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
