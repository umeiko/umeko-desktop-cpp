#include "session_files.hpp"
#include "backend.hpp"
#include "global_agents.hpp"
#include <QElapsedTimer>

SessionFiles::SessionFiles(Backend *backend) : QObject(backend), b(backend) {}
QString SessionFiles::workspace(QString path) const {
  if(path.isEmpty())return {};
  const auto key=U::cleanPath(path);
#ifdef Q_OS_WIN
  return key.toLower();
#else
  return key;
#endif
}
QVariantList SessionFiles::projectSessions(QString path) const {
  QVariantList rows;const auto key=workspace(path);
  for(const auto &value:b->sessionIndex)if(workspace(value.toMap()["workspace"].toString())==key)rows<<value;
  return rows;
}
QVariantMap SessionFiles::projectSummary(QString id) const {
  for(const auto &value:b->store.data["projects"].toList()){
    const auto project=value.toMap();if(project["id"]!=id)continue;
    const auto key=workspace(project["path"].toString());bool running=m_busy;
    for(const auto &record:b->records)if(workspace(record->workspace)==key)running|=record->busy()||record->subagentsActive||!record->approval.isEmpty();
    return {{"count",projectSessions(key).size()},{"busy",running}};
  }
  return {};
}
void SessionFiles::dismissFailure(){m_failure.clear();emit failureChanged();}
void SessionFiles::fail(QString title,QVariantList errors){
  QStringList lines;for(const auto &value:errors.mid(0,20)){const auto row=value.toMap();lines<<(row["name"].toString()+"\n"+row.value("error","操作失败").toString()).trimmed();}
  m_failure={{"title",title},{"details",lines.join("\n\n")}};emit failureChanged();
}
void SessionFiles::importFiles(QString raw,QString id){
  if(m_busy){emit b->toast("请等待当前会话文件操作完成");return;}
  QString path;bool found=id.isEmpty();
  for(const auto &value:b->store.data["projects"].toList())if(value.toMap()["id"]==id){path=value.toMap()["path"].toString();found=true;}
  if(!found)return;
  const auto document=QJsonDocument::fromJson(raw.toUtf8());QVariantList sources;
  if(document.isArray())for(const auto &value:document.array()){
    const QUrl url(value.toString());if(!url.isLocalFile()){fail("无法导入会话",{QVariantMap{{"error","仅支持本地 JSONL 会话文件"}}});return;}
    sources<<url.toLocalFile();
  }
  if(sources.isEmpty()||sources.size()>20){fail("无法导入会话",{QVariantMap{{"error","每次可导入 1–20 个会话文件"}}});return;}
  m_busy=true;emit changed();
  execute({{"operation","import"},{"sources",sources},{"profile",b->store.directory},{"sessionDir",b->store.directory+"/pi/sessions"},
    {"cwd",path.isEmpty()?b->store.directory+"/workspace":path}},{{"workspace",path},{"project",id}});
}
void SessionFiles::deleteSession(QString id){for(const auto &value:b->sessionIndex)if(value.toMap()["id"]==id){remove({value},{});return;}}
void SessionFiles::deleteProject(QString id){for(const auto &value:b->store.data["projects"].toList())if(value.toMap()["id"]==id){remove(projectSessions(value.toMap()["path"].toString()),id);return;}}
void SessionFiles::remove(QVariantList sessions,QString id){
  if(m_busy){emit b->toast("请等待当前会话文件操作完成");return;}
  QString path;for(const auto &value:b->store.data["projects"].toList())if(value.toMap()["id"]==id)path=value.toMap()["path"].toString();
  QSet<QString> ids;for(const auto &value:sessions)ids.insert(value.toMap()["id"].toString());
  QList<std::shared_ptr<Session>> records;
  for(const auto &record:b->records)if(ids.contains(record->id)||!id.isEmpty()&&workspace(record->workspace)==workspace(path)){
    if(record->busy()||record->subagentsActive||!record->approval.isEmpty()) {emit b->toast("此会话或项目仍有任务运行，请先停止任务");return;}
    records<<record;
  }
  m_busy=true;pending=ids;for(const auto &record:records)pending.insert(record->id);emit changed();
  b->previewLoader->cancel();
  b->conversationSearch->clear();QList<QPointer<QProcess>> processes;
  if(auto search=b->conversationSearch->retire())processes<<search;
  b->flushDsh();const auto writer=b->dshWriter;
  b->children->forget(records);
  for(const auto &record:records){if(record->process)processes<<record->process;b->retire(record);}
  QVariantMap payload{{"operation","delete"},{"profile",b->store.directory},{"sessions",sessions}};
  if(!id.isEmpty())payload["workspace"]=path;
  auto timer=new QTimer(this);auto elapsed=std::make_shared<QElapsedTimer>();elapsed->start();timer->setInterval(30);
  connect(timer,&QTimer::timeout,this,[this,timer,elapsed,processes,writer,payload,id,records]{
    bool alive=false;for(const auto &process:processes)alive|=process&&process->state()!=QProcess::NotRunning;
    if((alive||!writer.isFinished())&&elapsed->elapsed()<15000)return;
    timer->stop();timer->deleteLater();
    if(alive||!writer.isFinished()){m_busy=false;pending.clear();emit changed();fail("会话未能完全删除",{QVariantMap{{"error","会话进程尚未退出，未删除会话数据"}}});return;}
    execute(payload,{{"project",id}},records);
  });timer->start();
}
void SessionFiles::execute(QVariantMap payload,QVariantMap context,QList<std::shared_ptr<Session>> records){
  U::run(this,U::node(),{b->root+"/runtime/session-files.mjs"},QJsonDocument::fromVariant(payload).toJson(QJsonDocument::Compact),
    [this,payload,context,records](bool ok,QByteArray raw,QString error){
      m_busy=false;pending.clear();emit changed();if(b->closing)return;
      const auto result=QJsonDocument::fromJson(raw).object().toVariantMap();const bool importing=payload["operation"]=="import";
      if(!ok||!result["ok"].toBool()){fail(importing?"无法导入会话":"会话未能完全删除",{QVariantMap{{"error",result.value("error",error)}}});return;}
      const auto errors=result["errors"].toList();
      if(importing){
        for(const auto &value:result["sessions"].toList()){auto info=value.toMap();info["workspace"]=context["workspace"];b->sessionIndex.prepend(info);
          for(const auto &project:b->store.data["projects"].toList())if(project.toMap()["id"]==context["project"]&&project.toMap()["archived"].toBool()){
            auto meta=b->store.data["sessionMeta"].toMap();meta[info["id"].toString()]=QVariantMap{{"archived",true}};b->store.data["sessionMeta"]=meta;
          }}
        b->store.save();b->saveSessions();if(!result["sessions"].toList().isEmpty())emit b->toast("会话导入成功");
      }else{
        QSet<QString> ids;for(const auto &value:result["deleted"].toList())ids.insert(value.toMap()["id"].toString());deleted.unite(ids);
        const auto project=context["project"].toString();const bool clear=ids.contains(b->view->id)||!project.isEmpty()&&errors.isEmpty()&&b->store.data["activeProject"]==project;
        for(int i=b->sessionIndex.size()-1;i>=0;--i)if(ids.contains(b->sessionIndex[i].toMap()["id"].toString()))b->sessionIndex.removeAt(i);
        auto meta=b->store.data["sessionMeta"].toMap();for(const auto &id:ids)meta.remove(id);b->store.data["sessionMeta"]=meta;
        QStringList keys;
        for(const auto &record:records)if(ids.contains(record->id)||record->id.isEmpty()&&errors.isEmpty()){
          keys<<record->key;b->records.remove(record->key);b->lru.removeAll(record->key);b->dshDirty.remove(record->key);
          for(auto it=b->drafts.begin();it!=b->drafts.end();)if(it.value()==record)it=b->drafts.erase(it);else++it;
          record->messages.clear();record->tree.clear();record->displaySnapshot.clear();record->subagents.clear();record->attachments.clear();record->draft.clear();
        }
        b->desktopExtensions->globalAgents()->forgetSessions(ids,keys);
        if(!project.isEmpty()&&errors.isEmpty()){
          auto projects=b->store.data["projects"].toList();for(int i=projects.size()-1;i>=0;--i)if(projects[i].toMap()["id"]==project)projects.removeAt(i);b->store.data["projects"]=projects;
          auto removed=b->store.data["removedProjects"].toList();if(!removed.contains(project))removed<<project;b->store.data["removedProjects"]=removed;
        }
        if(clear){if(ids.contains(b->view->id))b->view->id.clear();if(!project.isEmpty()){b->store.data["workspace"]="";b->store.data["activeProject"]="";}b->activate(b->fresh(b->store.data["workspace"].toString()));}
        b->store.save();b->saveSessions();if(errors.isEmpty())emit b->toast(project.isEmpty()?"会话及数据已删除":"项目及会话数据已删除");
      }
      if(!errors.isEmpty())fail(importing?"无法导入会话":"会话未能完全删除",errors);
      emit b->changed();emit b->chatChanged();
    },180000);
}
