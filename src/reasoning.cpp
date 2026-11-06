#include "backend.hpp"

namespace {
QVariantMap savedModel(const Backend *b,QString provider,QString alias) {
  for(const auto &value:b->provider(provider)["models"].toList())if(value.toMap()["alias"]==alias)return U::normalizeModel(value.toMap());
  return {};
}
QVariantMap signature(QVariantMap model){model.remove("reasoningDetection");model.remove("reasoningFormatDetection");return model;}
QString state(QVariantMap model,QString automatic,QString cache) {
  return model.value(automatic,true).toBool()?model[cache].toMap().value("state","unknown").toString():"manual";
}
}
QVariantMap Backend::modelReasoningSettings(QString provider,QString alias) const {
  auto model=savedModel(this,provider,alias);if(model.isEmpty())return {};
  model["states"]=QVariantMap{{"levels",state(model,"reasoningLevelsAuto","reasoningDetection")},{"format",state(model,"reasoningFormatAuto","reasoningFormatDetection")}};
  return model;
}
QString Backend::reasoningDetectionState() const {
  if(view->backend=="dsh")return view->dshOptions.isEmpty()?"unknown":"known";
  const auto states=modelReasoningSettings(view->provider,view->model)["states"].toMap();
  for(const auto &value:QStringList{"probing","failed","unknown"})if(states.values().contains(value))return value;
  return states.isEmpty()?"unknown":"known";
}
QVariant Backend::reasoningOffSupported() const {
  if(view->backend=="dsh")return {};
  const auto model=savedModel(this,view->provider,view->model);
  return model.value("reasoningFormatAuto",true).toBool()?model["reasoningFormatDetection"].toMap()["offSupported"]:QVariant();
}
QVariantList Backend::reasoningFormatOptions() const {
  QVariantList result;for(const auto &value:QStringList{"openai","openai-none","chat-template","qwen","deepseek","anthropic-adaptive"})result<<QVariantMap{{"value",value},{"label",value}};return result;
}
bool Backend::ensureReasoningDetection() {return view->backend=="pi" && ensureModelReasoningDetection(view->provider,view->model);}
bool Backend::ensureModelReasoningDetection(QString id,QString alias) {
  const auto model=savedModel(this,id,alias);if(model.isEmpty())return false;
  if(state(model,"reasoningLevelsAuto","reasoningDetection")!="unknown" && state(model,"reasoningFormatAuto","reasoningFormatDetection")!="unknown")return true;
  return redetectModelReasoning(id,alias);
}
void Backend::cancelReasoningDetection(QString id) {
  for(auto it=reasoningJobs.begin();it!=reasoningJobs.end();++it)if(it.value() && (id.isEmpty() || it.key().startsWith(id+"/"))) {
    it.value()->setProperty("cancelled",true);it.value()->kill();
  }
}
void Backend::queueReasoningDetection(QString id) {
  for(const auto &value:provider(id)["models"].toList())ensureModelReasoningDetection(id,value.toMap()["alias"].toString());
}
bool Backend::redetectModelReasoning(QString id,QString alias) {
  const auto key=id+"/"+alias;if(reasoningJobs.contains(key))return true;
  const auto account=provider(id);const auto model=savedModel(this,id,alias);
  if(model.isEmpty() || closing || reasoningJobs.size()>=256)return false;
  if(account["api"]!="plugin" && account["baseUrl"].toString().isEmpty())return false;
  if(!model.value("reasoningLevelsAuto",true).toBool() && !model.value("reasoningFormatAuto",true).toBool())return true;
  reasoningJobs[key]=nullptr;
  if(!reasoningQueue.isActive())reasoningQueue.start();
  drainReasoningDetection();return true;
}
void Backend::drainReasoningDetection() {
  if(closing){reasoningQueue.stop();return;}
  int active=0;QSet<QString> providers;
  for(auto it=reasoningJobs.cbegin();it!=reasoningJobs.cend();++it)if(it.value()){++active;providers.insert(it.key().section('/',0,0));}
  for(const auto &key:reasoningJobs.keys()) {
    if(active>=2 || !gatewayPort)break;
    const auto id=key.section('/',0,0),alias=key.section('/',1);
    if(reasoningJobs.value(key) || providers.contains(id))continue;
    bool busy=false;for(const auto &s:records)if(s->provider==id&&(s->busy()||s->subagentsActive))busy=true;
    if(busy)continue;
    if(startReasoningDetection(id,alias)){++active;providers.insert(id);}else reasoningJobs.remove(key);
  }
  bool pending=false;for(auto it=reasoningJobs.cbegin();it!=reasoningJobs.cend();++it)if(!it.value())pending=true;
  if(!pending)reasoningQueue.stop();
}
bool Backend::startReasoningDetection(QString id,QString alias) {
  const auto key=id+"/"+alias;const auto account=provider(id);auto model=savedModel(this,id,alias);
  if(model.isEmpty())return false;
  auto process=new QProcess(this);reasoningJobs[key]=process;U::hideConsole(process);process->setProcessEnvironment(U::environment());
  process->setProperty("cancelled",false);const auto output=std::make_shared<QByteArray>();
  const auto setResult=[this,id,alias](QVariantMap levels,QVariantMap format){
    auto rows=store.data["providers"].toList();for(auto &value:rows) {
      auto p=value.toMap();if(p["id"]!=id)continue;auto models=p["models"].toList();
      for(auto &m:models){auto row=m.toMap();if(row["alias"]!=alias)continue;
        row["reasoningDetection"]=levels;row["reasoningFormatDetection"]=format;m=row;
      }p["models"]=models;value=p;
    }store.data["providers"]=rows;
  };
  const auto updateCache=[=,this](QVariantMap detection){setResult(detection,detection);emit reasoningDetectionChanged();emit changed();};
  updateCache({{"state","probing"}});
  QVariantMap input{{"gateway",QString("http://127.0.0.1:%1/").arg(gatewayPort)},{"token",privateToken},{"provider",id},{"alias",alias},{"model",model},{"protocol",account["wireApi"]=="responses"?"responses":account["api"]=="anthropic"?"anthropic":"openai"}};
  connect(process,&QProcess::started,this,[process,input]{process->write(QJsonDocument::fromVariant(input).toJson(QJsonDocument::Compact));process->closeWriteChannel();});
  connect(process,&QProcess::readyReadStandardOutput,this,[process,output]{*output+=process->readAllStandardOutput();if(output->size()>1024*1024)process->kill();});
  connect(process,&QProcess::readyReadStandardError,this,[process]{process->readAllStandardError();});
  const auto finished=std::make_shared<bool>(false);
  auto finish=[=,this](int exit){
    if(*finished)return;
    *finished=true;reasoningJobs.remove(key);
    if(closing){process->deleteLater();return;}
    if(signature(savedModel(this,id,alias))!=signature(model) || provider(id)["baseUrl"]!=account["baseUrl"]) {
      process->deleteLater();ensureModelReasoningDetection(id,alias);emit reasoningDetectionChanged();return;
    }
    const bool cancelled=process->property("cancelled").toBool();
    const auto response=QJsonDocument::fromJson(*output).object().toVariantMap();const auto result=response["result"].toMap();
    if(!cancelled && exit==0 && response["ok"].toBool())setResult(result["levels"].toMap(),result["format"].toMap());
    else {const QVariantMap failure{{"state",cancelled?"unknown":"failed"},{"error",cancelled?QString():response.value("error","Reasoning detection failed").toString().left(500)}};setResult(failure,failure);}
    if(!cancelled)store.save();
    if(cancelled && !closing)reasoningJobs[key]=nullptr;
    if(!reasoningQueue.isActive())reasoningQueue.start();
    configureGateway();for(const auto &s:records)reconcileReasoning(s);
    emit reasoningDetectionChanged();emit reasoningChanged();emit changed();process->deleteLater();
  };
  connect(process,&QProcess::finished,this,[finish](int code,QProcess::ExitStatus){finish(code);});
  connect(process,&QProcess::errorOccurred,this,[finish](QProcess::ProcessError error){if(error==QProcess::FailedToStart)finish(-1);});
  QTimer::singleShot(190000,process,[process]{if(process->state()!=QProcess::NotRunning)process->kill();});
  process->start(providers->resolvePython(),{"-B",root+"/model-runtime/reasoning.py"});return true;
}
