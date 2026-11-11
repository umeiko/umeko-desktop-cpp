#include "conversation_state.hpp"
#include "common.hpp"
#include <QGuiApplication>
#include <QClipboard>
#include <QtConcurrent>
void ConversationState::copy(QString text){QGuiApplication::clipboard()->setText(text);}
void ConversationState::switchTo(QString key){
  if(key==token)return;
  if(!token.isEmpty())viewports[token]={{"row",anchor},{"offset",offset},{"follow",follow}};
  // Metadata is tiny; retained reading positions still have a fixed bound.
  if(viewports.size()>64)viewports.erase(viewports.begin());
  ++generation;pendingFile.clear();token=key;auto saved=viewports.value(key);anchor=saved.value("row",-1).toInt();offset=saved.value("offset",0).toDouble();follow=saved.value("follow",true).toBool();
  loading=false;busy=false;status.clear();error.clear();usage.clear();canCompact=false;model.switchContext(key);emit chatChanged();emit viewChanged();
}
void ConversationState::load(QString path,QString leaf){
  ++generation;pendingFile=path;pendingLeaf=leaf;loading=true;error.clear();emit chatChanged();startLoad();
}
void ConversationState::startLoad(){
  if(reading||pendingFile.isEmpty())return;
  reading=true;const auto path=std::exchange(pendingFile,{});const auto leaf=std::exchange(pendingLeaf,{});const auto current=generation;
  const auto cache=files;const auto bounded=boundedHistory;
  auto watcher=new QFutureWatcher<QVariantMap>(this);
  connect(watcher,&QFutureWatcher<QVariantMap>::finished,this,[this,watcher,current]{
    auto result=watcher->result();watcher->deleteLater();reading=false;if(current!=generation){startLoad();return;}
    loading=false;error=result["error"].toString();auto rows=result["messages"].toList();
    const auto signature=result["signature"].toString();
    if(!signature.isEmpty()&&result["bytes"].toLongLong()<=32*1024*1024){files[signature]=result;fileOrder.removeAll(signature);fileOrder<<signature;}
    qint64 bytes=0;for(auto value:files)bytes+=value["bytes"].toLongLong();
    while(fileOrder.size()>3||bytes>32*1024*1024){auto old=fileOrder.takeFirst();bytes-=files.take(old)["bytes"].toLongLong();}

    model.seed(rows,result["projected"].toList(),follow?qMax(0,int(rows.size())-4):qMax(0,anchor));emit chatChanged();emit viewChanged();emit sourceLoaded(rows,error);startLoad();
  });
  watcher->setFuture(QtConcurrent::run([path,leaf,cache,bounded]{
    if(!QFileInfo(path).isFile())return QVariantMap{{"error","会话文件已不存在"}};
    const QFileInfo info(path);const auto signature=info.canonicalFilePath()+":"+QString::number(info.size())+":"+QString::number(info.lastModified().toMSecsSinceEpoch())+(bounded?":window:":":full:")+leaf;
    if(cache.contains(signature))return cache[signature];
    const auto source=U::preview(path,0,leaf).first;
    const auto rows=bounded?ChatModel::recentCompactionInterval(source):source;
    return QVariantMap{{"messages",rows},{"projected",ChatModel::project(rows)},{"signature",signature},{"bytes",info.size()}};
  }));
}

void ConversationState::update(QVariantList rows){
  if(boundedHistory){
    auto window=ChatModel::recentCompactionInterval(rows);
    if(window.size()<rows.size()){
      const auto first=window.first().toMap()["entryId"].toString();
      if(!model.rows.isEmpty() && model.rows.first().toMap()["entryId"].toString()!=first){
        files.clear();fileOrder.clear();model.switchContext(token+"/"+first);anchor=-1;offset=0;follow=true;emit viewChanged();
      }
    }
    rows=std::move(window);
  }
  model.update(rows);emit chatChanged();
}
