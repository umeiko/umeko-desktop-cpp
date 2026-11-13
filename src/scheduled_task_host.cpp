#include "widget_host.hpp"
#include "backend.hpp"
#include "global_agents.hpp"
#include <QTimeZone>
#include <stdexcept>

namespace {
void require(bool valid, QString error) {
  if(!valid)throw std::runtime_error(error.toStdString());
}
QString identity(std::shared_ptr<Session> record) {
  return record->id.isEmpty()?"draft:"+record->key:"session:"+record->id;
}
}

std::shared_ptr<Session> WidgetHost::timerRecord(QVariantMap source) {
  require(source["token"].typeId()==QMetaType::QString && !source["token"].toString().isEmpty()
          && source["id"].typeId()==QMetaType::QString,"A captured conversation is required");
  auto record=b->records.value(source["token"].toString());
  const auto id=source["id"].toString();
  if(!record&&!id.isEmpty())record=m->coordinators->record("session:"+id);
  require(record && record->globalAgent.isEmpty() && record->workspace==source["workspace"].toString(),
          "原会话已不存在或工作目录已改变 / Original conversation is unavailable or changed");
  require(id.isEmpty()||record->id==id,"Conversation identity changed");
  return record;
}

QVariantMap WidgetHost::timerMetadata(std::shared_ptr<Session> record) const {
  QVariantList targets,globals;
  auto bus=m->coordinators;
  if(record)targets<<QVariantMap{{"id","main"},{"name","主 Agent / Main Agent"}};
  for(auto it=bus->agents.cbegin();it!=bus->agents.cend();++it) {
    const QVariantMap row{{"id","agent:"+it.key()},{"name",it.value().toMap()["name"]}};
    globals<<row;
    if(record&&bus->active&&bus->allowed(record,"agent:"+it.key()))targets<<row;
  }
  return {{"targets",targets},{"global_targets",globals},{"time_zone",QString::fromUtf8(QTimeZone::systemTimeZoneId())}};
}

QVariantMap WidgetHost::captureTimerSession(QVariantMap source) {
  auto record=timerRecord(source);const auto info=timerMetadata(record);
  source["id"]=record->id;source["token"]=record->key;
  source["timerTargets"]=info["targets"];source["timerGlobalTargets"]=info["global_targets"];
  source["timerTimeZone"]=info["time_zone"];
  return source;
}

void WidgetHost::scheduledTasks(Task request) {
  auto caller=timerRecord(request->session);
  const auto args=request->arguments.toMap();const auto operation=request->operation;
  const auto allowed=timerMetadata(caller)["targets"].toList();
  const auto connected=[&](QString target){for(const auto &row:allowed)if(row.toMap()["id"]==target)return true;return false;};
  if(operation=="scheduledTasks.targets"&&args.isEmpty()) {
    reply(request,{{"ok",true},{"result",timerMetadata(caller)}});return;
  }
  if(operation=="scheduledTasks.status"&&args.keys()==QStringList{"run_ids"}) {
    require(args["run_ids"].typeId()==QMetaType::QVariantList,"At most 32 run IDs are allowed");
    const auto ids=args["run_ids"].toList();require(ids.size()<=32,"At most 32 run IDs are allowed");
    QVariantList result;
    for(const auto &value:ids) {
      require(value.typeId()==QMetaType::QString,"Invalid run ID");
      const auto id=value.toString();require(timerJobs.contains(id),"Timer run does not belong to this conversation");
      auto &job=timerJobs[id];require(job.record.lock()==caller||job.owner==identity(caller),"Timer run does not belong to this conversation");
      job.owner=identity(caller);
      if(job.state=="queued") {
        if(!job.bus.isEmpty()) {
          const auto index=m->coordinators->requestIndex(job.bus);
          if(index<0){job.state="failed";job.detail="Agent Bus request is no longer available";}
          else {
            const auto row=m->coordinators->requests[index].toMap();const auto status=row["status"].toString();
            if(status=="running"||status=="completed"){job.state="delivered";job.detail.clear();}
            else if(QStringList{"failed","cancelled","interrupted"}.contains(status)){job.state="failed";job.detail=row.value("result",status).toString();}
          }
        }
        if(job.state=="queued" && std::chrono::steady_clock::now()-job.created>std::chrono::seconds(60)) {
          job.state="failed";job.detail="投递回执超时；未自动重发 / Delivery receipt timed out; not resent";
        }
      }
      if(auto target=job.targetRecord.lock())job.sessionId=target->id;
      result<<QVariantMap{{"id",id},{"state",job.state},{"detail",job.detail},{"session_id",job.sessionId}};
    }
    reply(request,{{"ok",true},{"result",QVariantMap{{"runs",result}}}});return;
  }
  if(operation=="scheduledTasks.trigger"&&args.keys()==QStringList{"run_id","target","text"}) {
    const auto id=args["run_id"].toString(),target=args["target"].toString(),text=args["text"].toString();
    require(args["run_id"].typeId()==QMetaType::QString&&QRegularExpression("^[a-f0-9]{32}$").match(id).hasMatch(),"Invalid timer run ID");
    require(args["text"].typeId()==QMetaType::QString&&!text.trimmed().isEmpty()&&text.size()<=8000,"Message must contain 1–8000 characters");
    require(connected(target),"目标未与本会话建立 Agent Bus / Target is not connected to this conversation");
    if(timerJobs.contains(id)) {
      const auto &job=timerJobs[id];require(job.record.lock()==caller&&job.target==target&&job.text==text,"Timer run ID is already bound to another request");
      reply(request,{{"ok",true},{"result",QVariantMap{{"state",job.state}}}});return;
    }
    while(timerJobs.size()>=128) {
      auto old=std::find_if(timerJobOrder.cbegin(),timerJobOrder.cend(),[&](QString key){return timerJobs[key].state!="queued";});
      require(old!=timerJobOrder.cend(),"Timer delivery queue is full");const auto key=*old;timerJobOrder.removeAll(key);timerJobs.remove(key);
    }
    TimerJob job;job.record=caller;job.targetRecord=caller;job.owner=identity(caller);job.target=target;job.text=text;
    if(target=="main") {
      require(caller->backend!="dsh","定时触发暂仅支持 Pi 会话 / Timer delivery currently requires Pi");
      int busy=0;for(auto record:b->records)if(record->busy()&&record->globalAgent.isEmpty())++busy;
      require(caller->busy()||busy<8,"原生会话并发已满 / Native session capacity is full");
      const QVariantMap notification{{"id",id},{"requestId",""},{"widgetId","scheduled-tasks"},{"name","定时任务 / Scheduled Tasks"},
          {"message",text},{"wake",true},{"data",QVariantMap{{"timerRunId",id}}}};
      if(caller->busy()&&caller->process&&caller->ready)b->writePi(caller,{{"type","widget-notify"},{"notification",notification}});
      else require(b->command(caller,"widget-notify",{{"notification",notification}}),"目标内核暂不可用 / Target kernel is unavailable");
    } else {
      const auto result=m->coordinators->execute("agent_bus.dispatch",{{"targetId",target},{"text",text}},caller->key);
      job.bus=result["requestId"].toString();
      if(auto record=m->coordinators->records.value(target.mid(6)))job.targetRecord=record;
    }
    timerJobs[id]=job;timerJobOrder<<id;
    reply(request,{{"ok",true},{"result",QVariantMap{{"state","queued"},{"source",QVariantMap{{"token",caller->key},{"id",caller->id},{"workspace",caller->workspace}}}}}});return;
  }
  if(operation=="scheduledTasks.openTarget"&&args.keys()==QStringList{"target"}) {
    require(request->page&&m->m_page=="scheduled-tasks","Open targets from the timer page");
    const auto target=args["target"].toString();require(connected(target),"Target is no longer connected");
    if(target=="main") {if(!caller->id.isEmpty())b->openSession(caller->id);else m->closePage();}
    else {m->openPage("global-agents");m->coordinators->selectConversation(target.mid(6));}
    reply(request,{{"ok",true},{"result",QVariantMap{}}});return;
  }
  require(false,"Invalid Scheduled Tasks host arguments");
}

void WidgetHost::timerObserve(QString key,QVariantMap event) {
  if(event["type"]!="widget_delivery")return;
  auto id=event["id"].toString();if(!timerJobs.contains(id))return;
  auto &job=timerJobs[id];auto record=job.record.lock();
  if(!record||record->key!=key||job.state!="queued")return;
  const auto status=event["status"].toString();
  if(status=="delivered"||status=="failed"){job.state=status;job.detail=event["error"].toString().left(1000);}
}
