#pragma once
#include "chatmodel.hpp"
#include <functional>

// View state only: execution and transcript ownership stay with the session.
class ConversationState : public QObject {
  Q_OBJECT
  Q_PROPERTY(QObject *messageModel READ messageModel CONSTANT)
  Q_PROPERTY(int messageCount READ messageCount NOTIFY chatChanged)
  Q_PROPERTY(QVariantList displayMessages READ displayMessages NOTIFY chatChanged)
  Q_PROPERTY(QString viewKey MEMBER token NOTIFY viewChanged)
  Q_PROPERTY(double scrollPosition MEMBER offset NOTIFY viewChanged)
  Q_PROPERTY(int scrollAnchor MEMBER anchor NOTIFY viewChanged)
  Q_PROPERTY(bool followTail MEMBER follow NOTIFY viewChanged)
  Q_PROPERTY(bool busy MEMBER busy NOTIFY chatChanged)
  Q_PROPERTY(bool running MEMBER busy NOTIFY chatChanged)
  Q_PROPERTY(QString toolStatus MEMBER status NOTIFY chatChanged)
  Q_PROPERTY(QVariantMap turnStatus READ turnStatus NOTIFY chatChanged)
  Q_PROPERTY(QVariantMap contextUsage MEMBER usage NOTIFY chatChanged)
  Q_PROPERTY(bool compactionAvailable MEMBER canCompact NOTIFY chatChanged)
  Q_PROPERTY(bool activeSession READ activeSession NOTIFY chatChanged)
  Q_PROPERTY(bool loading MEMBER loading NOTIFY chatChanged)
  Q_PROPERTY(QString error MEMBER error NOTIFY chatChanged)
public:
  explicit ConversationState(QObject *parent=nullptr):QObject(parent),model(this) {
    connect(&model,&QAbstractItemModel::rowsInserted,this,[this]{emit chatChanged();});
    connect(&model,&QAbstractItemModel::modelReset,this,[this]{emit chatChanged();});
  }
  ChatModel model;
  QString token,status,error;
  double offset=0;
  int anchor=-1;
  bool follow=true,busy=false,canCompact=false,loading=false,boundedHistory=false;
  QVariantMap usage;
  std::function<void()> compact;
  std::function<bool(QString)> submit;
  QObject *messageModel(){return &model;}
  int messageCount() const{return model.rowCount();}
  QVariantList displayMessages() const{return model.rows;}
  QVariantMap turnStatus() const{return {{"state",busy?"running":"finished"}};}
  bool activeSession() const{return !token.isEmpty();}
  void switchTo(QString key);
  void discardFileCache(){files.clear();fileOrder.clear();}
  void update(QVariantList rows);
  void load(QString file, QString leaf = {});
  Q_INVOKABLE void saveViewport(int row,double y,bool tail){if(row>=0){anchor=row;offset=qMax(0.0,y);follow=tail;}}
  Q_INVOKABLE void copy(QString text);
  Q_INVOKABLE void compactSession(){if(canCompact&&!busy&&compact)compact();}
  Q_INVOKABLE bool send(QString text){return submit&&submit(text);}
signals:
  void chatChanged();
  void viewChanged();
  void transcriptChanged();
  void sourceLoaded(QVariantList messages, QString error);
private:
  void startLoad();
  bool reading=false;
  QString pendingFile, pendingLeaf;
  quint64 generation=0;
  QMap<QString,QVariantMap> files;
  QStringList fileOrder;
  QHash<QString,QVariantMap> viewports;
};
