#include "pi_capabilities.hpp"
#include <QRegularExpression>
#include <stdexcept>

namespace PiCapabilities {
static const QStringList presets{"full","standard","minimal","custom"};
QVariantMap normalize(QVariantMap value, QStringList blocked) {
  if(value.isEmpty() && !blocked.isEmpty()) {
    QVariantMap tools;for(const auto &name:blocked)tools[name]=false;
    value={{"preset","custom"},{"custom",QVariantMap{{"base","full"},{"tools",tools}}}};
  }
  auto custom=value["custom"].toMap();
  auto clean=[](QVariantMap entries,int length) {
    QVariantMap result;
    for(auto it=entries.cbegin();it!=entries.cend() && result.size()<2000;++it)
      if(!it.key().isEmpty() && it.key().size()<=length && it.value().metaType().id()==QMetaType::Bool)result[it.key()]=it.value();
    return result;
  };
  return {{"preset",presets.contains(value["preset"].toString()) ? value["preset"] : QVariant("full")},
    {"custom",QVariantMap{{"base",presets.mid(0,3).contains(custom["base"].toString()) ? custom["base"] : QVariant("full")},
      {"tools",clean(custom["tools"].toMap(),256)},{"plugins",clean(custom["plugins"].toMap(),4096)}}}};
}
QVariantMap effective(QVariantMap value) {
  value=normalize(value);
  return value["preset"]=="custom" ? value["custom"].toMap() : QVariantMap{{"base",value["preset"]},{"tools",QVariantMap{}},{"plugins",QVariantMap{}}};
}
bool pluginEnabled(QVariantMap value, QString source, bool fallback) {
  const auto profile=effective(value),plugins=profile["plugins"].toMap();
  return plugins.contains(source) ? plugins[source].toBool() : source=="native" || (profile["base"]=="full" && fallback);
}
bool toolEnabled(QVariantMap value,QVariantMap tool,bool fallback) {
  const auto source=tool.value("owner",tool.value("source","native")).toString();
  const auto profile=effective(value),tools=profile["tools"].toMap();const auto name=tool["name"].toString();
  return pluginEnabled(value,source,fallback) && (tools.contains(name) ? tools[name].toBool() : source!="native" || profile["base"]!="minimal" || QStringList{"read","bash","edit","write"}.contains(name));
}
QVariantList catalog(QVariantMap resources) {
  QMap<QString,QVariantMap> tools;
  for(const auto &name:core)tools[name]={{"name",name},{"description",""},{"owner","native"},{"source","builtin:"+name}};
  for(const auto &value:resources.value("toolCatalog",resources["allTools"]).toList()) {
    auto row=value.toMap();const auto name=row["name"].toString(),source=row.value("source","Pi").toString();
    if(name.isEmpty() || name.size()>256)continue;
    if(row["owner"].toString().isEmpty())row["owner"]=core.contains(name) && QStringList{"builtin:"+name,"builtin","Pi","native"}.contains(source) ? "native" : name.startsWith("mcp__") ? "builtin:mcp" : source;
    tools[name]=row;
  }
  QVariantList result;
  // Keep the native tool ordering consistent with the runtime and both editions.
  for(const auto &name:core)result<<tools.take(name);
  for(const auto &row:tools)result<<row;
  return result;
}
QVariantMap mergeResources(QVariantMap previous,QVariantMap current,QVariantMap profile) {
  if(!current.contains("toolCatalog"))return current;
  QMap<QString,QVariantMap> known;
  for(const auto &value:previous["toolCatalog"].toList()) {
    const auto row=value.toMap();if(!pluginEnabled(profile,row.value("owner",row["source"]).toString()))known[row["name"].toString()]=row;
  }
  for(const auto &value:current["toolCatalog"].toList()){const auto row=value.toMap();known[row["name"].toString()]=row;}
  QVariantList rows;for(const auto &row:known)rows<<row;
  current["toolCatalog"]=rows;return current;
}
QVariantMap change(QVariantMap profile,QString action,QVariantMap args,QVariantList extensions,QVariantMap resources) {
  auto result=normalize(profile);
  auto fail=[](const char *message){throw std::runtime_error(message);};
  if(action=="pi.preset") {
    if(!presets.contains(args["preset"].toString()))fail("Invalid Pi preset");
    result["preset"]=args["preset"];return result;
  }
  if(args["enabled"].metaType().id()!=QMetaType::Bool)fail("enabled must be boolean");
  if(result["preset"]!="custom")result["custom"]=QVariantMap{{"base",result["preset"]},{"tools",QVariantMap{}},{"plugins",QVariantMap{}}};
  result["preset"]="custom";auto custom=result["custom"].toMap();
  if(action=="pi.tool") {
    const auto name=args["name"].toString();QVariantMap found;
    for(const auto &row:catalog(resources))if(row.toMap()["name"]==name)found=row.toMap();
    if(name.isEmpty() || name.size()>256 || QRegularExpression("[\\*?\\[\\]!+\\x00-\\x20]").match(name).hasMatch() || found.isEmpty() || found["exposure"]=="hidden")fail("Refresh the current tool catalog first");
    auto tools=custom["tools"].toMap();if(!tools.contains(name) && tools.size()>=2000)fail("Tool configuration limit reached");tools[name]=args["enabled"];custom["tools"]=tools;
  } else if(action=="pi.plugin") {
    const auto id=args["id"].toString();QString source=id=="native" || id==nativeId ? "native" : "";
    for(const auto &row:extensions)if(row.toMap()["id"]==id)source=row.toMap()["path"].toString();
    if(source.isEmpty())fail("Refresh the current extension catalog first");
    auto plugins=custom["plugins"].toMap();if(!plugins.contains(source) && plugins.size()>=2000)fail("Plugin configuration limit reached");plugins[source]=args["enabled"];custom["plugins"]=plugins;
  } else fail("Unknown Pi capability operation");
  result["custom"]=custom;return result;
}
}
