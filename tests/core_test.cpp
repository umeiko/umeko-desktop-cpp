#include "chatmodel.hpp"
#include "pet_reminders.hpp"
#include "pi_capabilities.hpp"
#include "common.hpp"
#include "request_headers.hpp"
#include "store.hpp"
#include "document_preview.hpp"
#include "webview_support.hpp"
#include <QtTest>

class CoreTests : public QObject {
  Q_OBJECT
private slots:
  void customRequestHeadersValidation() {
    using namespace RequestHeaders;
    QCOMPARE(validate(QVariantList{}),defaults());
    for (const auto &patch:QVariantList{QVariantMap{{"name","Host"}},QVariantMap{{"name","Bad Header"}},QVariantMap{{"value","line\nnext"}},QVariantMap{{"value",QString::fromUtf8("中文")}},QVariantMap{{"enabled",1}}}) {
      auto rows=defaults(); auto row=rows.first().toMap();
      const auto changes=patch.toMap();for (auto it=changes.cbegin();it!=changes.cend();++it)row[it.key()]=it.value(); rows[0]=row;
      QVERIFY_EXCEPTION_THROWN(validate(rows),std::runtime_error);
    }
    auto rows=defaults();rows<<QVariantMap{{"id","custom-1"},{"name","USER-AGENT"},{"value","fixture"},{"enabled",true}};
    QVERIFY_EXCEPTION_THROWN(validate(rows),std::runtime_error);
    auto custom=rows.last().toMap();custom["enabled"]=false;rows.last()=custom;QCOMPARE(validate(rows),rows);
    rows<<rows.first();QVERIFY_EXCEPTION_THROWN(validate(rows),std::runtime_error);
  }
  void sharedViewportGroupsAndCompactionWindow() {
    ChatModel model;QVariantList rows;
    for(int i=0;i<600;++i)rows<<QVariantMap{{"role",i%2?"assistant":"user"},{"text",QString("row %1").arg(i)}};
    model.update(rows);QTRY_COMPARE(model.rowCount(),600);model.setGroupHeight(800);
    QVERIFY(model.groupCount()>1);QCOMPARE(model.groupFirst(0),0);QCOMPARE(model.groupLast(model.groupCount()-1),599);
    const auto first=model.groupFirst(model.groupOf(230)),last=model.groupLast(model.groupOf(230));
    model.requestGroups(230,233,1);QTRY_VERIFY(model.rangeReady(first,last));
    int prepared=0;for(int i=0;i<600;++i)prepared+=!model.data(model.index(i),ChatModel::MessageRole).toMap()["_loading"].toBool();QVERIFY(prepared<=256);
    model.recordHeight(230,450);QCOMPARE(model.estimatedHeight(230),450.0);model.setLayout("narrow");QVERIFY(model.estimatedHeight(230)!=450);
    auto receipt=[](QString id,QString state){return QVariantMap{{"role","assistant"},{"customType","umeko-compaction"},{"compaction",QVariantMap{{"eventId",id},{"state",state}}}};};
    QVariantList history{rows[0],receipt("one","success"),rows[1],receipt("two","success"),rows[2],receipt("failed","failure"),rows[3]};
    auto source=history;auto window=ChatModel::recentCompactionInterval(history);QCOMPARE(window.size(),6);QCOMPARE(window.first(),history[1]);QCOMPARE(history,source);
    history<<receipt("three","running")<<receipt("three","success")<<rows[4];
    window=ChatModel::recentCompactionInterval(history);QCOMPARE(window.first(),history[3]);QCOMPARE(window.last(),rows[4]);QVERIFY(window.contains(history[5]));
  }

  void selectedPassageFiles() {
    QTemporaryDir project;
    QVERIFY(project.isValid());
    const QString text = "中文𠮷 selected words\n\n```python\nprint(\"preserved\")\n```\n";
    const auto first = U::writeSelectionAttachment(project.path(), text);
    const auto second = U::writeSelectionAttachment(project.path(), text + "second");
    QVERIFY(first["path"] != second["path"]);
    QCOMPARE(first["name"].toString(), QString("中文𠮷 selected words `.md"));
    QCOMPARE(second["name"].toString(), QString("中文𠮷 selected words ` (2).md"));
    QFile file(first["path"].toString()); QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(file.readAll()), text);
    QCOMPARE(QFileInfo(file).absolutePath(), project.path() + "/.umeko");
    for (const auto &value : QStringList{"../../danger:<name>?*|\"", "CON", "nul.txt", "..."}) {
      const auto item = U::writeSelectionAttachment(project.path(), value);
      QCOMPARE(QFileInfo(item["path"].toString()).absolutePath(), project.path() + "/.umeko");
      QVERIFY(!item["name"].toString().contains(QRegularExpression("[<>:\"/\\\\|?*]")));
    }
    QVERIFY(U::writeSelectionAttachment(project.path(), " \n\t").isEmpty());
    QVERIFY_EXCEPTION_THROWN(U::writeSelectionAttachment(project.filePath("missing"), "text"), std::runtime_error);
    QFile script(QDir::currentPath() + "/app/qml/markdown.js"); QVERIFY(script.open(QIODevice::ReadOnly));
    const auto source = QString::fromUtf8(script.readAll());
    const QVariantList rows{QVariantMap{{"role","assistant"},{"text","中文𠮷 selected words\n\n```python\nprint(\"preserved\")\n```\n\nLast paragraph."}}};
    const QVariantMap start{{"row",0},{"block",0},{"offset",0}}, end{{"row",0},{"block",2},{"offset",5}};
    QCOMPARE(U::selectedChatText(source, rows, {{"anchor",end},{"cursor",start}}),
             QString("中文𠮷 selected words\n\nprint(\"preserved\")\n\nLast "));
    QCOMPARE(U::selectedChatText(source, rows, {{"anchor",QVariantMap{{"row",0},{"block",0},{"offset",2}}},{"cursor",QVariantMap{{"row",0},{"block",0},{"offset",4}}}}), QString("𠮷"));
  }

  void modelReasoningLevels() {
    const auto unknown = U::normalizeModel({});
    QVERIFY(U::reasoningLevels(unknown).isEmpty()); // Unknown automatic capability must not invent levels.
    QCOMPARE(U::reasoningLevels(U::normalizeModel({{"reasoningLevelsAuto", false}})), (QStringList{"low", "high", "max"}));
    QCOMPARE(U::reasoningLevels(U::normalizeModel({{"reasoningDetection", QVariantMap{{"state", "known"}, {"levels", QVariantList{"low", "high"}}}}})), (QStringList{"low", "high"}));
    const auto custom = U::normalizeModel({{"reasoningLevels", QVariantList{" medium ", "xhigh", "vendor-custom"}}});
    QCOMPARE(U::reasoningLevels(custom), (QStringList{"medium", "xhigh", "vendor-custom"}));
    QCOMPARE(U::reasoningLevels(U::normalizeModel({{"alias", "renamed"}}, custom)), U::reasoningLevels(custom));
    QVERIFY(U::reasoningLevels(U::normalizeModel({{"reasoningLevels", QVariantList{}}}, custom)).isEmpty());
    for (const auto &bad : QVariantList{QVariant(), "low", QVariantList{false}, QVariantList{""}, QVariantList{"OFF"}, QVariantList{"low", "LOW"}, QVariantList{"two words"}, QVariantList{QString(65, 'a')}}) {
      QVERIFY_EXCEPTION_THROWN(U::normalizeModel({{"reasoningLevels", bad}}), std::runtime_error);
    }
    QCOMPARE(U::reconcileEffort("high", {"low", "medium"}), QString("medium"));
    QCOMPARE(U::reconcileEffort("xhigh", {"high", "max"}), QString("high"));
    QCOMPARE(U::reconcileEffort("vendor-custom", {"vendor-custom"}), QString("vendor-custom"));
    QCOMPARE(U::reconcileEffort("vendor-custom", {"medium"}), QString("off"));
    QCOMPARE(U::reconcileEffort("max", {}), QString("off"));
  }
  void longRuntimePaths() {
    QTemporaryDir temp;QString path=temp.path();
    while(path.size()<340)path+="/native-runtime-with-long-project-name";
    QVERIFY(QDir().mkpath(path));QVERIFY(U::writeJson(path+"/settings.json",QVariantMap{{"fixture",true}}));
    QCOMPARE(U::readJson(path+"/settings.json").toMap()["fixture"].toBool(),true);
    QString error;QVERIFY2(U::copyTree(path,temp.filePath("copied"),&error),qPrintable(error));
    QCOMPARE(U::readJson(temp.filePath("copied/settings.json")).toMap()["fixture"].toBool(),true);
    QProcess node;node.setWorkingDirectory(U::processDirectory(path));U::hideConsole(&node);
    node.start(U::node(),{"-e","const fs=require('node:fs');process.stdout.write(fs.readFileSync('settings.json','utf8'));"});
    QVERIFY2(node.waitForStarted(15000),qPrintable(node.errorString()));QVERIFY(node.waitForFinished(30000));QCOMPARE(node.exitCode(),0);
    QCOMPARE(QJsonDocument::fromJson(node.readAllStandardOutput()).object()["fixture"].toBool(),true);
  }
  void compactionHistoryAndContextMetadata() {
    QVariantMap data{{"version",2},{"eventId",U::uuid()},{"state","running"},{"mode","manual"},{"before",12345},{"after",QVariant()},{"afterEstimated",true},{"summary",""},{"error",""}};
    QVariantMap entry{{"type","custom"},{"customType","umeko-compaction"},{"id","first"},{"timestamp","2026-11-02T01:00:00Z"},{"data",data}};
    const auto row=U::compactionView(entry);QVERIFY(!row.isEmpty());
    QCOMPARE(U::compactionStatus(row)["compaction"].toMap()["state"].toString(),QString("failure"));
    QCOMPARE(U::compactionStatus(row,data["eventId"].toString())["compaction"].toMap()["state"].toString(),QString("running"));
    QTemporaryDir temp;QFile file(temp.filePath("history.jsonl"));QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument::fromVariant(entry).toJson(QJsonDocument::Compact)+'\n');
    QVariantMap message{{"type","message"},{"id","message"},{"parentId","first"},{"message",QVariantMap{{"role","assistant"},{"content","Work after compaction"}}}};
    file.write(QJsonDocument::fromVariant(message).toJson(QJsonDocument::Compact)+'\n');
    data["state"]="success";data["after"]=2000;data["summary"]="Summary";entry["data"]=data;entry["id"]="last";entry["parentId"]="message";
    file.write(QJsonDocument::fromVariant(entry).toJson(QJsonDocument::Compact)+'\n');file.close();
    auto rows=U::preview(file.fileName()).first;QCOMPARE(rows.size(),2);QCOMPARE(rows[0].toMap()["entryId"].toString(),QString("first"));QCOMPARE(rows[0].toMap()["compaction"].toMap()["state"].toString(),QString("success"));QCOMPARE(ChatModel::project(rows).size(),2);
    for(const auto &invalid:QVariantList{true,-1,"42"}){auto bad=data;bad["before"]=invalid;entry["data"]=bad;QVERIFY(U::compactionView(entry).isEmpty());}
    QCOMPARE(U::providerContext({{"context_length",500000},{"limits",QVariantMap{{"max_input_tokens",262144},{"max_tokens",7}}}}),qint64(262144));
    QVariantMap model{{"upstream","unknown"},{"providerContextWindow",1048576},{"contextWindowAuto",true}};
    QCOMPARE(U::normalizeModel(model)["contextWindow"].toLongLong(),qint64(1048576));
    auto previous=U::normalizeModel(model);model["contextWindowAuto"]=false;model["contextWindow"]="256K";
    QCOMPARE(U::normalizeModel(model,previous)["contextWindow"].toLongLong(),qint64(262144));
    model["providerContextWindow"]=42;model["contextWindowAuto"]=true;
    QCOMPARE(U::normalizeModel(model,previous)["contextWindow"].toLongLong(),qint64(1048576));
  }
  void nativePiCapabilityPolicies() {
    using namespace PiCapabilities;
    auto migrated=normalize({},QStringList{"write"});QCOMPARE(migrated["preset"].toString(),QString("custom"));
    QVERIFY(!toolEnabled(migrated,{{"name","write"},{"owner","native"}}));
    for(const auto &preset:QStringList{"minimal","standard","full"}) {
      const QVariantMap profile{{"preset",preset}};int count=0;
      for(const auto &name:core)count+=toolEnabled(profile,{{"name",name},{"owner","native"}});
      QCOMPARE(count,preset=="minimal"?4:8);QCOMPARE(pluginEnabled(profile,"builtin:umeko-control"),preset=="full");
      QCOMPARE(pluginEnabled(profile,"/optional",true),preset=="full");
    }
    const QVariantList rows{QVariantMap{{"id","optional"},{"path","/optional"}},QVariantMap{{"id","desktop"},{"path","builtin:chatbox"}}};
    const QVariantMap resources{{"toolCatalog",QVariantList{QVariantMap{{"name","one"},{"owner","/optional"}},QVariantMap{{"name","two"},{"owner","/optional"}},QVariantMap{{"name","submit_to_chatbox"},{"owner","builtin:chatbox"}}}}};
    auto profile=change({},"pi.tool",{{"name","one"},{"enabled",false}},rows,resources);
    QVERIFY(!toolEnabled(profile,{{"name","one"},{"owner","/optional"}}));QVERIFY(toolEnabled(profile,{{"name","two"},{"owner","/optional"}}));
    const auto custom=profile;profile=change(profile,"pi.preset",{{"preset","full"}},rows,resources);
    QVERIFY(toolEnabled(profile,{{"name","one"},{"owner","/optional"}}));
    QCOMPARE(change(profile,"pi.preset",{{"preset","custom"}},rows,resources),custom);
    profile=change(custom,"pi.plugin",{{"id","optional"},{"enabled",false}},rows,resources);
    QVERIFY(!toolEnabled(profile,{{"name","two"},{"owner","/optional"}}));
    QCOMPARE(catalog(mergeResources(resources,{{"toolCatalog",QVariantList{}}},profile)).size(),10);
    profile=change(profile,"pi.plugin",{{"id","desktop"},{"enabled",false}},rows,resources);
    QVERIFY(!toolEnabled(profile,{{"name","submit_to_chatbox"},{"owner","builtin:chatbox"}}));
    profile=change(profile,"pi.plugin",{{"id","desktop"},{"enabled",true}},rows,resources);
    profile=change(profile,"pi.tool",{{"name","submit_to_chatbox"},{"enabled",false}},rows,resources);
    QVERIFY(pluginEnabled(profile,"builtin:chatbox"));QVERIFY(!toolEnabled(profile,{{"name","submit_to_chatbox"},{"owner","builtin:chatbox"}}));
    QVERIFY_EXCEPTION_THROWN(change(profile,"pi.tool",{{"name","*"},{"enabled",false}},rows,resources),std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(change(profile,"pi.tool",{{"name","read"},{"enabled",1}},rows,resources),std::runtime_error);
  }
  void nativePetReminderPersistenceAndOccurrences() {
    QTemporaryDir temp;auto now=QDateTime::fromString("2026-11-02T08:00:00Z",Qt::ISODate);bool visible=true,enabled=true;
    const auto folder=temp.filePath("reminders");PetReminders service(folder,nullptr,[&]{return now;});
    service.visible=[&]{return visible;};service.notifications=[&]{return enabled;};QSignalSpy due(&service,&PetReminders::due);service.start();
    auto result=service.handle("createItem",{{"title","  离线事项  "},{"notes","中文备注"},{"dueAt","2026-11-02T16:00:01+08:00"}});
    auto item=result["item"].toMap();const auto id=item["id"].toString();QCOMPARE(item["title"].toString(),QString("离线事项"));
    QCOMPARE(item["dueAt"].toString(),QString("2026-11-02T08:00:01Z"));QCOMPARE(due.size(),0);
    now=now.addSecs(2);service.check();QCOMPARE(due.size(),1);
    item=service.handle("getItem",{{"id",id}})["item"].toMap();QCOMPARE(item["notificationOutcome"].toString(),QString("presentation_requested"));
    service.check();QCOMPARE(due.size(),1);service.stop();
    PetReminders restored(folder,nullptr,[&]{return now;});restored.visible=[] {return true;};QSignalSpy replay(&restored,&PetReminders::due);restored.start();QCOMPARE(replay.size(),0);
    item=restored.handle("updateItem",{{"id",id},{"dueAt",now.addSecs(1).toString(Qt::ISODate)}})["item"].toMap();QCOMPARE(item["occurrence"].toInt(),2);
    now=now.addSecs(2);restored.check();QCOMPARE(replay.size(),1);
    restored.handle("completeItem",{{"id",id}});QCOMPARE(restored.handle("listItems",{})["items"].toList().size(),0);
    QCOMPARE(restored.handle("listItems",{{"status","completed"}})["items"].toList().size(),1);
    restored.handle("deleteItem",{{"id",id}});QCOMPARE(restored.handle("listItems",{{"status","all"}})["items"].toList().size(),0);
    for(const auto &bad:QStringList{"2026-11-02T08:00:00","2026-11-02T08:00:00+01:60","2026-02-30T08:00:00Z","2026-11-02T24:00:00Z","2026-11-02T08:60:00Z","2026-11-02T08:00:60Z"})
      QVERIFY_EXCEPTION_THROWN(restored.handle("createItem",{{"title","bad"},{"dueAt",bad}}),std::runtime_error);
    QVERIFY_EXCEPTION_THROWN(restored.handle("createItem",{{"title",QString(201,'x')}}),std::runtime_error);
    auto todo=restored.handle("createItem",{{"title","without time"}})["item"].toMap();
    QVERIFY_EXCEPTION_THROWN(restored.handle("updateItem",{{"id",todo["id"]},{"clearDueAt",false}}),std::runtime_error);
  }
  void nativePetReminderSuppressionAndWriteFailure() {
    QTemporaryDir temp;auto now=QDateTime::fromString("2026-11-02T08:00:00Z",Qt::ISODate);bool show=false,enabled=true;
    PetReminders service(temp.filePath("ok"),nullptr,[&]{return now;});service.visible=[&]{return show;};service.notifications=[&]{return enabled;};service.start();
    QSignalSpy due(&service,&PetReminders::due);
    auto create=[&](QString title){return service.handle("createItem",{{"title",title},{"dueAt",now.toString(Qt::ISODate)}})["item"].toMap();};
    auto item=create("hidden");QCOMPARE(item["notificationOutcome"].toString(),QString("suppressed_hidden"));show=true;service.check();QCOMPARE(due.size(),0);
    enabled=false;item=create("disabled");QCOMPARE(item["notificationOutcome"].toString(),QString("suppressed_preference"));enabled=true;service.check();QCOMPARE(due.size(),0);
    service.stop();create("stopped");QCOMPARE(due.size(),0);service.start();QCOMPARE(due.size(),1);
    // Replacing the target directory with a file simulates a durable write failure.
    service.stop();const auto folder=temp.filePath("blocked");PetReminders blocked(folder,nullptr,[&]{return now;});
    QFile obstruction(folder);QVERIFY(obstruction.open(QIODevice::WriteOnly));obstruction.write("keep");obstruction.close();
    QVERIFY_EXCEPTION_THROWN(blocked.handle("createItem",{{"title","cannot persist"}}),std::runtime_error);
    QCOMPARE(blocked.handle("listItems",{})["items"].toList().size(),0);
    const auto corrupt=temp.filePath("corrupt");QDir().mkpath(corrupt);QFile broken(corrupt+"/reminders.json");QVERIFY(broken.open(QIODevice::WriteOnly));broken.write("broken");broken.close();
    PetReminders invalid(corrupt);QVERIFY_EXCEPTION_THROWN(invalid.handle("createItem",{{"title","must not overwrite"}}),std::runtime_error);
    QVERIFY(broken.open(QIODevice::ReadOnly));QCOMPARE(broken.readAll(),QByteArray("broken"));
  }
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
    QVERIFY(complex["chunked"].toBool()); QVERIFY(complex["html"].toString().isEmpty());
    QCOMPARE(complex["text"].toString().size(),15000);
    const auto big = write("huge.py", QByteArray(270000, 'x'));
    QFile huge(big); QVERIFY(huge.open(QIODevice::ReadWrite)); QVERIFY(huge.resize(512ll * 1024 * 1024)); huge.close();
    auto limited = DocumentPreview::read(big, "file");
    QVERIFY(limited["truncated"].toBool()); QVERIFY(limited["text"].toString().size() <= 48002);
    QVERIFY(!limited["highlight"].toBool());
    auto lines = DocumentPreview::read(write("lines.txt", QByteArray("line\n").repeated(2000)), "file");
    QVERIFY(!lines["truncated"].toBool()); QVERIFY(lines["chunked"].toBool()); QCOMPARE(lines["text"].toString().count('\n'),2000);
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
