#include "widget_host.hpp"
#include "backend.hpp"
#include <QtConcurrent>
#include <stdexcept>

bool WidgetHost::prepareSession(std::shared_ptr<Session> record) {
  if(record->pluginOwner.isEmpty())return true;
  try {sessionPolicy(record);return true;}catch(const std::exception &error){emit b->toast(QString::fromUtf8(error.what()));return false;}
}

namespace {
void check(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
qint64 clockMs(){return QDateTime::currentMSecsSinceEpoch();}
QVariantMap runResult(const QString &id,const QString &status,const QStringList &messages,const QString &error,bool truncated=false){return {{"runId",id},{"status",status},{"assistantMessages",messages},{"error",error},{"truncated",truncated}};}
bool terminal(QString status){return QStringList{"completed","failed","cancelled"}.contains(status);}
}
void WidgetHost::sessionPolicy(std::shared_ptr<Session> record) {
  const auto profile=b->store.data["pluginSessionProfiles"].toMap()[record->pluginOwner["plugin"].toString()].toMap()[record->pluginOwner["profile"].toString()].toMap();
  check(!profile.isEmpty(),"The desktop session profile is unavailable");
  const auto path=profile["workspace"].toString();
  check(QDir::isAbsolutePath(path)&&QFileInfo(path).isDir(),"The profile workspace must be an existing absolute directory");
  const auto workspace=U::cleanPath(path);
  check(record->file.isEmpty() || U::cleanPath(record->workspace)==workspace,"The profile workspace changed; acquire a new external key");
  const auto model=profile["model"].toString();const auto provider=profile["provider"].toString();QString resolved;
  for(const auto &value:b->modelCatalog())if(value.toMap()["id"]==model && (provider.isEmpty() || value.toMap()["provider"]==provider)){resolved=value.toMap()["provider"].toString();break;}
  check(!resolved.isEmpty(),"The profile must name an existing desktop model and provider");
  const auto raw=profile.value("tools",QVariantList{});check(raw.typeId()==QMetaType::QVariantList||raw.typeId()==QMetaType::QStringList,"The profile tools must be an array");
  QStringList tools;
  for(const auto &value:raw.toList()){
    const auto name=value.toString();check(value.typeId()==QMetaType::QString&&!name.isEmpty()&&name.size()<=256,"The profile tools must be exact tool names");
    check(!QStringList{"umeko_desktop","pi","pi_commands","run_pi_command"}.contains(name),"Desktop administration tools cannot be enabled in plugin sessions");
    if(!tools.contains(name))tools<<name;
  }
  check(tools.size()<=256,"Too many profile tools");
  auto mode=profile.value("permissionMode","ask").toString();check(mode=="ask"||mode=="auto","Invalid profile permissionMode");
  if(b->store.data["agent"].toMap()["toolPermissionMode"]=="ask")mode="ask";
  const QVariantMap policy{{"workspace",workspace},{"model",model},{"provider",resolved},{"allowedTools",tools},{"mode",mode},{"allowApprovals",profile["allowApprovals"].toBool()}};
  if(record->busy())return;
  if(record->pluginPolicy!=policy && record->process)b->retire(record);
  record->workspace=workspace;record->model=model;record->provider=resolved;record->pluginPolicy=policy;record->dangerousAllowed=false;
}
void WidgetHost::sessionLease(Task request,std::shared_ptr<Session> record,bool created) {
  owner(request);check(sessionLeases.size()<64,"Too many active session handles");
  const auto handle=U::uuid();sessionLeases[handle]={request->widget,request->worker,record,false,false};
  reply(request,{{"ok",true},{"result",QVariantMap{{"handle",handle},{"sessionId",record->id},{"created",created}}}});
}
void WidgetHost::sessionService(Task request) {
  const auto args=request->arguments.toMap();const auto operation=request->operation;
  const QMap<QString,QStringList> fields{
    {"sessions.acquire",{"namespace","externalKey","createIfMissing","profile"}},
    {"sessions.submit",{"handle","requestId","content"}},{"sessions.subscribe",{"handle"}},
    {"sessions.wait",{"handle","runId"}},{"sessions.cancel",{"handle","runId"}},
    {"sessions.history",{"handle","limit"}},{"sessions.release",{"handle"}},
    {"approvals.registerHandler",{"handle"}},{"approvals.respond",{"handle","requestId","decision"}}};
  for(auto it=args.cbegin();it!=args.cend();++it)check(fields[operation].contains(it.key()),"Unknown session service arguments");
  if(operation=="sessions.acquire") {
    const auto space=args.value("namespace","default").toString(),external=args["externalKey"].toString(),profile=args["profile"].toString();
    for(const auto &field:QStringList{"externalKey","profile"})check(args[field].typeId()==QMetaType::QString,"A desktop profile and external key are required");
    check(!space.isEmpty()&&space.size()<=512&&!external.isEmpty()&&external.size()<=512&&!profile.isEmpty()&&profile.size()<=512,"Invalid external session identity");
    if(args.contains("namespace"))check(args["namespace"].typeId()==QMetaType::QString,"Invalid namespace");
    if(args.contains("createIfMissing"))check(args["createIfMissing"].typeId()==QMetaType::Bool,"createIfMissing must be boolean");
    const auto identity=QString::fromUtf8(QJsonDocument(QJsonArray{request->widget,space,external}).toJson(QJsonDocument::Compact));
    const auto binding=b->store.data["pluginSessions"].toMap()[identity].toMap();
    if(!binding.isEmpty()) {
      check(binding["profile"]==profile,"This external key is bound to another profile");
      const auto meta=b->metadata(binding["sessionId"].toString());
      check(!meta["archived"].toBool()&&!meta["pluginSession"].toMap().isEmpty()&&QFileInfo(binding["file"].toString()).isFile(),"The mapped native session was removed or archived");
      auto record=b->findSession(binding["sessionId"].toString());
      if(!record){record=b->fresh(binding["workspace"].toString());record->id=binding["sessionId"].toString();record->file=binding["file"].toString();record->title="Plugin session";}
      record->pluginOwner=meta["pluginSession"].toMap();sessionPolicy(record);sessionLease(request,record,false);return;
    }
    check(args.value("createIfMissing",true).toBool(),"No session is mapped to this external key");
    if(sessionAcquiring.contains(identity)){sessionAcquires[sessionAcquiring[identity]]<<request;return;}
    check(sessionAcquiring.size()<8,"Too many sessions are being acquired");
    auto record=b->fresh({});record->id="plugin-pending-"+record->key;record->title="Plugin session";
    record->pluginOwner={{"plugin",request->widget},{"namespace",space},{"externalKey",external},{"profile",profile}};
    try{sessionPolicy(record);}catch(...){b->records.remove(record->key);b->lru.removeAll(record->key);throw;}
    b->setMetadata(record->id,{{"pluginSession",record->pluginOwner}});
    sessionAcquiring[identity]=record->key;sessionAcquires[record->key]<<request;
    if(!b->command(record,"plugin-session-acquire"))QTimer::singleShot(0,this,[this,key=record->key]{sessionObserve(key,{{"type","exit"},{"message","The native session could not be created"}});});
    return;
  }
  const auto handle=args["handle"].toString();auto it=sessionLeases.find(handle);
  check(args["handle"].typeId()==QMetaType::QString&&it!=sessionLeases.end()&&it->plugin==request->widget&&it->worker==request->worker,"Session handle does not belong to this plugin instance");
  auto record=it->record;check(!b->metadata(record->id)["archived"].toBool(),"The acquired session was archived");
  const auto ok=[this,request](QVariantMap result){reply(request,{{"ok",true},{"result",result}});};
  if(operation=="sessions.subscribe"){it->subscribed=true;ok({{"subscribed",true}});return;}
  if(operation=="sessions.release"){sessionLeases.erase(it);ok({{"released",true}});b->trimPool();return;}
  if(operation=="approvals.registerHandler"){sessionPolicy(record);check(record->pluginPolicy["allowApprovals"].toBool(),"The desktop profile does not allow plugin approval handling");it->handler=true;ok({{"registered",true}});return;}
  if(operation=="approvals.respond") {
    const auto decision=args["decision"].toString(),id=args["requestId"].toString();
    check(QStringList{"allow","deny","unhandled"}.contains(decision),"Invalid approval decision");
    auto approval=sessionApprovals.find(id);bool accepted=false;
    if(approval!=sessionApprovals.end() && approval->expires<=clockMs()){answerSessionApproval(id,false);approval=sessionApprovals.end();}
    if(approval!=sessionApprovals.end() && approval->handlerExpires<=clockMs()){fallbackSessionApproval(id);approval=sessionApprovals.find(id);}
    sessionPolicy(record);
    if(approval!=sessionApprovals.end()&&approval->record==record&&it->handler&&approval->handlers.contains(handle)&&!approval->desktop&&b->store.data["pluginSessionProfiles"].toMap()[record->pluginOwner["plugin"].toString()].toMap()[record->pluginOwner["profile"].toString()].toMap()["allowApprovals"].toBool()) {
      accepted=true;if(decision=="unhandled"){approval->handlers.remove(handle);if(approval->handlers.isEmpty())fallbackSessionApproval(id);}
      else answerSessionApproval(id,decision=="allow");
    }ok({{"accepted",accepted}});return;
  }
  if(operation=="sessions.submit") {
    check(args["requestId"].typeId()==QMetaType::QString&&!args["requestId"].toString().isEmpty()&&args["requestId"].toString().size()<=256&&args["content"].typeId()==QMetaType::QString&&!args["content"].toString().trimmed().isEmpty(),"A requestId and nonempty text content are required");
    for(const auto &run:sessionRuns)if(run.record==record&&run.requestId==args["requestId"]){ok({{"runId",run.id},{"disposition",run.disposition}});return;}
    if(sessionRuns.size()>=512)for(const auto &old:sessionRunOrder)if(sessionRuns.contains(old)&&terminal(sessionRuns[old].status)){sessionRuns.remove(old);sessionRunOrder.removeAll(old);break;}
    check(sessionRuns.size()<512,"The session queue is full; wait for active tasks");
    check(sessionQueues[record->key].size()<64,"The session queue is full");sessionPolicy(record);
    const auto id=U::uuid();PluginRun run;run.record=record;run.id=id;run.requestId=args["requestId"].toString();run.content=args["content"].toString();run.disposition=record->busy()||sessionActive.contains(record->key)||!sessionQueues[record->key].isEmpty()?"queued":"started";
    sessionRuns[id]=run;sessionRunOrder<<id;sessionQueues[record->key]<<id;ok({{"runId",id},{"disposition",run.disposition}});nextSessionRun(record);return;
  }
  if(operation=="sessions.history") {
    const auto limit=args.value("limit",50);check(limit.typeId()!=QMetaType::Bool&&limit.toDouble()==limit.toInt()&&limit.toInt()>=1&&limit.toInt()<=200,"limit must be from 1 to 200");
    const auto file=record->file;const auto fallback=record->messages;const auto id=record->id;const int count=limit.toInt();
    auto watcher=new QFutureWatcher<QVariantMap>(this);connect(watcher,&QFutureWatcher<QVariantMap>::finished,this,[this,request,watcher]{try{owner(request);reply(request,{{"ok",true},{"result",watcher->result()}});}catch(const std::exception &error){reply(request,{{"ok",false},{"error",error.what()}});}watcher->deleteLater();});
    watcher->setFuture(QtConcurrent::run([file,fallback,id,count]{auto messages=QFileInfo(file).isFile()?U::preview(file).first:fallback;auto rows=messages.mid(qMax(0,int(messages.size())-count));bool truncated=rows.size()<messages.size();while(!rows.isEmpty()&&QJsonDocument::fromVariant(rows).toJson(QJsonDocument::Compact).size()>240000){rows.removeFirst();truncated=true;}return QVariantMap{{"sessionId",id},{"messages",rows},{"truncated",truncated}};}));return;
  }
  const auto runId=args["runId"].toString();
  if(!runId.isEmpty())check(sessionRuns.contains(runId)&&sessionRuns[runId].record==record,"The run does not belong to this session");
  if(operation=="sessions.wait") {
    check(!runId.isEmpty(),"A runId is required");auto &run=sessionRuns[runId];
    if(terminal(run.status))ok(runResult(run.id,run.status,run.messages,run.error,run.truncated));else run.waiters<<request;return;
  }
  if(operation=="sessions.cancel") {
    for(const auto &id:sessionQueues[record->key])if(runId.isEmpty()||id==runId)finishSessionRun(id,"cancelled");
    const auto active=sessionActive.value(record->key);if(!active.isEmpty()&&(runId.isEmpty()||active==runId)){sessionRuns[active].cancelling=true;record->cancelled=true;b->writePi(record,{{"type","abort"}});}
    for(const auto &id:sessionApprovals.keys())if(sessionApprovals[id].record==record)answerSessionApproval(id,false);
    ok({{"cancelled",true}});return;
  }
  check(false,"Unknown session service operation");
}
void WidgetHost::nextSessionRun(std::shared_ptr<Session> record) {
  auto &queue=sessionQueues[record->key];if(queue.isEmpty()||record->busy()||sessionActive.contains(record->key))return;
  const auto id=queue.takeFirst();auto &run=sessionRuns[id];sessionActive[record->key]=id;run.status="running";
  try{sessionPolicy(record);if(!b->command(record,"prompt",{{"text",run.content},{"images",QVariantList{}}}))throw std::runtime_error("The session could not start this input");}
  catch(const std::exception &error){finishSessionRun(id,"failed",error.what());}
}
void WidgetHost::finishSessionRun(QString id,QString status,QString error) {
  auto it=sessionRuns.find(id);if(it==sessionRuns.end()||terminal(it->status))return;
  auto record=it->record;if(sessionActive.value(record->key)==id)sessionActive.remove(record->key);sessionQueues[record->key].removeAll(id);
  it->status=status;if(!error.isEmpty())it->error=error;
  auto result=runResult(id,status,it->messages,it->error,it->truncated);auto event=result;event.remove("runId");emitSessionEvent(record,"turn_end",id,event);
  const auto waiters=it->waiters;it->waiters.clear();for(const auto &request:waiters)reply(request,{{"ok",true},{"result",result}});
  QTimer::singleShot(0,this,[this,record]{nextSessionRun(record);});
}
void WidgetHost::emitSessionEvent(std::shared_ptr<Session> record,QString type,QString run,QVariantMap values) {
  for(auto it=sessionLeases.cbegin();it!=sessionLeases.cend();++it)if(it->record==record&&(it->subscribed||(type=="approval_request"&&it->handler))&&m->workers.value(it->plugin)==it->worker) {
    auto data=values;
    const auto encoded=QJsonDocument::fromVariant(data).toJson(QJsonDocument::Compact);
    if(encoded.size()>60000)data={{"truncated",true},{"preview",QString::fromUtf8(encoded.left(24000))}};
    data.insert({{"handle",it.key()},{"sessionId",record->id},{"runId",run},{"type",type}});
    m->send(it->plugin,{{"id",U::uuid()},{"operation","event"},{"arguments",QVariantMap{{"name","sessions.event"},{"data",data}}}});
  }
}
void WidgetHost::sessionObserve(QString key,QVariantMap event) {
  auto record=b->records.value(key);if(!record || record->pluginOwner.isEmpty())return;
  const auto type=event["type"].toString();
  if((type=="done"||type=="exit")&&sessionAcquires.contains(key)) {
    const auto requests=sessionAcquires.take(key);QString identity;for(auto it=sessionAcquiring.cbegin();it!=sessionAcquiring.cend();++it)if(it.value()==key){identity=it.key();break;}sessionAcquiring.remove(identity);
    const bool valid=type=="done"&&!record->failed&&!record->id.startsWith("plugin-pending-")&&QFileInfo(record->file).isFile();
    if(valid){auto mappings=b->store.data["pluginSessions"].toMap();auto binding=record->pluginOwner;binding.insert({{"sessionId",record->id},{"file",record->file},{"workspace",record->workspace},{"model",record->model},{"provider",record->provider}});mappings[identity]=binding;b->store.data["pluginSessions"]=mappings;b->setMetadata(record->id,{{"pluginSession",record->pluginOwner}});b->indexSession(record);b->saveSessions();b->store.save();}
    for(int i=0;i<requests.size();++i)try{if(valid)sessionLease(requests[i],record,i==0);else reply(requests[i],{{"ok",false},{"error","Pi did not persist the native session"}});}catch(const std::exception &error){reply(requests[i],{{"ok",false},{"error",error.what()}});}
  }
  const auto id=sessionActive.value(key);
  if(!id.isEmpty()) {
    auto &run=sessionRuns[id];
    if(type=="delta")emitSessionEvent(record,"stream_delta",id,{{"text",event["text"]}});
    else if(type=="message"&&event["role"]=="assistant") {const auto text=event["text"].toString();if(text.size()>16000)run.truncated=true;run.messages<<text.left(16000);while(run.messages.join("").size()>60000){run.messages.removeFirst();run.truncated=true;}if(!event["error"].toString().isEmpty())run.error=event["error"].toString();emitSessionEvent(record,"assistant_text",id,{{"text",event["text"]},{"stopReason",event["stopReason"]}});}
    else if(type=="tool")emitSessionEvent(record,event["state"]=="done"||event["state"]=="error"?"tool_result":"tool_call",id,{{"tool",event}});
    else if(type=="error"){run.error=event["message"].toString();emitSessionEvent(record,"error",id,{{"message",run.error}});}
    else if(type=="done"||type=="exit")finishSessionRun(id,run.cancelling||record->cancelled?"cancelled":type=="exit"||record->failed||!run.error.isEmpty()?"failed":"completed");
  }
  if(type=="done"||type=="exit")for(const auto &id:sessionApprovals.keys())if(sessionApprovals[id].record==record)answerSessionApproval(id,false);
}
bool WidgetHost::captureSessionApproval(QString key,QVariantMap event) {
  const auto record=b->records.value(key);if(!record||record->pluginOwner.isEmpty())return false;
  QSet<QString> handlers;
  if(event["method"]=="confirm"&&record->pluginPolicy["allowApprovals"].toBool())for(auto it=sessionLeases.cbegin();it!=sessionLeases.cend();++it)if(it->record==record&&it->handler&&m->workers.value(it->plugin)==it->worker)handlers.insert(it.key());
  const auto id=event["id"].toString();sessionApprovals[id]={record,event,handlers,clockMs()+120000,clockMs()+5000,handlers.isEmpty()};
  emitSessionEvent(record,"approval_request",sessionActive.value(key),{{"requestId",id},{"approval",event},{"timeoutMs",120000}});
  if(handlers.isEmpty())showSessionApproval();else QTimer::singleShot(5000,this,[this,id]{fallbackSessionApproval(id);});
  QTimer::singleShot(120000,this,[this,id]{answerSessionApproval(id,false);});return true;
}
void WidgetHost::fallbackSessionApproval(QString id){auto it=sessionApprovals.find(id);if(it==sessionApprovals.end())return;it->desktop=true;showSessionApproval();}
void WidgetHost::showSessionApproval(){
  if(pending || !visibleSessionApproval.isEmpty() || !approval.isEmpty())return;
  for(auto it=sessionApprovals.cbegin();it!=sessionApprovals.cend();++it)if(it->desktop){visibleSessionApproval=it.key();approval=it->event;approval["title"]="Plugin session · "+approval["title"].toString();emit changed();return;}
}
bool WidgetHost::answerSessionApproval(QString id,QVariant value){
  auto it=sessionApprovals.find(id);if(it==sessionApprovals.end())return false;const auto item=it.value();sessionApprovals.erase(it);
  const auto method=item.event["method"].toString();
  if(clockMs()>=item.expires)value=false;
  else if(method=="confirm")value=value.typeId()==QMetaType::Bool && value.toBool();
  else if(method=="select" && !item.event["options"].toList().contains(value))value=QVariant();
  else if(method!="confirm" && value.typeId()!=QMetaType::QString)value=QVariant();
  b->writePi(item.record,{{"type","ui_response"},{"id",id},{"value",value}});
  if(visibleSessionApproval==id){visibleSessionApproval.clear();approval.clear();emit changed();}
  QTimer::singleShot(0,this,[this]{showSessionApproval();});return true;
}
void WidgetHost::stopSessions(QString plugin){
  QSet<QString> records;
  for(auto it=sessionLeases.begin();it!=sessionLeases.end();)if(it->plugin==plugin){records.insert(it->record->key);it=sessionLeases.erase(it);}else++it;
  for(const auto &key:records){auto record=b->records.value(key);if(!record)continue;bool awaiting=false;for(const auto &id:sessionApprovals.keys())if(sessionApprovals[id].record==record){awaiting=true;fallbackSessionApproval(id);}
    const auto queued=sessionQueues.value(key);for(const auto &id:queued)finishSessionRun(id,"cancelled");
    if(!awaiting&&!sessionActive.value(key).isEmpty()){sessionRuns[sessionActive[key]].cancelling=true;record->cancelled=true;b->writePi(record,{{"type","abort"}});}
  }
}

void WidgetHost::respondPiDialog(QString value){if(!visibleSessionApproval.isEmpty())answerSessionApproval(visibleSessionApproval,value);}
