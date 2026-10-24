#include "backend.hpp"
#include <stdexcept>

QVariantList Backend::commonProviders() const {
  const QString bundled = ":/qt/qml/Umeko/App/app/provider-presets.json";
  return U::readJson(QFileInfo::exists(bundled) ? bundled : root + "/app/provider-presets.json", QVariantList{}).toList();
}
bool Backend::addCommonProvider(QString presetId, QString key, QString alias, QString region) {
  try {
    QVariantMap preset;
    for (const auto &value : commonProviders())
      if (value.toMap()["id"] == presetId) preset = value.toMap();
    if (preset.isEmpty()) throw std::runtime_error("请选择常见供应商");
    if (region != "cn" && region != "international")
      throw std::runtime_error("请选择有效的供应商站点");
    if (region == "international") {
      const auto international = preset["international"].toMap();
      if (international.isEmpty()) throw std::runtime_error("该供应商没有国际站配置");
      for (auto it = international.cbegin(); it != international.cend(); ++it) preset[it.key()] = it.value();
    }
    key = key.trimmed(); alias = alias.trimmed();
    if (key.isEmpty()) throw std::runtime_error("请输入 API Key");
    if (alias.size() > 60 || alias.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")))
      throw std::runtime_error("别名最多 60 个字符，不能包含换行或控制字符");
    QSet<QString> names, taken;
    int count = 0;
    for (const auto &value : store.data["providers"].toList()) {
      const auto record = value.toMap(); names.insert(record["name"].toString());
      if (record["presetId"] == presetId && record.value("presetRegion", "cn") == region) ++count;
      for (const auto &model : record["models"].toList()) taken.insert(model.toMap()["alias"].toString());
    }
    const auto baseName = preset["name"].toString();
    auto name = baseName + (alias.isEmpty() ? "" : "(" + alias + ")");
    if (!alias.isEmpty() && names.contains(name))
      throw std::runtime_error("该供应商别名已存在，请换一个别名");
    if (alias.isEmpty() && (count || names.contains(name))) {
      int number = qMax(2, count + 1);
      do {name = baseName + "(" + QString::number(number++) + ")";} while (names.contains(name));
    }
    QVariantList models;
    for (const auto &value : preset["models"].toList()) {
      auto model = value.toMap(); const auto upstream = model["upstream"].toString();
      auto modelAlias = upstream; int number = 2;
      while (taken.contains(modelAlias)) modelAlias = upstream + "-" + QString::number(number++);
      model["alias"] = modelAlias; taken.insert(modelAlias); models << model;
    }
    const auto id = U::uuid();
    if (!saveProvider(id, name, preset["api"].toString(), preset["baseUrl"].toString(), key,
                      QString::fromUtf8(QJsonDocument::fromVariant(models).toJson()))) return false;
    auto rows = store.data["providers"].toList();
    for (auto &value : rows) {
      auto record = value.toMap();
      if (record["id"] != id) continue;
      record["presetId"] = presetId; record["presetRegion"] = region; record["accountAlias"] = alias;
      if (preset.contains("wireApi")) record["wireApi"] = preset["wireApi"];
      value = record;
    }
    store.data["providers"] = rows;
    notify();
    if (!busy()) selectModel(id, models.first().toMap()["alias"].toString());
    return true;
  } catch (const std::exception &error) {
    emit toast(QString::fromUtf8(error.what())); return false;
  }
}
