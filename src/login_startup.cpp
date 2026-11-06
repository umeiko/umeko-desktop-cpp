#include "login_startup.hpp"
#include "backend.hpp"
LoginStartupSettings::LoginStartupSettings(Backend *backend):QObject(backend),b(backend) {
  available=QFileInfo(b->root+"/build-info.json").isFile() && !QCoreApplication::arguments().contains("--data-dir");
  if(!available)current["reason"]=b->language()=="en"?"Login startup is unavailable in source or isolated previews.":"源码运行和隔离验收配置不修改开机启动项。";
}
void LoginStartupSettings::refresh(){if(available)invoke("read");else emit changed();}
void LoginStartupSettings::setEnabled(bool umeko,bool proxy){if(available)invoke("set",umeko,proxy);}
void LoginStartupSettings::invoke(QString operation,bool umeko,bool proxy) {
  if(busy)return;
  busy=true;
  const QVariantMap input{{"operation",operation},{"executable",QCoreApplication::applicationFilePath()},{"umeko",umeko},{"proxy",proxy}};
  U::run(this,b->providerPlugins()->property("pythonExecutable").toString(),{"-B",b->root+"/model-runtime/login.py"},QJsonDocument::fromVariant(input).toJson(QJsonDocument::Compact),[this](bool ok,QByteArray raw,QString){
    busy=false;const auto result=QJsonDocument::fromJson(raw).object().toVariantMap();
    if(ok&&result["ok"].toBool())current=result["state"].toMap();
    else {current["reason"]=result.value("error","Could not inspect login startup");emit b->toast(current["reason"].toString());}
    emit changed();
  },15000);
}
