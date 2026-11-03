#include "pet_reminders.hpp"
#include <QSaveFile>
#include <algorithm>
#include <limits>
#include <tuple>
#include <stdexcept>

namespace {
void need(bool ok, QString message) {if(!ok)throw std::runtime_error(message.toStdString());}
QDateTime parse(QVariant value) {
  need(value.metaType().id()==QMetaType::QString,"dueAt must be an ISO 8601 time with Z or a UTC offset");
  auto text=value.toString().toUpper();
  const auto match=QRegularExpression("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}(?::\\d{2}(?:\\.\\d{1,6})?)?(?:Z|([+-])(\\d{2}):(\\d{2}))$").match(text);
  need(match.hasMatch() && text.mid(11,2).toInt()<24 && text.mid(14,2).toInt()<60 && ((text.size()>16 ? text.at(16) : QChar{})!=QChar(':') || text.mid(17,2).toInt()<60) && (match.captured(1).isEmpty() || (match.captured(2).toInt()<=23 && match.captured(3).toInt()<=59)),"Invalid ISO 8601 date or UTC offset");
  // QDateTime accepts submillisecond inputs but schedules at Qt's millisecond precision.
  auto date=QDateTime::fromString(text,Qt::ISODateWithMs);
  need(date.isValid(),"Invalid ISO 8601 date or UTC offset");return date.toUTC();
}
QString stamp(QDateTime value) {
  value=value.toUTC();const auto base=value.toString("yyyy-MM-dd'T'HH:mm:ss");
  return base+(value.time().msec() ? QString(".%1").arg(value.time().msec(),3,10,QChar('0'))+"000Z" : QString("Z"));
}
QString identifier(QVariant value) {
  const auto text=value.toString();
  need(value.metaType().id()==QMetaType::QString && !QUuid(text).isNull() && QUuid(text).toString(QUuid::WithoutBraces)==text,"id must be a UUID string");return text;
}
QString textValue(QVariant value,int limit,bool required=false) {
  need(value.metaType().id()==QMetaType::QString,"Reminder text must be a string");
  auto text=required ? value.toString().trimmed() : value.toString();
  need(text.size()<=limit && (!required || !text.isEmpty()),QString("Reminder text must contain %1–%2 characters").arg(required?1:0).arg(limit));return text;
}
bool pending(QVariantMap item) {return !item["completed"].toBool() && !item["dueAt"].isNull() && item["firedOccurrence"]!=item["occurrence"];}
void stored(QVariantMap item) {
  const QStringList fields{"id","title","notes","dueAt","completed","createdAt","updatedAt","revision","occurrence","firedOccurrence","processedAt","notificationOutcome"};
  need(item.size()==fields.size(),"Invalid reminder fields");for(const auto &name:fields)need(item.contains(name),"Invalid reminder fields");
  identifier(item["id"]);need(textValue(item["title"],200,true)==item["title"],"Invalid stored title");textValue(item["notes"],2000);
  parse(item["createdAt"]);parse(item["updatedAt"]);if(!item["dueAt"].isNull())parse(item["dueAt"]);
  need(item["completed"].metaType().id()==QMetaType::Bool,"Invalid completed flag");
  for(const auto &key:{QString("revision"),QString("occurrence")}) {
    const auto value=item[key];need((value.metaType().id()==QMetaType::Int || value.metaType().id()==QMetaType::LongLong || value.metaType().id()==QMetaType::Double) && value.toDouble()==value.toLongLong() && value.toLongLong()>=(key=="revision"?1:0),"Invalid "+key);
  }
  need(item["dueAt"].isNull() || item["occurrence"].toLongLong()>0,"Timed reminder must have an occurrence");
  if(item["firedOccurrence"].isNull())need(item["processedAt"].isNull() && item["notificationOutcome"].isNull(),"Unexpected processed reminder fields");
  else {
    const auto fired=item["firedOccurrence"];
    need(fired.metaType().id()!=QMetaType::Bool && fired.metaType().id()!=QMetaType::QString && fired.toDouble()==fired.toLongLong() && fired.toLongLong()>=1 && fired.toLongLong()<=item["occurrence"].toLongLong(),"Invalid firedOccurrence");
    parse(item["processedAt"]);need(QStringList{"claimed","suppressed_hidden","suppressed_preference","presentation_requested"}.contains(item["notificationOutcome"].toString()),"Invalid reminder outcome");
  }
}
}
PetReminders::PetReminders(QString directory,QObject *parent,std::function<QDateTime()> clock)
    : QObject(parent),directory(directory),clock(clock) {
  timer.setSingleShot(true);timer.setTimerType(Qt::CoarseTimer);
  connect(&timer,&QTimer::timeout,this,&PetReminders::check);load();
}
void PetReminders::load() {
  const auto path=directory+"/reminders.json";if(!QFileInfo::exists(path))return;
  try {
    QFile file(path);need(file.open(QIODevice::ReadOnly) && file.size()<=8*1024*1024,"Cannot read reminders.json, or file exceeds 8 MiB");
    QJsonParseError error;const auto doc=QJsonDocument::fromJson(file.readAll(),&error);const auto value=doc.object().toVariantMap();
    need(error.error==QJsonParseError::NoError && doc.isObject() && value.size()==2 && value["schemaVersion"].metaType().id()!=QMetaType::Bool && value["schemaVersion"].metaType().id()!=QMetaType::QString && value["schemaVersion"].toDouble()==1 && value["items"].metaType().id()==QMetaType::QVariantList,"Unsupported reminders file schema");
    const auto next=value["items"].toList();need(next.size()<=10000,"Reminder count exceeds 10000");QSet<QString> ids;
    for(const auto &row:next) {const auto item=row.toMap();stored(item);const auto id=item["id"].toString();need(!ids.contains(id),"Duplicate reminder id");ids.insert(id);}
    items=next;
  } catch(const std::exception &e) {invalid=true;error="Cannot load reminders.json: "+QString::fromUtf8(e.what());}
}
void PetReminders::commit(QVariantList next) {
  const auto raw=QJsonDocument::fromVariant(QVariantMap{{"schemaVersion",1},{"items",next}}).toJson(QJsonDocument::Indented);
  QSaveFile file(directory+"/reminders.json");
  if(next.size()>10000 || raw.size()>8*1024*1024 || !QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly) || file.write(raw)!=raw.size() || !file.commit()) {
    error="Cannot save reminders.json: "+file.errorString();emit changed();need(false,error);
  }
  items=next;error.clear();paused=false;emit changed();
}
void PetReminders::start(){if(running)return;running=true;check();}
void PetReminders::stop(){running=false;timer.stop();}
QVariantMap PetReminders::view(QVariantMap item) const {
  item["overdue"]=!item["completed"].toBool() && !item["dueAt"].isNull() && parse(item["dueAt"])<=clock();return item;
}
QVariantList PetReminders::ordered() const {
  auto result=items;const auto now=clock();
  auto order=[now](QVariant value){const auto item=value.toMap();const auto date=item["dueAt"].isNull() ? QDateTime{} : parse(item["dueAt"]);
    return std::tuple{!date.isValid()?2:date<=now?0:1,date.isValid()?date.toMSecsSinceEpoch():std::numeric_limits<qint64>::max(),item["createdAt"].toString(),item["id"].toString()};};
  std::stable_sort(result.begin(),result.end(),[&](const auto &a,const auto &b){return order(a)<order(b);});return result;
}
QVariantMap PetReminders::timeInfo() const {return {{"now",stamp(clock())},{"localNow",clock().toLocalTime().toString(Qt::ISODateWithMs)}};}
QVariantMap PetReminders::summary() const {
  int open=0,overdue=0;QDateTime next;const auto now=clock();
  for(const auto &value:items){const auto item=value.toMap();if(!item["completed"].toBool()){++open;if(!item["dueAt"].isNull() && parse(item["dueAt"])<=now)++overdue;}if(pending(item)){const auto date=parse(item["dueAt"]);if(!next.isValid() || date<next)next=date;}}
  return {{"openCount",open},{"overdueCount",overdue},{"nextDueAt",next.isValid()?QVariant(stamp(next)):QVariant{}},{"error",error}};
}
void PetReminders::check() {
  if(!running || invalid || paused || checking)return;
  checking=true;const auto guard=qScopeGuard([this]{checking=false;});timer.stop();
  const auto now=clock();QVariantList dueItems;
  for(const auto &value:ordered())if(pending(value.toMap()) && parse(value.toMap()["dueAt"])<=now)dueItems<<value;
  if(!dueItems.isEmpty()) {
    const bool enabled=notifications(),show=enabled && visible();
    const auto outcome=show?"claimed":enabled?"suppressed_hidden":"suppressed_preference";
    QSet<QString> ids;for(const auto &value:dueItems)ids.insert(value.toMap()["id"].toString());
    auto next=items;
    for(auto &value:next){auto item=value.toMap();if(ids.contains(item["id"].toString())){item["firedOccurrence"]=item["occurrence"];item["processedAt"]=stamp(now);item["notificationOutcome"]=outcome;value=item;}}
    try {
      commit(next);
      if(show && running) {
        next=items;QVariantList requested;
        for(auto &value:next){auto item=value.toMap();for(const auto &old:dueItems)if(item["id"]==old.toMap()["id"] && item["revision"]==old.toMap()["revision"] && item["occurrence"]==old.toMap()["occurrence"] && !item["completed"].toBool()){item["notificationOutcome"]="presentation_requested";value=item;requested<<view(item);}}
        if(!requested.isEmpty()) {
          commit(next);QVariantList current;
          for(const auto &value:ordered())for(const auto &snapshot:requested) {
            const auto row=value.toMap(),old=snapshot.toMap();
            if(row["id"]==old["id"] && row["revision"]==old["revision"] && row["occurrence"]==old["occurrence"] && !row["completed"].toBool())current<<view(row);
          }
          if(running && !current.isEmpty())emit due(current);
        }
      }
    }catch(const std::exception &){paused=true;timer.stop();return;}
  }
  if(!running || paused)return;
  qint64 wait=60000;bool future=false;
  for(const auto &value:items)if(pending(value.toMap())){future=true;wait=qMin(wait,clock().msecsTo(parse(value.toMap()["dueAt"])));}
  if(future)timer.start(int(qBound(qint64(1),wait,qint64(60000))));
}
QVariantMap PetReminders::handle(QString method,QVariantMap args) {
  const QMap<QString,QStringList> schemas{{"listItems",{"status"}},{"getItem",{"id"}},{"createItem",{"title","notes","dueAt"}},
    {"updateItem",{"id","title","notes","dueAt","clearDueAt"}},{"completeItem",{"id"}},{"deleteItem",{"id"}}};
  need(schemas.contains(method),"Unknown reminder method");for(auto it=args.cbegin();it!=args.cend();++it)need(schemas[method].contains(it.key()),"Unknown reminder arguments");
  need(!invalid,error);
  if(method=="listItems") {
    const auto status=args.value("status","open");need(status.metaType().id()==QMetaType::QString && QStringList{"open","completed","all"}.contains(status.toString()),"status must be open, completed or all");
    QVariantList rows;for(const auto &value:ordered())if(status=="all" || value.toMap()["completed"].toBool()==(status=="completed"))rows<<view(value.toMap());
    auto result=timeInfo();result["items"]=rows;return result;
  }
  if(method=="updateItem")need(args.size()>1,"updateItem requires title, notes or dueAt");
  QVariantMap patch;
  if(args.contains("title"))patch["title"]=textValue(args["title"],200,true);
  if(args.contains("notes"))patch["notes"]=textValue(args["notes"],2000);
  if(args.contains("clearDueAt")) {
    need(args["clearDueAt"].metaType().id()==QMetaType::Bool,"clearDueAt must be a boolean");
    if(args["clearDueAt"].toBool()){need(args["dueAt"].isNull(),"clearDueAt cannot be combined with dueAt");patch["dueAt"]=QVariant{};}
    else need(args.contains("title") || args.contains("notes") || args.contains("dueAt"),"updateItem requires an actual editable field");
  }
  if(args.contains("dueAt"))patch["dueAt"]=args["dueAt"].isNull()?QVariant{}:QVariant(stamp(parse(args["dueAt"])));
  auto next=items;QVariantMap item;int index=-1;
  if(method=="createItem") {
    need(patch.contains("title"),"title is required");
    item={{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"title",patch["title"]},{"notes",patch.value("notes","")},{"dueAt",patch["dueAt"]},{"completed",false},
      {"createdAt",stamp(clock())},{"updatedAt",stamp(clock())},{"revision",1},{"occurrence",patch["dueAt"].isNull()?0:1},{"firedOccurrence",QVariant{}},{"processedAt",QVariant{}},{"notificationOutcome",QVariant{}}};next<<item;
  } else {
    const auto id=identifier(args["id"]);for(int i=0;i<next.size();++i)if(next[i].toMap()["id"]==id){index=i;item=next[i].toMap();break;}
    need(index>=0,"Reminder id does not exist");
    if(method=="getItem"){auto result=timeInfo();result["item"]=view(item);return result;}
    if(method=="completeItem")patch["completed"]=true;
    if(method=="deleteItem")next.removeAt(index);
    else {
      bool differs=false;for(auto it=patch.cbegin();it!=patch.cend();++it)differs|=item[it.key()]!=it.value();
      if(!differs)return {{"item",view(item)}};
      if(patch.contains("dueAt") && patch["dueAt"]!=item["dueAt"]){item["occurrence"]=item["occurrence"].toLongLong()+1;item["firedOccurrence"]=QVariant{};item["processedAt"]=QVariant{};item["notificationOutcome"]=QVariant{};}
      for(auto it=patch.cbegin();it!=patch.cend();++it)item[it.key()]=it.value();
      item["revision"]=item["revision"].toLongLong()+1;item["updatedAt"]=stamp(clock());next[index]=item;
    }
  }
  commit(next);check();if(method=="deleteItem")return {{"deletedId",item["id"]}};
  for(const auto &value:items)if(value.toMap()["id"]==item["id"])return {{"item",view(value.toMap())}};
  return {};
}
