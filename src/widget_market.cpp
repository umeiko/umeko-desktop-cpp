#include "widget_host.hpp"
#include "backend.hpp"
#include <QDesktopServices>
#include <QNetworkProxyFactory>
#include <QtConcurrent>
#include <stdexcept>

namespace {
void requireMarket(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
void removePrepared(QString profile,QString path) {
  const auto parent=QFileInfo(path).absolutePath();
  const auto base=U::cleanPath(profile+"/widget-market/tmp")+"/";
  if(QFileInfo(path).fileName()=="package.zip" && U::cleanPath(parent).startsWith(base))
    (void)QtConcurrent::run([parent]{QDir(parent).removeRecursively();});
}
}
void WidgetHost::discardMarket(QString token) {
  const auto prepared=marketPrepared.take(token);
  removePrepared(b->store.directory,prepared.package["path"].toString());
}
void WidgetHost::market(Task request) {
  const auto operation=request->operation;const auto args=request->arguments.toMap();
  const auto id=args["id"].toString();
  if(operation=="market.importLocal") {reply(request,{{"ok",true}});emit m->importRequested();return;}
  if(operation=="market.browse") {
    auto url=QString("https://github.com/umeiko/umeko-desktop-widgets");
    if(args["target"]=="readme") {
      QVariantMap row;for(const auto &value:marketRows)if(value.toMap()["id"]==id)row=value.toMap();
      requireMarket(!row.isEmpty(),"Refresh the Widget list before opening its README");
      const auto readme=row.value("readme","README.md").toString();
      requireMarket(!readme.isEmpty() && !QDir::isAbsolutePath(readme) && !readme.contains('\\') && !readme.contains(':') && !readme.split('/').contains("..") && !readme.split('/').contains("."),"Invalid README path");
      url+="/blob/"+QString::fromLatin1(QUrl::toPercentEncoding(row["tag"].toString()))+"/widgets/"+id+"/"+QString::fromLatin1(QUrl::toPercentEncoding(readme,"/"));
    }
    requireMarket(QDesktopServices::openUrl(QUrl(url)),"Could not open the system browser");reply(request,{{"ok",true}});return;
  }
  if(QStringList{"market.enable","market.open","market.uninstall"}.contains(operation)) {
    const auto entry=m->entry(id);requireMarket(!entry.isEmpty() && !entry["builtin"].toBool(),"Select an installed external Widget");
    if(operation=="market.uninstall") {requireMarket(m->uninstall(id),"Could not uninstall Widget");reply(request,{{"ok",true},{"result",QVariantMap{{"uninstalled",id}}}});return;}
    if(operation=="market.enable") {m->setEnabled(id,true);m->setSessionEnabled(id,true);requireMarket(m->enabled(id)&&m->sessionEnabled(id),"Could not enable Widget");reply(request,{{"ok",true},{"result",QVariantMap{{"enabled",id}}}});return;}
    requireMarket(m->enabled(id)&&m->sessionEnabled(id)&&!entry["page"].toString().isEmpty(),"Enable this Widget before opening it");
    reply(request,{{"ok",true},{"result",QVariantMap{{"opened",id}}}});m->openPage(id);return;
  }
  if(operation=="market.install" || operation=="market.discard") {
    const auto token=args["token"].toString();const auto prepared=marketPrepared.value(token);
    requireMarket(prepared.owner && prepared.owner->worker==request->worker && prepared.owner->session["token"]==request->session["token"] && prepared.owner->session["workspace"]==request->session["workspace"],"Download confirmation expired");
    if(operation=="market.discard"){discardMarket(token);reply(request,{{"ok",true}});return;}
    requireMarket(!m->busy(),"Plugin management is busy");
    const auto key=m->importZip(QUrl::fromLocalFile(prepared.package["path"].toString()).toString());
    requireMarket(!key.isEmpty(),"Could not start ZIP import");
    marketPrepared.remove(token);marketImports[key]=prepared;marketImportRequests[key]=request;return;
  }
  requireMarket(!marketBusy,"The market is busy; retry shortly");
  marketBusy=true;
  QVariantMap input{{"root",b->root},{"profile",b->store.directory},{"node",U::node()},{"operation",operation},{"arguments",args}};
  for(const auto &proxy:QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(QUrl("https://api.github.com")))) {
    if(proxy.type()!=QNetworkProxy::HttpProxy || proxy.hostName().isEmpty())continue;
    QUrl url;url.setScheme("http");url.setHost(proxy.hostName());url.setPort(proxy.port());input["proxy"]=url.toString();break;
  }
  U::run(this,b->providers->resolvePython(),{"-B",b->root+"/model-runtime/market.py"},QJsonDocument::fromVariant(input).toJson(QJsonDocument::Compact),
    [this,request,operation](bool ok,QByteArray raw,QString){
      marketBusy=false;const auto value=QJsonDocument::fromJson(raw).object().toVariantMap();auto result=value["result"].toMap();
      try {
        owner(request);requireMarket(ok && value["ok"].toBool(),qPrintable(value.value("error","Market worker failed").toString()));
        if(operation=="market.prepare") {
          requireMarket(marketPrepared.size()<16,"Too many pending Widget downloads");
          const auto token=QUuid::createUuid().toString(QUuid::Id128);marketPrepared[token]={request,result};
          reply(request,{{"ok",true},{"result",QVariantMap{{"token",token},{"digestVerified",result["digestVerified"]}}}});
          QTimer::singleShot(600000,this,[this,token]{discardMarket(token);});
          return;
        }
        marketRows=result["rows"].toList();QVariantList rows;
        for(const auto &value:marketRows) {
          auto row=value.toMap();const auto id=row["id"].toString();const auto entry=m->entry(id);const auto version=entry["version"].toString();
          row["installedVersion"]=version;row["builtin"]=entry["builtin"].toBool();
          row["state"]=version.isEmpty()?"not-installed":QVersionNumber::fromString(row["version"].toString())>QVersionNumber::fromString(version)?"update-available":"installed";
          row["enabled"]=!version.isEmpty() && m->enabled(id) && m->sessionEnabled(id);row["sessionEnabled"]=!version.isEmpty() && m->sessionEnabled(id);
          row["running"]=m->workers.contains(id) && m->workers[id]->ready;
          if(!entry.isEmpty())row.insert(m->iconFor(id));else{row["iconGlyph"]="puzzle";row["iconSource"]="";}
          rows<<row;
        }
        result["rows"]=rows;reply(request,{{"ok",true},{"result",result}});
      } catch(const std::exception &error) {
        if(operation=="market.prepare")removePrepared(b->store.directory,result["path"].toString());
        reply(request,{{"ok",false},{"error",QString::fromUtf8(error.what())}});
      }
    },600000);
}
