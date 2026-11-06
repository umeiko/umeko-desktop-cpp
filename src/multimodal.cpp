#include "backend.hpp"
#include <cmath>
#include <stdexcept>

QVariantList Backend::auxiliaryVisionModels() const {
  QVariantList result;
  for (const auto &value : config()["providers"].toList()) {
    const auto p=value.toMap();
    for (const auto &model : p["models"].toList()) {
      const auto m=model.toMap();if(!m["vision"].toBool())continue;
      result << QVariantMap{{"providerId",p["id"]},{"model",m["alias"]},{"modelName",m["upstream"]},
        {"providerName",p["name"]},{"label",p["name"].toString()+" / "+m["alias"].toString()}};
    }
  }
  return result;
}
QVariantMap Backend::auxiliaryVision() const {
  auto value=store.data["auxiliaryVision"].toMap();
  for(const auto &row:auxiliaryVisionModels()) {
    const auto m=row.toMap();if(m["providerId"]==value["providerId"] && m["model"]==value["model"]){
      for(auto it=m.cbegin();it!=m.cend();++it)value[it.key()]=it.value();
      return value;
    }
  }
  value["model"]="";value["providerId"]="";return value;
}
QVariantMap Backend::generationConfig() const {
  QVariantMap result;
  for(const auto &kind:QStringList{"image","speech","video"}) {
    const auto selection=store.data["multimodal"].toMap()[kind].toMap(),p=provider(selection["providerId"].toString());
    if(((p["api"]=="openai" && p["hasKey"].toBool()) || (p["api"]=="plugin" && p["generationCapabilities"].toStringList().contains(kind))) && !selection["model"].toString().isEmpty())
      result[kind]=QVariantMap{{"provider",p["name"]},{"model",selection["model"]}};
  }
  return result;
}
bool Backend::saveAuxiliaryVision(QString raw) {
  const auto previous=store.data;
  try {
    QJsonParseError error;const auto doc=QJsonDocument::fromJson(raw.toUtf8(),&error);
    if(error.error!=QJsonParseError::NoError || !doc.isObject())throw std::runtime_error("Invalid auxiliary vision settings");
    auto value=store.data["auxiliaryVision"].toMap();const auto patch=doc.object().toVariantMap();
    for(auto it=patch.cbegin();it!=patch.cend();++it){
      if(!value.contains(it.key()))throw std::runtime_error("Unknown auxiliary vision setting");
      if(it.key()=="autoAnalyze" || it.key()=="toolEnabled") {if(it.value().typeId()!=QMetaType::Bool)throw std::runtime_error("Invalid vision switch");}
      else if(it.value().typeId()!=QMetaType::QString || it.value().toString().size()>256)throw std::runtime_error("Invalid vision model");
      value[it.key()]=it.value();
    }
    if(value["providerId"].toString().isEmpty()!=value["model"].toString().isEmpty())throw std::runtime_error("Select both a vision provider and model");
    bool valid=value["model"].toString().isEmpty();
    for(const auto &row:auxiliaryVisionModels())if(row.toMap()["providerId"]==value["providerId"] && row.toMap()["model"]==value["model"])valid=true;
    if(!valid)throw std::runtime_error("Select a configured vision model");
    store.data["auxiliaryVision"]=value;if(!store.save())throw std::runtime_error("Could not save settings");
    invalidate();emit changed();return true;
  } catch(const std::exception &e){store.data=previous;emit toast(QString::fromUtf8(e.what()));return false;}
}
bool Backend::saveMultimodalSettings(QString raw) {
  const auto previous=store.data;
  try {
    QJsonParseError error;const auto doc=QJsonDocument::fromJson(raw.toUtf8(),&error);
    if(error.error!=QJsonParseError::NoError || !doc.isObject())throw std::runtime_error("Invalid multimodal settings");
    auto values=Store::defaults()["multimodal"].toMap();const auto saved=store.data["multimodal"].toMap();
    for(auto it=saved.cbegin();it!=saved.cend();++it)if(values.contains(it.key()))values[it.key()]=it.value();
    const auto patch=doc.object().toVariantMap();
    for(auto kind=patch.cbegin();kind!=patch.cend();++kind) {
      if(!values.contains(kind.key()) || kind.value().typeId()!=QMetaType::QVariantMap)throw std::runtime_error("Invalid generation kind");
      auto row=values[kind.key()].toMap();const auto fields=kind.value().toMap();
      for(auto it=fields.cbegin();it!=fields.cend();++it) {
        if(!row.contains(it.key()))throw std::runtime_error("Unknown generation setting");
        if(it.key()=="count" || it.key()=="speed") {
          if(it.value().typeId()==QMetaType::Bool || it.value().typeId()==QMetaType::QString)throw std::runtime_error("Invalid generation number");
          const double n=it.value().toDouble();if(!std::isfinite(n) || (it.key()=="count" ? n<1 || n>10 || std::floor(n)!=n : n<.25 || n>4))throw std::runtime_error("Generation number outside range");
        } else {
          if(it.value().typeId()!=QMetaType::QString || it.value().toString().size()>512 || it.value().toString().contains(QRegularExpression("[\\x00-\\x1f]")))throw std::runtime_error("Invalid generation text");
          const auto text=it.value().toString().trimmed();
          if(it.key()=="format" && !(kind.key()=="image" ? QStringList{"","png","jpeg","webp"} : QStringList{"mp3","opus","aac","flac","wav","pcm"}).contains(text))throw std::runtime_error("Invalid generation format");
          if(kind.key()=="video" && !text.isEmpty() && ((it.key()=="seconds" && !QRegularExpression("^[1-9][0-9]{0,3}$").match(text).hasMatch()) || (it.key()=="size" && !QRegularExpression("^[1-9][0-9]*x[1-9][0-9]*$").match(text).hasMatch())))throw std::runtime_error("Invalid video duration or size");
        }
        row[it.key()]=it.value();
      }
      if(!row["providerId"].toString().isEmpty() && provider(row["providerId"].toString())["api"]!="openai" && !(provider(row["providerId"].toString())["api"]=="plugin" && provider(row["providerId"].toString())["generationCapabilities"].toStringList().contains(kind.key())))throw std::runtime_error("Select a compatible generation provider");
      values[kind.key()]=row;
    }
    store.data["multimodal"]=values;if(!store.save())throw std::runtime_error("Could not save settings");
    configureGateway();invalidate();emit changed();return true;
  } catch(const std::exception &e){store.data=previous;emit toast(QString::fromUtf8(e.what()));return false;}
}
