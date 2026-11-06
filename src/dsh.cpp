#include "backend.hpp"
#include "dsh.hpp"
#include <QtConcurrent>
#include <QNetworkProxyFactory>

namespace {
QString npmEntry() {
  const auto folder=QFileInfo(U::node()).absolutePath();
  for(const auto &path:QStringList{folder+"/node_modules/npm/bin/npm-cli.js",folder+"/../lib/node_modules/npm/bin/npm-cli.js"})if(QFileInfo(path).isFile())return U::cleanPath(path);
  return {};
}
QVariantMap helperInput(Backend *b,QString operation) {
  return {{"root",b->root},{"node",U::node()},{"npm",npmEntry()},{"operation",operation},{"path",b->dshRuntimeService()->property("path")},{"runtimeRoot",b->store.directory+"/dsh/runtime"}};
}
QProcessEnvironment dshEnvironment(Backend *b,bool own) {
  auto env=U::environment(b->store.data["agent"].toMap());
  if(own){const auto system=QProcessEnvironment::systemEnvironment();for(const auto &name:system.keys())if(name.toUpper().endsWith("API_KEY") || name.toUpper().endsWith("ACCESS_TOKEN"))env.insert(name,system.value(name));}
  QStringList keys;for(const auto &value:b->store.data["providers"].toList()){const auto key=value.toMap()["key"].toString();if(!key.isEmpty())keys<<key;}
  for(const auto &name:env.keys()) {
    const auto upper=name.toUpper();
    if(upper.startsWith("UMEKO_DSH_KEY_") || upper=="UMEKO_DSH_PROXY_TOKEN" || (!own && (upper.endsWith("API_KEY") || upper.endsWith("APIKEY") || upper.endsWith("ACCESS_TOKEN") || upper.endsWith("AUTH_TOKEN") || upper.endsWith("_SECRET_KEY") || keys.contains(env.value(name)))))env.remove(name);
  }
  if(!own)env.insert("NO_PROXY",env.value("NO_PROXY",env.value("no_proxy"))+",127.0.0.1,localhost");
  env.insert("DSH_TELEMETRY_DISABLED","1");return env;
}
QVariantMap modelOption(const std::shared_ptr<Session> &s) {for(const auto &value:s->dshOptions){const auto o=value.toMap();if(o["id"]=="model" || o["category"]=="model")return o;}return {};}
QVariantList choices(QVariantList options) {QVariantList out;for(const auto &value:options){const auto o=value.toMap();if(o.contains("options"))out.append(choices(o["options"].toList()));else out<<o;}return out;}
}
DshRuntime::DshRuntime(Backend *backend):QObject(backend),b(backend) {}
QString DshRuntime::path() const{return b->store.data["dsh"].toMap()["dshPath"].toString();}
bool DshRuntime::useOwnConfig() const{return b->store.data["dsh"].toMap().value("useOwnConfig",false).toBool();}
void DshRuntime::setPath(QString value) {
  if(process)return;value=value.trimmed();if(value.startsWith('"')&&value.endsWith('"'))value=value.mid(1,value.size()-2);
  if(value==path() || value.size()>4096)return;auto config=b->store.data["dsh"].toMap();config["dshPath"]=value;b->store.data["dsh"]=config;b->store.save();
  m_ready=m_failed=false;m_path.clear();m_version.clear();m_status="路径已更改，请检查";m_hint="检查通过后即可使用。";emit changed();
}
void DshRuntime::setUseOwnConfig(bool value) {
  if(value==useOwnConfig())return;auto config=b->store.data["dsh"].toMap();config["useOwnConfig"]=value;b->store.data["dsh"]=config;
  if(!b->store.save()){config["useOwnConfig"]=!value;b->store.data["dsh"]=config;emit b->toast("dsh 设置保存失败");return;}
  emit changed();emit b->chatChanged();emit b->changed();
}
void DshRuntime::check(){start(false);}
void DshRuntime::download(){start(true);}
void DshRuntime::cancel(){if(process){cancelled=true;m_status="正在取消";process->write("{\"type\":\"cancel\"}\n");emit changed();}}
void DshRuntime::close(){cancel();if(process && !process->waitForFinished(5000)){process->kill();process->waitForFinished(2000);}}
void DshRuntime::start(bool install) {
  if(process)return;
  auto p=new QProcess(this);U::hideConsole(p);process=p;cancelled=false;m_ready=m_failed=false;m_progress=0;m_status=install?"正在下载 dsh":"正在检查 dsh";m_hint.clear();emit changed();
  auto env=U::environment(b->store.data["agent"].toMap());
  for(const auto &proxy:QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(QUrl("https://registry.npmjs.org/"))))if(proxy.type()==QNetworkProxy::HttpProxy && !proxy.hostName().isEmpty()){QUrl url;url.setScheme("http");url.setHost(proxy.hostName());url.setPort(proxy.port());env.insert("UMEKO_UPDATE_PROXY",url.toString());break;}
  p->setProcessEnvironment(env);p->setProgram(b->providers->resolvePython());p->setArguments({"-B",b->root+"/model-runtime/dsh.py"});
  const auto input=helperInput(b,install?"download":"check");
  connect(p,&QProcess::started,this,[p,input]{p->write(QJsonDocument::fromVariant(input).toJson(QJsonDocument::Compact)+'\n');});
  auto buffer=std::make_shared<QByteArray>();auto result=std::make_shared<QVariantMap>();
  connect(p,&QProcess::readyReadStandardError,p,[p]{p->readAllStandardError();});
  connect(p,&QProcess::readyReadStandardOutput,this,[this,p,buffer,result]{
    *buffer+=p->readAllStandardOutput();if(buffer->size()>1048576){p->kill();return;}
    while(buffer->contains('\n')){auto pos=buffer->indexOf('\n');auto row=QJsonDocument::fromJson(buffer->left(pos)).object().toVariantMap();buffer->remove(0,pos+1);
      if(row["type"]=="result")*result=row;
      else if(row["type"]=="progress"){m_progress=qBound(0.,row["progress"].toDouble(),1.);if(!cancelled)m_status=row["message"].toString();emit changed();}
    }
  });
  const auto finish=[this,p,result,install]{if(process!=p)return;process=nullptr;m_ready=result->value("ok").toBool()&&!cancelled;m_failed=!m_ready&&!cancelled;
    m_status=cancelled?"已取消":result->value("message","dsh 操作失败").toString();m_hint=result->value("hint").toString();
    if(m_ready){m_path=(*result)["path"].toString();m_version=(*result)["version"].toString();m_progress=1;if(install){auto cfg=b->store.data["dsh"].toMap();cfg["dshPath"]="";b->store.data["dsh"]=cfg;b->store.save();}}
    p->deleteLater();emit changed();};
  connect(p,&QProcess::finished,this,finish);connect(p,&QProcess::errorOccurred,this,[finish](QProcess::ProcessError error){if(error==QProcess::FailedToStart)finish();});p->start();
}

bool Backend::dshOwnConfig() const {return view->dshOwnFixed?view->dshOwn:dshRuntime->useOwnConfig();}
bool Backend::setSessionBackend(QString value) {
  if(!canChooseBackend() || !QStringList{"pi","dsh"}.contains(value))return false;
  if(view->backend==value)return true;retire(view);view->backend=value;view->dshOptions.clear();emit chatChanged();emit changed();emit reasoningChanged();emit reasoningDetectionChanged();return true;
}
QVariantList Backend::dshModelProviders() const {
  QVariantList out;int i=0;
  for(const auto &value:modelOption(view)["options"].toList()) {const auto group=value.toMap();QVariantList models;
    for(const auto &choice:choices(group.contains("options")?group["options"].toList():QVariantList{group})){auto c=choice.toMap();if(c["value"].typeId()==QMetaType::QString)models<<QVariantMap{{"alias",c.value("name",c["value"])},{"upstream",""},{"value",c["value"]}};}
    if(!models.isEmpty())out<<QVariantMap{{"id",QString("dsh-native-%1").arg(i++)},{"name",group.contains("options")?group.value("name","DeepSeek Harness"):QVariant("DeepSeek Harness")},{"models",models}};
  }return out;
}
QString Backend::dshCurrentModelValue() const {return modelOption(view)["currentValue"].toString();}
QString Backend::dshProviderName() const {for(const auto &g:dshModelProviders())for(const auto &m:g.toMap()["models"].toList())if(m.toMap()["value"]==dshCurrentModelValue())return g.toMap()["name"].toString();return "DeepSeek Harness";}
QString Backend::dshModelName() const {for(const auto &g:dshModelProviders())for(const auto &m:g.toMap()["models"].toList())if(m.toMap()["value"]==dshCurrentModelValue())return m.toMap()["alias"].toString();return "默认模型";}
bool Backend::selectDshModel(QString value) {
  if(view->backend!="dsh" || !dshOwnConfig() || view->busy() || !view->ready)return false;
  for(const auto &g:dshModelProviders())for(const auto &m:g.toMap()["models"].toList())if(m.toMap()["value"]==value){writePi(view,{{"type","set_config"},{"configId",modelOption(view)["id"]},{"value",value}});return true;}return false;
}
QVariantList Backend::dshSelection(std::shared_ptr<Session> s) const {
  QVariantList out;
  for(const auto &value:s->dshOptions) {const auto o=value.toMap();const bool model=o["id"]=="model" || o["category"]=="model";const auto target=model?s->model:s->reasoningEffort;
    if((model && s->dshOwn) || (!model && o["id"]!="reasoning_effort"))continue;
    for(const auto &choice:choices(o["options"].toList())){const auto c=choice.toMap();const auto parsed=QJsonDocument::fromJson(c["value"].toString().toUtf8()).array();if(c["value"]==target || (model && parsed.size()==2 && parsed[0].toString()==s->provider && parsed[1].toString()==target)){out<<QVariantMap{{"configId",o["id"]},{"value",c["value"]}};break;}}
  }return out;
}
void Backend::switchDshConfig(std::shared_ptr<Session> s) {
  if(s->ready && !s->busy()){for(const auto &value:dshSelection(s)){auto option=value.toMap();option["type"]="set_config";writePi(s,option);}}
  else if(s->process)s->stale=true;
  saveDsh(s);
}
void Backend::saveDsh(std::shared_ptr<Session> s) {
  if(s->id.isEmpty())return;indexSession(s);saveSessions();dshDirty.insert(s->key);if(!dshSaveTimer.isActive())dshSaveTimer.start();
}
void Backend::flushDsh() {
  if(dshDirty.isEmpty())return;
  if(dshWriter.isRunning()){dshSaveTimer.start();return;}
  QList<QPair<QString,QVariantMap>> snapshots;
  for(const auto &key:dshDirty){auto s=records.value(key);if(!s)continue;
    snapshots.append({store.directory+"/dsh/transcripts/"+s->id+".json",{{"messages",s->messages},{"contextUsage",s->usage},{"configOptions",s->dshOptions},{"interruptionPending",s->dshInterrupted}}});
  }
  dshDirty.clear();
  const auto failed=std::make_shared<bool>(false);
  auto watcher=new QFutureWatcher<void>(this);
  connect(watcher,&QFutureWatcher<void>::finished,this,[this,watcher,failed]{watcher->deleteLater();if(*failed && !closing)emit toast("DeepSeek Harness 显示历史保存失败，请检查磁盘空间和权限");});
  dshWriter=QtConcurrent::run([snapshots,failed]{for(const auto &item:snapshots)if(!U::writeJson(item.first,item.second))*failed=true;});
  watcher->setFuture(dshWriter);
}
bool Backend::commandDsh(std::shared_ptr<Session> s,QString action,QVariantMap values) {
  if(action=="inspect")action="open";
  if(!QStringList{"open","prompt"}.contains(action)){emit toast("此功能仅适用于 Pi 会话");return false;}
  if(s->busy() || closing)return false;
  s->action=action;s->failed=s->cancelled=false;s->phase="running";s->status="正在连接 DeepSeek Harness";
  s->promptStarted=action=="prompt"?std::optional(std::chrono::steady_clock::now()):std::nullopt;
  if(s->process && s->ready && !s->stale){values["type"]=action;values["config"]=dshSelection(s);writePi(s,values);emit chatChanged();return true;}
  if(s->process)retire(s);
  s->dshOwn=s->dshOwnFixed?s->dshOwn:dshRuntime->useOwnConfig();s->dshOwnFixed=true;
  if(!gatewayPort && !s->dshOwn){event(s,{{"type","error"},{"message","Umeko 内部 API 代理不可用，请重新启动 Umeko 后重试。"}});dshEvent(s,{{"type","dsh_done"},{"stopReason","error"}});return false;}
  auto input=helperInput(this,"prepare");const auto home=store.directory+"/dsh/profiles/"+s->key;
  input.insert({{"profile",store.directory},{"home",s->dshHome.isEmpty()?home:s->dshHome},{"own",s->dshOwn},{"providers",config()["providers"]},{"provider",s->provider},{"model",s->model},{"maxTokens",store.data["agent"].toMap().value("maxTokens",8192)},{"permissionMode",store.data["agent"].toMap().value("toolPermissionMode","auto")},{"proxyUrl",QString("http://127.0.0.1:%1/v1").arg(gatewayPort)},{"workspace",s->workspace.isEmpty()?store.directory+"/workspace":s->workspace}});
  auto p=new QProcess(this);U::hideConsole(p);s->dshPreparing=p;p->setProcessEnvironment(dshEnvironment(this,s->dshOwn));p->setProgram(providers->resolvePython());p->setArguments({"-B",root+"/model-runtime/dsh.py"});
  connect(p,&QProcess::started,this,[p,input]{p->write(QJsonDocument::fromVariant(input).toJson(QJsonDocument::Compact)+'\n');});
  auto buffer=std::make_shared<QByteArray>();connect(p,&QProcess::readyReadStandardOutput,p,[p,buffer]{*buffer+=p->readAllStandardOutput();if(buffer->size()>1048576)p->kill();});connect(p,&QProcess::readyReadStandardError,p,[p]{p->readAllStandardError();});
  const auto finish=[this,s,p,buffer,values,action]{if(s->dshPreparing!=p){p->deleteLater();return;}s->dshPreparing=nullptr;p->deleteLater();if(closing)return;
    const auto result=QJsonDocument::fromJson(buffer->trimmed()).object().toVariantMap();
    if(!result["ok"].toBool() || s->cancelled){if(!s->cancelled)event(s,{{"type","error"},{"message",result.value("message","DeepSeek Harness 无法准备运行时").toString()+"\n"+result["hint"].toString()}});dshEvent(s,{{"type","dsh_done"},{"stopReason",s->cancelled?"cancelled":"error"}});return;}
    s->dshHome=result["home"].toString();auto payload=values;
    payload.insert({{"type","start"},{"runtime",result["path"]},{"workspace",s->workspace.isEmpty()?store.directory+"/workspace":s->workspace},{"sessionId",s->acpSessionId},{"action",action},{"config",QVariantList{}},{"useOwnConfig",s->dshOwn},{"permissionMode",store.data["agent"].toMap().value("toolPermissionMode","auto")}});
    if(s->dshOwn)payload["patchFiles"]=QStringList{result["overlay"].toString()};
    else payload.insert({{"home",s->dshHome},{"model",QVariantMap{{"provider",s->provider},{"id",s->model}}},{"reasoningEffort",s->reasoningEffort}});
    auto child=new QProcess(this);U::hideConsole(child);s->process=child;s->buffer.clear();s->ready=false;s->stale=false;
    auto env=dshEnvironment(this,s->dshOwn);if(!s->dshOwn)env.insert("UMEKO_DSH_PROXY_TOKEN",privateToken);child->setProcessEnvironment(env);child->setProgram(U::node());child->setArguments({root+"/runtime/dsh-acp-bridge.mjs"});
    connect(child,&QProcess::started,this,[this,s,payload]{writePi(s,payload);});
    connect(child,&QProcess::readyReadStandardOutput,this,[this,s,child]{if(s->process!=child || closing)return;s->buffer+=child->readAllStandardOutput();if(s->buffer.size()>16*1024*1024){child->kill();return;}while(s->buffer.contains('\n')){const auto i=s->buffer.indexOf('\n');const auto row=QJsonDocument::fromJson(s->buffer.left(i)).object().toVariantMap();s->buffer.remove(0,i+1);if(!row.isEmpty())event(s,row);}});
    connect(child,&QProcess::readyReadStandardError,child,[child]{child->readAllStandardError();});
    const auto ended=[this,s,child]{if(s->process!=child)return;s->process=nullptr;if(!closing)event(s,{{"type","exit"},{"message","DeepSeek Harness 运行时已退出，请在设置 → Agent 中检查运行时。"}});child->deleteLater();};
    connect(child,&QProcess::finished,this,ended);connect(child,&QProcess::errorOccurred,this,[ended](QProcess::ProcessError error){if(error==QProcess::FailedToStart)ended();});child->start();saveDsh(s);
  };
  connect(p,&QProcess::finished,this,finish);connect(p,&QProcess::errorOccurred,this,[finish](QProcess::ProcessError error){if(error==QProcess::FailedToStart)finish();});p->start();emit chatChanged();return true;
}
void Backend::sendDsh(QString text,bool steering) {
  auto s=view;if(steering){emit toast("DeepSeek Harness 首版不支持引导；运行中请按 Enter 将消息加入队列。");return;}
  if(s->busy() && s->phase!="running")return;
  text=U::attachmentPrompt(text.trimmed(),s->attachments);if(text.isEmpty())return;
  s->attachments.clear();s->draft.clear();emit attachmentsChanged();emit editorText("");
  if(s->busy()){s->queue<<QVariantMap{{"id",U::uuid()},{"text",text},{"mode","followUp"}};emit chatChanged();return;}
  promptDsh(s,text);
}
void Backend::promptDsh(std::shared_ptr<Session> s,QString text) {
  if(s->id.isEmpty()){s->id="dsh-"+s->key;s->dshOwn=dshRuntime->useOwnConfig();s->dshOwnFixed=true;}
  if(s->messages.isEmpty())s->title=text.simplified().left(28);
  s->dshPromptInterrupted=s->dshInterrupted;
  s->messages<<QVariantMap{{"role","user"},{"text",text},{"error",false}}<<QVariantMap{{"role","assistant"},{"text",""},{"error",false}};
  s->optimistic=true;saveDsh(s);
  commandDsh(s,"prompt",{{"text",text},{"interruptionNotice",s->dshInterrupted?"[用户已停止上一条请求；请只处理新消息，除非用户明确要求继续。]":""}});emit chatChanged();
}
bool Backend::dshEvent(std::shared_ptr<Session> s,QVariantMap payload) {
  const auto type=payload["type"].toString();const bool visible=s==view;
  if(type=="dsh_session" || type=="dsh_config"){
    if(payload.contains("sessionId"))s->acpSessionId=payload["sessionId"].toString();s->dshOptions=payload.value("configOptions",s->dshOptions).toList();saveDsh(s);
    if(visible){emit changed();emit reasoningChanged();emit reasoningDetectionChanged();}return true;
  }
  if(type=="dsh_ready"){s->ready=true;s->status=s->action=="prompt"?"正在生成":"";if(visible)emit chatChanged();return true;}
  if(type=="dsh_done" || type=="exit") {
    const auto reason=payload["stopReason"].toString();
    if(reason=="max_tokens")event(s,{{"type","error"},{"message","DeepSeek Harness 达到本轮输出上限，请缩短任务或调整输出上限后再发送。"}});
    const bool stopped=s->cancelled || reason=="cancelled";s->cancelled=stopped;
    finishTurn(s,s->failed || type=="exit");s->phase="idle";s->promptStarted.reset();s->status.clear();s->approval.clear();if(type=="exit")s->ready=false;
    if(stopped){s->dshInterrupted=true;if(!s->messages.isEmpty() && s->messages.last().toMap()["role"]=="assistant"){auto row=s->messages.last().toMap();row["stopReason"]="aborted";if(row["text"].toString().isEmpty() && row["thinking"].toString().isEmpty() && row["tools"].toList().isEmpty())s->messages.removeLast();else s->messages.last()=row;}
      if(s->messages.isEmpty() || s->messages.last().toMap()["customType"]!="umeko-dsh-interruption")s->messages<<QVariantMap{{"role","status"},{"customType","umeko-dsh-interruption"},{"text","已停止上一条请求"},{"error",false}};
    }else if(s->action=="prompt" && s->dshPromptInterrupted && !s->failed && type!="exit")s->dshInterrupted=false;
    saveDsh(s);
    if(!s->queue.isEmpty() && !stopped && !s->failed && type!="exit"){const auto next=s->queue.takeFirst().toMap()["text"].toString();QTimer::singleShot(0,this,[this,s,next]{if(!closing)promptDsh(s,next);});}
    else if(!s->queue.isEmpty()){QStringList texts;for(const auto &row:s->queue)texts<<row.toMap()["text"].toString();s->queue.clear();restoreQueued(s,texts);}
    if(visible){emit chatChanged();emit transcriptChanged();emit approvalChanged();}else emit sidebarChanged();trimPool();return true;
  }
  return false;
}
