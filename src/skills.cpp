#include "skills.hpp"
#include "backend.hpp"
#include <QClipboard>
#include <QDesktopServices>
#include <QMimeData>
#include <stdexcept>

Skills::Skills(Backend *backend):QObject(backend),b(backend) {
  connect(b,&Backend::viewChanged,this,[this]{if(token!=b->view->key || workspace!=b->view->workspace)close();});
}
Skills::~Skills(){
  close();
  // close() keeps navigation asynchronous. On final destruction, reap only
  // this service's children so no interpreter outlives its owning profile.
  for(auto process:findChildren<QProcess *>()) {
    disconnect(process,nullptr,this,nullptr);
    if(process->state()!=QProcess::NotRunning){process->kill();process->waitForFinished(1000);}
  }
}
bool Skills::enabled() const { return b->desktopExtensions->skillsEnabled(b->view->key); }

QVariantMap Skills::context() const {
  auto cwd=b->view->workspace.isEmpty()?b->store.directory+"/workspace":b->view->workspace;
  QDir().mkpath(cwd);
  return {{"node",U::node()},{"release",b->release},{"cwd",cwd},{"agentDir",b->agentDir()},
    {"agent",b->store.data["agent"]},{"includeProject",!b->view->workspace.isEmpty()},
    {"sessionSkills",b->view->skillModes},{"loadedSkills",b->view->resources["skills"]},{"skillsEnabled",enabled()}};
}
void Skills::installBundled() {
  if(installLock)return;
  QDir().mkpath(b->agentDir());
  auto lock=std::make_shared<QLockFile>(b->agentDir()+"/.umeko-skills.lock");
  if(!lock->tryLock(0))return;
  installLock=lock;
  U::run(this,b->providers->resolvePython(),{"-B",b->root+"/skill-runtime/worker.py","--install-bundled",b->agentDir()}, {},
    [this,lock](bool ok,QByteArray,QString error){installLock.reset();lock->unlock();if(!ok)emit b->toast("预置 Skills 初始化失败："+error.left(200));},45000);
}
void Skills::close() {
  if(worker){
    auto process=worker.data();worker=nullptr;disconnect(process,nullptr,this,nullptr);
    connect(process,&QProcess::finished,process,&QObject::deleteLater);
    if(process->state()==QProcess::NotRunning)process->deleteLater();else process->kill();
  }
  jobs.clear();order.clear();buffer.clear();token.clear();workspace.clear();
}
QVariantMap Skills::start(QString operation,QVariantMap args) {
  for(const auto &value:jobs){const auto row=value.toMap();if(row["status"]=="running" && row["operation"]!="skills.preview")throw std::runtime_error("Skill manager is busy; retry shortly");}
  if(!worker) {
    token=b->view->key;workspace=b->view->workspace;
    auto p=new QProcess(this);worker=p;U::hideConsole(p);p->setProcessEnvironment(U::environment(b->store.data["agent"].toMap()));
    connect(p,&QProcess::readyReadStandardOutput,this,&Skills::receive);
    connect(p,&QProcess::readyReadStandardError,this,[p]{p->readAllStandardError();});
    auto fail=[this,p]{if(worker!=p)return;for(auto it=jobs.begin();it!=jobs.end();++it){auto j=it.value().toMap();if(j["status"]=="running"){j["status"]="failed";j["result"]=QVariantMap{{"ok",false},{"error","Skill worker stopped; reopen the Widget to retry"}};it.value()=j;}}worker=nullptr;p->deleteLater();};
    connect(p,&QProcess::errorOccurred,this,[fail](QProcess::ProcessError){fail();});
    connect(p,&QProcess::finished,this,[fail](int,QProcess::ExitStatus){fail();});
    p->start(b->providers->resolvePython(),{"-B",b->root+"/skill-runtime/worker.py"});
  }
  if(operation=="skills.paste") {
    auto mime=QGuiApplication::clipboard()->mimeData();const auto urls=mime?mime->urls():QList<QUrl>{};
    if(urls.size()!=1 || !urls.first().isLocalFile())throw std::runtime_error("请先复制一个本地 Skill 文件夹或压缩包");
    args["source"]=urls.first().toLocalFile();
  }
  if(args.contains("source"))args["source"]=U::localPath(args["source"].toString());
  const auto id=U::uuid();jobs[id]=QVariantMap{{"id",id},{"operation",operation},{"args",args},{"status","running"},{"result",QVariantMap{}}};order<<id;
  while(order.size()>128) {if(jobs[order.first()].toMap()["status"]=="running")break;jobs.remove(order.takeFirst());}
  const QVariantMap request{{"id",id},{"operation",operation},{"arguments",args},{"context",context()}};
  worker->write(QJsonDocument::fromVariant(request).toJson(QJsonDocument::Compact)+'\n');
  QTimer::singleShot(90000,this,[this,id]{auto j=jobs[id].toMap();if(j["status"]=="running" && worker)worker->kill();});
  return {{"ok",true},{"accepted",true},{"jobId",id}};
}
void Skills::receive() {
  if(!worker)return;
  buffer+=worker->readAllStandardOutput();
  if(buffer.size()>64*1024*1024){worker->kill();return;}
  while(buffer.contains('\n')) {
    const auto end=buffer.indexOf('\n');const auto response=QJsonDocument::fromJson(buffer.left(end)).object().toVariantMap();buffer.remove(0,end+1);
    const auto id=response["id"].toString();if(!jobs.contains(id))continue;
    auto job=jobs[id].toMap(),result=response["result"].toMap();
    if(token!=b->view->key || workspace!=b->view->workspace)continue;
    const auto op=job["operation"].toString();const auto args=job["args"].toMap();
    if(result["ok"].toBool()) {
      if(op=="skills.attach") {
        if(b->busy() || b->view->subagentsActive)result={{"ok",false},{"error","The chat became busy; retry after it finishes"}};
        else {b->view->skillModes[result["path"].toString()]=result["enabled"];if(!b->view->id.isEmpty())b->setMetadata(b->view->id,{{"skillModes",b->view->skillModes}});b->invalidate();emit b->changed();}
      } else if(op=="skills.open" || op=="skills.copy" || op=="skills.insert") {
        const auto directory=result["directory"].toString();
        if(op=="skills.insert")b->attachChatboxFile(directory);
        else if(op=="skills.copy") {
          if(args["kind"]=="path")QGuiApplication::clipboard()->setText(directory);
          else {auto mime=new QMimeData;mime->setUrls({QUrl::fromLocalFile(directory)});QGuiApplication::clipboard()->setMimeData(mime);}
        } else if(args["mode"]=="preview") emit b->widgetDocumentRequested(directory+"/SKILL.md",QFileInfo(directory).fileName(),"file","skill-manager");
        else if(!QDesktopServices::openUrl(QUrl::fromLocalFile(args["mode"]=="reveal" ? directory : directory+"/SKILL.md")))result={{"ok",false},{"error","Could not open the selected file"}};
      } else if(QStringList{"skills.import","skills.importLocal","skills.paste","skills.rename","skills.delete","skills.default"}.contains(op))b->invalidate();
    }
    if(result["ok"].toBool() && QStringList{"skills.attach","skills.import","skills.importLocal","skills.paste","skills.rename","skills.delete","skills.default"}.contains(op))b->slash->invalidate();
    job["result"]=result;job["status"]=result["ok"].toBool()?"completed":"failed";jobs[id]=job;
  }
}
QVariantMap Skills::job(QString id,int offset,int limit) const {
  auto item=jobs[id].toMap();if(item.isEmpty())throw std::runtime_error("Skill job does not belong to this chat");
  auto result=item["result"].toMap();const bool preview=result["preview"].toMap().contains("skills");auto container=preview?result["preview"].toMap():result;
  if(container.contains("skills")) {
    const auto rows=container["skills"].toList();if(offset<0 || offset>rows.size() || limit<1 || limit>200)throw std::runtime_error("Invalid Skill page");
    QVariantList page;qsizetype bytes=0;
    for(const auto &row:rows.mid(offset,limit)){const auto n=QJsonDocument::fromVariant(row).toJson(QJsonDocument::Compact).size();if(bytes+n>180*1024)break;bytes+=n;page<<row;}
    if(page.isEmpty() && offset<rows.size())throw std::runtime_error("A Skill entry exceeds the page size");
    container["skills"]=page;if(preview)result["preview"]=container;else result=container;
    const auto next=offset+page.size();result["skillsPage"]=QVariantMap{{"offset",offset},{"total",rows.size()},{"nextOffset",next<rows.size()?QVariant(next):QVariant()},{"field",preview?"preview.skills":"skills"}};
  }
  return {{"ok",true},{"jobs",QVariantList{QVariantMap{{"id",id},{"status",item["status"]},{"result",result}}}}};
}
