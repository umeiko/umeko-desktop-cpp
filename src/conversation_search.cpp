#include "conversation_search.hpp"
#include "backend.hpp"
#include <algorithm>

ConversationSearch::ConversationSearch(Backend *backend) : QAbstractListModel(backend),b(backend) {
  outputPump.setSingleShot(true);
  connect(&outputPump,&QTimer::timeout,this,&ConversationSearch::drainOutput);
  connect(b->previewLoader,&SessionPreview::ready,this,[this](int token,bool ok){
    if(token!=jumpToken || pendingJump.isEmpty())return;
    if(!ok){pendingJump.clear();emit jumpReady(-1,{});return;}
    if(b->previewLoader->activate(token,true))finishJump();
    else {pendingJump.clear();emit jumpReady(-1,{});}
  });
}
ConversationSearch::~ConversationSearch(){close();}
QVariantMap ConversationSearch::item(int row) const {
  if(row<0 || row>=flat.size())return {};
  const auto [group,hit]=flat[row];const auto &value=groups[group];
  if(hit<0)return {{"kind","session"},{"sessionId",value.id},{"title",value.title},{"count",value.hits.size()},{"collapsed",value.collapsed}};
  auto result=value.hits[hit].toMap();result["kind"]="match";return result;
}
QVariant ConversationSearch::data(const QModelIndex &index,int role) const {
  return index.isValid() && role==Qt::UserRole+1 ? item(index.row()) : QVariant{};
}
void ConversationSearch::rebuild(){
  beginResetModel();flat.clear();
  for(int i=0;i<groups.size();++i){flat.append({i,-1});if(!groups[i].collapsed)for(int j=0;j<groups[i].hits.size();++j)flat.append({i,j});}
  endResetModel();
}
void ConversationSearch::add(QVariantList items){
  QHash<QString,QVariantList> batches;QStringList order;
  for(const auto &value:items){const auto hit=value.toMap();const auto id=hit["sessionId"].toString();if(!batches.contains(id))order<<id;batches[id]<<hit;}
  for(const auto &id:order){
    int g=0;while(g<groups.size() && groups[g].id!=id)g++;
    if(g==groups.size()){
      const auto title=batches[id].first().toMap()["title"].toString();g=0;
      while(g<groups.size() && QString::compare(groups[g].title,title,Qt::CaseInsensitive)<=0)++g;
      const int row=g==groups.size() ? flat.size() : flat.indexOf({g,-1});
      beginInsertRows({},row,row);
      for(auto &position:flat)if(position.first>=g)++position.first;
      groups.insert(g,{id,title,{},false});flat.insert(row,{g,-1});endInsertRows();
    }
    auto &group=groups[g];const auto start=group.hits.size();const auto values=batches[id];
    int header=flat.indexOf({g,-1});
    if(group.collapsed)group.hits.append(values);
    else{
      const int at=header+1+start;beginInsertRows({},at,at+values.size()-1);
      group.hits.append(values);for(int i=0;i<values.size();++i)flat.insert(at+i,{g,start+i});endInsertRows();
    }
    emit dataChanged(index(header),index(header),{Qt::UserRole+1});m_count+=values.size();
  }
}
void ConversationSearch::toggle(QString id){for(auto &group:groups)if(group.id==id){group.collapsed=!group.collapsed;rebuild();return;}}
void ConversationSearch::collapseAll(){bool expand=!groups.isEmpty() && std::all_of(groups.begin(),groups.end(),[](const Group &g){return g.collapsed;});for(auto &g:groups)g.collapsed=!expand;rebuild();}
bool ConversationSearch::start(){
  if(worker && worker->state()!=QProcess::NotRunning)return true;
  const auto node=U::node(),script=b->root+"/runtime/conversation-search.mjs";
  if(node.isEmpty() || !QFileInfo(script).isFile()){m_error="搜索组件暂不可用";m_busy=false;emit changed();return false;}
  if(worker)worker->deleteLater();
  worker=new QProcess(this);auto process=worker;buffer.clear();
  auto env=QProcessEnvironment::systemEnvironment();env.remove("NODE_OPTIONS");env.remove("NODE_PATH");
  worker->setProcessEnvironment(env);worker->setProgram(node);worker->setArguments({script});
  connect(process,&QProcess::readyReadStandardOutput,this,[this,process]{
    if(process!=worker)return;buffer+=process->readAllStandardOutput();if(!outputPump.isActive())outputPump.start(0);
  });
  connect(process,&QProcess::readyReadStandardError,process,[process]{process->readAllStandardError();});
  connect(process,&QProcess::errorOccurred,this,[this,process](QProcess::ProcessError){if(worker==process && m_busy){m_busy=false;m_error="无法启动本地搜索";emit changed();}});
  connect(process,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,process](int,QProcess::ExitStatus){if(worker==process && m_busy){m_busy=false;m_error="本地搜索进程已退出，请重试";emit changed();}});
  worker->start();return true;
}
void ConversationSearch::write(QVariantMap job){if(worker)worker->write(QJsonDocument::fromVariant(job).toJson(QJsonDocument::Compact)+'\n');}
void ConversationSearch::drainOutput(){
  QElapsedTimer budget;budget.start();int newline,batches=0;
  while(batches<4 && budget.elapsed()<6 && (newline=buffer.indexOf('\n'))>=0){
    const auto line=buffer.left(newline);buffer.remove(0,newline+1);
    const auto event=QJsonDocument::fromJson(line).object().toVariantMap();
    if(event["id"].toInt()==generation)receive(event);++batches;
  }
  if(buffer.contains('\n'))outputPump.start(8);
}
void ConversationSearch::cancel(){++generation;write({{"type","cancel"},{"id",generation}});m_busy=false;m_phase="stopped";emit changed();}
void ConversationSearch::clear(){cancel();m_query.clear();m_error.clear();m_phase.clear();m_count=0;m_skipped=0;m_limit=false;beginResetModel();groups.clear();flat.clear();endResetModel();emit changed();}
void ConversationSearch::search(QString text,QVariantMap options){
  cancel();m_query=text;m_error.clear();m_count=0;m_skipped=0;m_limit=false;
  beginResetModel();groups.clear();flat.clear();endResetModel();
  if(text.trimmed().isEmpty()){m_phase.clear();emit changed();return;}
  m_busy=true;m_phase="preparing";emit changed();if(!start())return;
  QVariantList sources;const auto workspace=b->view->workspace.isEmpty() ? QString() : U::cleanPath(b->view->workspace);
  for(auto value:b->sessionIndex){
    auto session=value.toMap();const auto rawWorkspace=session["workspace"].toString();
    if((rawWorkspace.isEmpty()?QString():U::cleanPath(rawWorkspace))!=workspace)continue;
    if(options["scope"]=="session" && session["id"]!=b->view->id)continue;
    const auto title=session["title"].toString();
    const auto included=options["include"].toString().split(',',Qt::SkipEmptyParts),excluded=options["exclude"].toString().split(',',Qt::SkipEmptyParts);
    const auto matches=[&title](const QString &term){return title.contains(term.trimmed(),Qt::CaseInsensitive);};
    if(!included.isEmpty() && !std::any_of(included.begin(),included.end(),matches))continue;
    if(std::any_of(excluded.begin(),excluded.end(),matches))continue;
    if(auto record=b->findSession(session["id"].toString())){
      session["leaf"]=record->searchLeaf;QVariantList live;
      for(int i=0;i<record->messages.size();++i){
        auto row=record->messages[i].toMap();
        if(!row["entryId"].toString().isEmpty() || row["text"].toString().isEmpty())continue;
        if(row["_searchId"].toString().isEmpty()){row["_searchId"]="live:"+record->key+":"+U::uuid();record->messages[i]=row;}
        live<<QVariantMap{{"entryId",row["_searchId"]},{"role",row["role"]},{"text",row["text"]},{"timestamp",row["timestamp"]},{"sourceIndex",i}};
      }
      session["live"]=live;
    }
    sources<<session;
  }
  // An unsubmitted draft is deliberately not part of the searchable corpus.
  auto rg=b->root+"/runtime/releases/coding-1.3.0/bin/rg";
#ifdef Q_OS_WIN
  rg+=".exe";
#endif
  if(!QFileInfo(rg).isFile()){m_error="未找到内置 ripgrep，请修复客户端安装";m_busy=false;emit changed();return;}
  write({{"type","search"},{"id",generation},{"query",text},{"options",options},{"workspace",workspace},
    {"cache",b->store.directory+"/cache/conversation-search-v1"},{"rg",rg},{"sessions",sources},{"maxResults",20000}});
}
void ConversationSearch::receive(QVariantMap event){
  const auto type=event["type"].toString();
  if(type=="matches")add(event["items"].toList());
  else if(type=="preparing" || type=="searching")m_phase=type;
  else if(type=="source_error")++m_skipped;
  else if(type=="done"){m_busy=false;m_phase="done";m_limit=event["limitHit"].toBool();m_skipped=event["skipped"].toInt();}
  else if(type=="error"){m_busy=false;m_error=event["message"].toString();m_phase="error";}
  emit changed();
}
void ConversationSearch::open(int row){
  const auto hit=item(row);if(hit["kind"]!="match")return;
  if(!hit["file"].toString().isEmpty() && !QFileInfo(hit["file"].toString()).isFile()){
    emit b->toast("原消息已不可用，请刷新搜索");emit jumpReady(-1,{});return;
  }
  pendingJump=hit;const auto id=hit["sessionId"].toString();
  if(b->findSession(id)){finishJump();return;}
  // Only a read-only preview is loaded. No Pi inspect/prompt/navigation command.
  jumpToken=--jumpGeneration;
  b->previewLoader->prepare(id,jumpToken);
}
void ConversationSearch::finishJump(){
  const auto hit=pendingJump;pendingJump.clear();
  auto record=b->findSession(hit["sessionId"].toString());
  if(!record){emit b->toast("原消息已不可用，请刷新搜索");emit jumpReady(-1,{});return;}
  const auto entry=hit["entryId"].toString();
  if(!std::any_of(record->messages.begin(),record->messages.end(),[&entry](const QVariant &m){return m.toMap()["entryId"]==entry || m.toMap()["_searchId"]==entry;})){
    emit b->toast("原消息已不可用，请刷新搜索");emit jumpReady(-1,{});return;
  }
  b->activate(record);const int target=b->chat.rowForEntry(entry);
  if(target>=0)emit jumpReady(target,entry);
  else {emit b->toast("原消息已不可用，请刷新搜索");emit jumpReady(-1,{});}
}
void ConversationSearch::close(){outputPump.stop();if(worker){worker->disconnect(this);write({{"type","cancel"}});worker->closeWriteChannel();if(!worker->waitForFinished(1000)){worker->kill();worker->waitForFinished(1000);}worker->deleteLater();worker=nullptr;}}
