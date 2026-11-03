#include "slash_commands.hpp"
#include "backend.hpp"
SlashCommands::SlashCommands(Backend *backend):QObject(backend),b(backend) {
  connect(b,&Backend::viewChanged,this,&SlashCommands::invalidate);
  connect(b->desktopExtensions,&DesktopPlugins::changed,this,[this]{if(loadedContext!=contextKey())invalidate();else emit changed();});
  connect(b,&Backend::appearanceChanged,this,&SlashCommands::changed);
}
QByteArray SlashCommands::contextKey() const {return QJsonDocument::fromVariant(b->skills->context()).toJson(QJsonDocument::Compact);}
void SlashCommands::invalidate(){loadedContext.clear();skills.clear();emit changed();}
QVariantList SlashCommands::entries() const {
  const bool en=b->language()=="en";
  QVariantList rows{QVariantMap{{"id","compact"},{"group","命令"},{"title","/compact"},{"command","/compact"},{"icon","archive"},{"description",en?"Compact conversation context":"压缩当前会话的上下文"}},
    QVariantMap{{"id","reload"},{"group","命令"},{"title","/reload"},{"command","/reload"},{"icon","refresh-cw"},{"description",en?"Reload Pi resources":"重新加载 Pi 资源"}}};
  for(const auto &value:b->desktopExtensions->sessionPlugins()) {const auto w=value.toMap();rows<<QVariantMap{{"id","widget:"+w["id"].toString()},{"widgetId",w["id"]},{"group","Widgets"},{"title",w["name"]},{"icon",w["iconGlyph"]},{"iconSource",w["iconSource"]},{"description",w["summary"]},{"scope","Widget"}};}
  if(b->skills->enabled() && loadedContext==contextKey())rows<<skills;
  static const auto dictionary=U::readJson(b->root+"/skill-runtime/title-pinyin.json",QVariantMap{}).toMap();
  for(auto &value:rows){auto row=value.toMap();QStringList pronunciation;for(const auto c:row["title"].toString())pronunciation<<dictionary.value(QString(c),QString(c)).toString();row["titlePinyin"]=pronunciation;value=row;}
  return rows;
}
void SlashCommands::refresh() {
  emit changed();
  if(pending || !b->skills->enabled() || (loadedContext==contextKey() && loaded.isValid() && loaded.elapsed()<30000))return;
  pending=true;const auto key=contextKey();auto request=b->skills->context();request["action"]="list";
  U::run(this,U::node(),{b->release+"/skills.mjs"},QJsonDocument::fromVariant(request).toJson(QJsonDocument::Compact),
    [this,key](bool ok,QByteArray raw,QString){pending=false;if(key!=contextKey()){refresh();return;}const auto result=QJsonDocument::fromJson(raw).object();if(!ok || !result["ok"].toBool())return;
      skills.clear();for(const auto &value:result["skills"].toArray()){auto row=value.toObject().toVariantMap();if(!row["attached"].toBool())continue;
        const auto path=row["path"].toString();const auto info=QFileInfo(path);
        skills<<QVariantMap{{"id",row["id"]},{"group","技能"},{"title",info.fileName()=="SKILL.md"?info.dir().dirName():row["name"].toString()},{"path",path},{"icon","skill"},{"description",row["description"]},{"scope",row["scope"]=="project"?(b->language()=="en"?"Project":"项目"):(b->language()=="en"?"Global":"全局")}};}
      loadedContext=key;loaded.start();emit changed();},45000,{},b->store.data["agent"].toMap());
}
bool SlashCommands::choose(QString id) {
  for(const auto &value:entries()){auto row=value.toMap();if(row["id"]!=id)continue;
    if(row["group"]=="Widgets")return b->desktopExtensions->openPage(row["widgetId"].toString());
    if(row["group"]=="技能"){const QFileInfo info(row["path"].toString());if(!info.isFile())return false;return b->addAttachments(QString::fromUtf8(QJsonDocument(QJsonArray{QUrl::fromLocalFile(info.fileName()=="SKILL.md"?info.absolutePath():info.absoluteFilePath()).toString()}).toJson(QJsonDocument::Compact)));}}
  return false;
}
bool SlashCommands::execute(QString text) {
  if(b->busy() || b->kernelBusy()){emit b->toast("请等待当前操作结束");return false;}
  text=text.trimmed();if(text!="/compact" && text!="/reload"){emit b->toast("请选择命令、Widget 或技能");return false;}
  invalidate();return b->command(b->view,text=="/compact"?"compact":"reload",{});
}
