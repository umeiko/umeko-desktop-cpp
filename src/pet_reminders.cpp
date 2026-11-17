#include "pet_reminders.hpp"
#include <QSaveFile>
#include <QTimeZone>
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
QTimeZone zoneValue(QVariant value) {
  need(value.typeId()==QMetaType::QString && value.toString().size()<=128,"Invalid IANA time zone");
  QTimeZone zone(value.toString().toUtf8());need(zone.isValid(),"Invalid IANA time zone");return zone;
}
void normalizeRepeat(QVariantMap &item, const QVariantMap &patch) {
  if(item["dueAt"].isNull())item["repeat"]="once";
  if(item["repeat"]=="once"){item["timeZone"]="";item["dailyTime"]="";return;}
  if(item["timeZone"].toString().isEmpty())item["timeZone"]=QString::fromUtf8(QTimeZone::systemTimeZoneId());
  auto zone=zoneValue(item["timeZone"]);
  if(item["dailyTime"].toString().isEmpty()||patch.contains("dueAt")||patch.contains("timeZone"))
    item["dailyTime"]=parse(item["dueAt"]).toTimeZone(zone).time().toString("HH:mm:ss");
}
QDateTime nextDaily(QVariantMap item,QDateTime now) {
  const auto zone=zoneValue(item["timeZone"]);const auto previous=parse(item["dueAt"]).toTimeZone(zone);
  auto date=qMax(previous.date().addDays(1),now.toTimeZone(zone).date());
  const auto time=QTime::fromString(item["dailyTime"].toString(),"HH:mm:ss");need(time.isValid(),"Invalid daily time");
  for(int day=0;day<4;++day,date=date.addDays(1)) {
    const QDateTime nominal(date,time,QTimeZone::UTC);QSet<int> offsets;
    for(int hour=-48;hour<=48;++hour)offsets.insert(zone.offsetFromUtc(nominal.addSecs(hour*3600)));
    for(int minute=0;minute<=180;++minute) {
      const auto wall=nominal.addSecs(minute*60);QList<QDateTime> candidates;
      for(const auto offset:offsets) {
        const auto instant=wall.addSecs(-offset),local=instant.toTimeZone(zone);
        if(local.date()==wall.date()&&local.time()==wall.time())candidates<<instant;
      }
      if(!candidates.isEmpty()) {
        std::sort(candidates.begin(),candidates.end());
        for(const auto &candidate:candidates)if(candidate>now)return candidate;
        break;
      }
    }
  }
  need(false,"Cannot find next daily reminder time");return {};
}
void stored(QVariantMap item) {
  const QStringList fields{"id","title","notes","dueAt","completed","createdAt","updatedAt","revision","occurrence","firedOccurrence","processedAt","notificationOutcome","repeat","timeZone","dailyTime","agentId"};
  need(item.size()==fields.size(),"Invalid reminder fields");for(const auto &name:fields)need(item.contains(name),"Invalid reminder fields");
  need(QStringList{"once","daily"}.contains(item["repeat"].toString()) && item["repeat"].typeId()==QMetaType::QString,"Invalid reminder recurrence");
  need(item["agentId"].typeId()==QMetaType::QString && (item["agentId"].toString().isEmpty()||QRegularExpression("^[a-f0-9]{32}$").match(item["agentId"].toString()).hasMatch()),"Invalid reminder Agent");
  if(item["repeat"]=="daily") {zoneValue(item["timeZone"]);need(!item["dueAt"].isNull()&&QTime::fromString(item["dailyTime"].toString(),"HH:mm:ss").isValid(),"Daily reminder requires time");}
  else need(item["timeZone"]==""&&item["dailyTime"]=="","Unexpected daily reminder fields");
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
    parse(item["processedAt"]);need(QStringList{"claimed","suppressed_hidden","suppressed_preference","presentation_requested","restored_overdue"}.contains(item["notificationOutcome"].toString()),"Invalid reminder outcome");
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
    const int version=value["schemaVersion"].toInt();
    need(error.error==QJsonParseError::NoError && doc.isObject() && value["schemaVersion"].typeId()!=QMetaType::Bool && value["schemaVersion"].typeId()!=QMetaType::QString && (version==1||version==2) && value["schemaVersion"].toDouble()==version && value["items"].typeId()==QMetaType::QVariantList && (version==1?value.size()==2:value.size()==4),"Unsupported reminders file schema");
    auto next=value["items"].toList();need(next.size()<=10000,"Reminder count exceeds 10000");QSet<QString> ids;
    for(auto &row:next) {
      auto item=row.toMap();
      if(version==1){need(item.size()==12,"Invalid legacy reminder fields");item["repeat"]="once";item["timeZone"]="";item["dailyTime"]="";item["agentId"]="";}
      stored(item);const auto id=item["id"].toString();need(!ids.contains(id),"Duplicate reminder id");ids.insert(id);row=item;
    }
    if(version==2) {
      need(value["notifications"].typeId()==QMetaType::QVariantList&&value["alerts"].typeId()==QMetaType::QVariantList,"Invalid reminder outbox");
      const auto notifications=value["notifications"].toList(),unread=value["alerts"].toList();need(notifications.size()<=10000&&unread.size()<=10000,"Reminder outbox limit exceeded");
      for(const auto &row:notifications) {
        const auto event=row.toMap();need(event.size()==9,"Invalid reminder notification");identifier(event["id"]);identifier(event["reminderId"]);parse(event["dueAt"]);textValue(event["title"],200,true);textValue(event["notes"],500);
        need(event["occurrence"].toLongLong()>0&&event["revision"].toLongLong()>0&&QStringList{"once","daily"}.contains(event["repeat"].toString()),"Invalid reminder notification occurrence");
        need(event["agentId"].typeId()==QMetaType::QString&&(event["agentId"].toString().isEmpty()||QRegularExpression("^[a-f0-9]{32}$").match(event["agentId"].toString()).hasMatch()),"Invalid notification Agent");
      }
      for(const auto &row:unread)stored(row.toMap());
      outbox=notifications;alerts=unread;
    }
    items=next;
  } catch(const std::exception &e) {invalid=true;error="Cannot load reminders.json: "+QString::fromUtf8(e.what());}
}
void PetReminders::commit(QVariantList next,std::optional<QVariantList> notifications,std::optional<QVariantList> unread) {
  const auto nextOutbox=notifications.value_or(outbox),nextAlerts=unread.value_or(alerts);
  const auto raw=QJsonDocument::fromVariant(QVariantMap{{"schemaVersion",2},{"items",next},{"notifications",nextOutbox},{"alerts",nextAlerts}}).toJson(QJsonDocument::Indented);
  QSaveFile file(directory+"/reminders.json");
  if(next.size()>10000 || nextOutbox.size()>10000 || nextAlerts.size()>10000 || raw.size()>8*1024*1024 || !QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly) || file.write(raw)!=raw.size() || !file.commit()) {
    error="Cannot save reminders.json: "+file.errorString();emit changed();need(false,error);
  }
  items=next;outbox=nextOutbox;alerts=nextAlerts;error.clear();paused=false;emit changed();
}
void PetReminders::acknowledgeNotifications(QStringList ids) {
  auto remaining=outbox;remaining.removeIf([&](const QVariant &value){return ids.contains(value.toMap()["id"].toString());});
  if(remaining.size()!=outbox.size())commit(items,remaining);
}
void PetReminders::markViewed(QString id) {
  auto remaining=alerts;remaining.removeIf([&](const QVariant &value){return id.isEmpty()||value.toMap()["id"]==id;});
  if(remaining.size()!=alerts.size())commit(items,std::nullopt,remaining);
}
void PetReminders::start(){if(running)return;running=true;check();}
void PetReminders::stop(){running=false;timer.stop();}
QVariantMap PetReminders::resolveLocalTime(QVariantMap args) const {
  need(args.keys()==QStringList{"date","time","timeZone"},"Expected date, time and timeZone");
  const auto zone=zoneValue(args["timeZone"]);const auto date=QDate::fromString(args["date"].toString(),"yyyy-MM-dd");const auto time=QTime::fromString(args["time"].toString(),"HH:mm");
  if(!date.isValid()||!time.isValid())return {{"candidates",QVariantList{}}};
  const QDateTime nominal(date,time,QTimeZone::UTC);QSet<int> offsets;QMap<qint64,QVariantMap> candidates;
  for(int hour=-48;hour<=48;++hour)offsets.insert(zone.offsetFromUtc(nominal.addSecs(hour*3600)));
  for(const auto offset:offsets) {
    const auto instant=nominal.addSecs(-offset),local=instant.toTimeZone(zone);
    if(local.date()!=date||local.time()!=time)continue;
    const auto abs=qAbs(offset)/60;
    candidates[instant.toMSecsSinceEpoch()]={{"epoch",instant.toMSecsSinceEpoch()},{"label",QString("UTC%1%2:%3").arg(offset>=0?"+":"-").arg(abs/60,2,10,QChar('0')).arg(abs%60,2,10,QChar('0'))}};
  }
  QVariantList rows;for(const auto &row:candidates)rows<<row;return {{"candidates",rows}};
}
QVariantMap PetReminders::view(QVariantMap item) const {
  if(!item["dueAt"].isNull()) {const auto local=item["repeat"]=="daily"?parse(item["dueAt"]).toTimeZone(zoneValue(item["timeZone"])):parse(item["dueAt"]).toLocalTime();item["localDate"]=local.date().toString("yyyy-MM-dd");item["localTime"]=local.time().toString("HH:mm");}
  item["overdue"]=!item["completed"].toBool() && !item["dueAt"].isNull() && parse(item["dueAt"])<=clock();return item;
}
QVariantList PetReminders::ordered() const {
  auto result=items;const auto now=clock();
  auto order=[now](QVariant value){const auto item=value.toMap();const auto date=item["dueAt"].isNull() ? QDateTime{} : parse(item["dueAt"]);
    return std::tuple{!date.isValid()?2:date<=now?0:1,date.isValid()?date.toMSecsSinceEpoch():std::numeric_limits<qint64>::max(),item["createdAt"].toString(),item["id"].toString()};};
  std::stable_sort(result.begin(),result.end(),[&](const auto &a,const auto &b){return order(a)<order(b);});return result;
}
QVariantMap PetReminders::timeInfo() const {return {{"now",stamp(clock())},{"localNow",clock().toLocalTime().toString(Qt::ISODateWithMs)},{"timeZone",QString::fromUtf8(QTimeZone::systemTimeZoneId())}};}
QVariantMap PetReminders::summary() const {
  int open=0,overdue=0;QDateTime next;const auto now=clock();
  for(const auto &value:items){const auto item=value.toMap();if(!item["completed"].toBool()){++open;if(!item["dueAt"].isNull() && parse(item["dueAt"])<=now)++overdue;}if(pending(item)){const auto date=parse(item["dueAt"]);if(!next.isValid() || date<next)next=date;}}
  return {{"openCount",open},{"overdueCount",overdue},{"nextDueAt",next.isValid()?QVariant(stamp(next)):QVariant{}},{"unreadCount",alerts.size()},{"error",error}};
}
void PetReminders::check() {
  if(!running || invalid || paused || checking)return;
  checking=true;const auto guard=qScopeGuard([this]{checking=false;});timer.stop();
  const auto now=clock();QVariantList dueItems;
  auto recovered=items;bool recoveredDaily=false;
  for(auto &value:recovered) {auto item=value.toMap();if(item["repeat"]=="daily"&&!item["completed"].toBool()&&item["firedOccurrence"]==item["occurrence"]) {item["dueAt"]=stamp(nextDaily(item,now));item["occurrence"]=item["occurrence"].toLongLong()+1;value=item;recoveredDaily=true;}}
  if(recoveredDaily)try{commit(recovered);}catch(const std::exception &){paused=true;return;}
  for(const auto &value:ordered())if(pending(value.toMap()) && parse(value.toMap()["dueAt"])<=now)dueItems<<value;
  if(!dueItems.isEmpty()) {
    const bool enabled=notifications(),show=enabled && visible();
    const auto outcome=show?"claimed":enabled?"suppressed_hidden":"suppressed_preference";
    QSet<QString> ids;for(const auto &value:dueItems)ids.insert(value.toMap()["id"].toString());
    auto next=items,notifications=outbox,unread=alerts;
    for(auto &value:next) {
      auto item=value.toMap();if(!ids.contains(item["id"].toString()))continue;
      if(item["agentId"].toString().isEmpty())item["agentId"]=assistant();
      const auto eventId=QUuid::createUuidV5(QUuid("6ba7b811-9dad-11d1-80b4-00c04fd430c8"),("umeko-reminder:"+item["id"].toString()+":"+item["occurrence"].toString()).toUtf8()).toString(QUuid::WithoutBraces);
      notifications.removeIf([&](const QVariant &row){return row.toMap()["reminderId"]==item["id"];});
      notifications<<QVariantMap{{"id",eventId},{"reminderId",item["id"]},{"occurrence",item["occurrence"]},{"revision",item["revision"]},{"agentId",item["agentId"]},{"title",item["title"]},{"notes",item["notes"].toString().left(500)},{"dueAt",item["dueAt"]},{"repeat",item["repeat"]}};
      item["firedOccurrence"]=item["occurrence"];item["processedAt"]=stamp(now);item["notificationOutcome"]=outcome;value=item;
      unread.removeIf([&](const QVariant &row){return row.toMap()["id"]==item["id"];});unread<<item;
    }
    try {
      commit(next,notifications,unread);
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
      auto advanced=items;bool advance=false;
      for(auto &value:advanced) {auto item=value.toMap();if(ids.contains(item["id"].toString())&&item["repeat"]=="daily"&&!item["completed"].toBool()&&item["firedOccurrence"]==item["occurrence"]) {item["dueAt"]=stamp(nextDaily(item,now));item["occurrence"]=item["occurrence"].toLongLong()+1;value=item;advance=true;}}
      if(advance)commit(advanced);
    }catch(const std::exception &){paused=true;timer.stop();return;}
  }
  if(!running || paused)return;
  qint64 wait=60000;bool future=false;
  for(const auto &value:items)if(pending(value.toMap())){future=true;wait=qMin(wait,clock().msecsTo(parse(value.toMap()["dueAt"])));}
  if(future)timer.start(int(qBound(qint64(1),wait,qint64(60000))));
}
QVariantMap PetReminders::handle(QString method,QVariantMap args) {
  const QMap<QString,QStringList> schemas{{"listItems",{"status"}},{"getItem",{"id"}},{"createItem",{"title","notes","dueAt","repeat","timeZone"}},
    {"updateItem",{"id","title","notes","dueAt","clearDueAt","repeat","timeZone"}},{"completeItem",{"id","completed"}},{"deleteItem",{"id"}},{"clearCompleted",{}}};
  need(schemas.contains(method),"Unknown reminder method");for(auto it=args.cbegin();it!=args.cend();++it)need(schemas[method].contains(it.key()),"Unknown reminder arguments");
  need(!invalid,error);
  if(method=="clearCompleted") {
    auto next=items;QSet<QString> removed;
    next.removeIf([&](const QVariant &value){const auto row=value.toMap();if(!row["completed"].toBool())return false;removed.insert(row["id"].toString());return true;});
    if(!removed.isEmpty()) {
      auto notifications=outbox;notifications.removeIf([&](const QVariant &value){return removed.contains(value.toMap()["reminderId"].toString());});
      commit(next,notifications);
    }
    return {{"deletedCount",removed.size()}};
  }
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
  if(args.contains("repeat")){need(args["repeat"].typeId()==QMetaType::QString&&QStringList{"once","daily"}.contains(args["repeat"].toString()),"repeat must be once or daily");patch["repeat"]=args["repeat"];}
  if(args.contains("timeZone")){zoneValue(args["timeZone"]);patch["timeZone"]=args["timeZone"];}
  auto next=items;QVariantMap item;int index=-1;
  if(method=="createItem") {
    need(patch.contains("title"),"title is required");need(patch.value("repeat","once")!="daily"||!patch.value("dueAt").isNull(),"Daily reminder requires dueAt");
    item={{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"title",patch["title"]},{"notes",patch.value("notes","")},{"dueAt",patch["dueAt"]},{"completed",false},
      {"createdAt",stamp(clock())},{"updatedAt",stamp(clock())},{"revision",1},{"occurrence",patch["dueAt"].isNull()?0:1},{"firedOccurrence",QVariant{}},{"processedAt",QVariant{}},{"notificationOutcome",QVariant{}},{"repeat",patch.value("repeat","once")},{"timeZone",patch.value("timeZone","")},{"dailyTime",""},{"agentId",assistant()}};normalizeRepeat(item,patch);next<<item;
  } else {
    const auto id=identifier(args["id"]);for(int i=0;i<next.size();++i)if(next[i].toMap()["id"]==id){index=i;item=next[i].toMap();break;}
    need(index>=0,"Reminder id does not exist");
    if(method=="getItem"){auto result=timeInfo();result["item"]=view(item);return result;}
    if(method=="completeItem") {
      const auto completed=args.value("completed",true);need(completed.typeId()==QMetaType::Bool,"completed must be a boolean");patch["completed"]=completed;
      // Restore overdue rows quietly; do not replay a missed notification.
      if(!completed.toBool()&&item["completed"].toBool()&&!item["dueAt"].isNull()&&parse(item["dueAt"])<=clock()&&item["firedOccurrence"]!=item["occurrence"]) {
        patch["firedOccurrence"]=item["occurrence"];patch["processedAt"]=stamp(clock());patch["notificationOutcome"]="restored_overdue";
      }
    }
    if(method=="deleteItem")next.removeAt(index);
    else {
      bool differs=false;for(auto it=patch.cbegin();it!=patch.cend();++it)differs|=item[it.key()]!=it.value();
      if(!differs)return {{"item",view(item)}};
      auto updated=item;for(auto it=patch.cbegin();it!=patch.cend();++it)updated[it.key()]=it.value();normalizeRepeat(updated,patch);
      for(const auto key:{QString("repeat"),QString("timeZone"),QString("dailyTime")})patch[key]=updated[key];
      bool rescheduled=false;for(const auto key:{QString("dueAt"),QString("repeat"),QString("timeZone"),QString("dailyTime")})rescheduled|=patch.contains(key)&&patch[key]!=item[key];
      if(rescheduled) {
        // Rescheduling reopens a completed reminder; content-only edits keep it completed.
        if(!updated["dueAt"].isNull())patch["completed"]=false;
        item["occurrence"]=item["occurrence"].toLongLong()+1;item["firedOccurrence"]=QVariant{};item["processedAt"]=QVariant{};item["notificationOutcome"]=QVariant{};
      }
      for(auto it=patch.cbegin();it!=patch.cend();++it)item[it.key()]=it.value();
      item["revision"]=item["revision"].toLongLong()+1;item["updatedAt"]=stamp(clock());next[index]=item;
    }
  }
  auto notifications=outbox;notifications.removeIf([&](const QVariant &row){return row.toMap()["reminderId"]==item["id"];});
  commit(next,notifications);check();if(method=="deleteItem")return {{"deletedId",item["id"]}};
  for(const auto &value:items)if(value.toMap()["id"]==item["id"])return {{"item",view(value.toMap())}};
  return {};
}
