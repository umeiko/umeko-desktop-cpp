#include "backend.hpp"
#include <stdexcept>

namespace {
const QStringList bases{"light", "dark", "yanami"};
const QSet<QString> colorNames{
    "bg",          "sidebar",       "surface", "userBubble", "hover",
    "line",        "ink",           "muted",   "accent",     "accentBg",
    "selectionBg", "selectionText", "button",  "buttonText", "danger"};
QVariantMap normalized(QVariantMap value) {
  auto fail = [](const char *text) { throw std::runtime_error(text); };
  const auto version = value["version"];
  if (value["format"].metaType().id() != QMetaType::QString ||
      value["format"] != "umeko-theme" ||
      (version.metaType().id() != QMetaType::Double &&
       version.metaType().id() != QMetaType::Int &&
       version.metaType().id() != QMetaType::LongLong) ||
      version.toDouble() != 1)
    fail("需要 format 为 umeko-theme、version 为 1 的 JSON 主题");
  if (value["id"].metaType().id() != QMetaType::QString ||
      !QRegularExpression("^[a-z0-9][a-z0-9-]{0,47}$")
           .match(value["id"].toString())
           .hasMatch())
    fail("主题 id 只能使用小写字母、数字和短横线，最多 48 位");
  auto name = value["name"].toString().trimmed();
  if (value["name"].metaType().id() != QMetaType::QString || name.isEmpty() ||
      name.size() > 48)
    fail("主题名称应为 1–48 个字符");
  if (!bases.contains(value["base"].toString()))
    fail("base 应为 light、dark 或 yanami");
  if (value["colors"].metaType().id() != QMetaType::QVariantMap)
    fail("需要 colors 配色对象");
  const auto colors = value["colors"].toMap();
  for (auto it = colors.begin(); it != colors.end(); ++it)
    if (!colorNames.contains(it.key()) ||
        it.value().metaType().id() != QMetaType::QString ||
        !QRegularExpression("^#[0-9a-fA-F]{6}$")
             .match(it.value().toString())
             .hasMatch())
      fail("颜色需使用支持的颜色名称及 #RRGGBB 格式");
  return {{"format", "umeko-theme"}, {"version", 1},
          {"id", value["id"]},       {"name", name},
          {"base", value["base"]},   {"colors", colors}};
}
QVariantList library(QVariantList values) {
  QVariantList rows;
  QSet<QString> ids;
  for (auto value : values.mid(0, 32)) {
    try {
      auto row = normalized(value.toMap());
      const auto id = row["id"].toString();
      if (!ids.contains(id)) {
        rows << row;
        ids.insert(id);
      }
    } catch (...) {
    }
  }
  return rows;
}
} // namespace

QVariantList Backend::customThemes() const {
  auto rows = library(store.data["themes"].toList());
  for (auto &value : rows) {
    auto row = value.toMap();
    row["id"] = "custom:" + row["id"].toString();
    value = row;
  }
  return rows;
}
QVariantMap Backend::themeStyle() const {
  for (auto value : customThemes())
    if (value.toMap()["id"] == store.data["theme"])
      return value.toMap();
  return {};
}
bool Backend::importTheme(QString url) {
  const auto previous = store.data;
  try {
    QFile file(U::localPath(url));
    if (!file.open(QIODevice::ReadOnly))
      throw std::runtime_error("无法读取主题文件");
    const auto raw = file.read(65537);
    if (raw.size() > 65536)
      throw std::runtime_error("主题文件不能超过 64 KiB");
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
      throw std::runtime_error("主题 JSON 格式无效");
    const auto item = normalized(doc.object().toVariantMap());
    auto rows = library(store.data["themes"].toList());
    bool found = false;
    for (auto &value : rows)
      if (value.toMap()["id"] == item["id"]) {
        value = item;
        found = true;
      }
    if (!found) {
      if (rows.size() >= 32)
        throw std::runtime_error(
            "最多保存 32 个自定义主题，请先删除不需要的主题");
      rows << item;
    }
    store.data["themes"] = rows;
    store.data["theme"] = "custom:" + item["id"].toString();
    store.data["dark"] = item["base"] == "dark";
    if (!store.save())
      throw std::runtime_error("无法保存主题设置");
    emit changed();
    emit toast("主题已导入并应用");
    return true;
  } catch (const std::exception &error) {
    store.data = previous;
    emit toast("主题导入失败：" + QString::fromUtf8(error.what()));
    return false;
  }
}
void Backend::removeTheme(QString id) {
  auto previous = store.data;
  auto rows = library(store.data["themes"].toList());
  bool found = false;
  for (int i = rows.size() - 1; i >= 0; --i) {
    const auto row = rows[i].toMap();
    if ("custom:" + row["id"].toString() != id)
      continue;
    found = true;
    rows.removeAt(i);
    if (store.data["theme"] == id) {
      store.data["theme"] = row["base"];
      store.data["dark"] = row["base"] == "dark";
    }
  }
  if (!found)
    return;
  store.data["themes"] = rows;
  if (!store.save()) {
    store.data = previous;
    emit toast("无法保存主题设置");
    return;
  }
  emit changed();
}
void Backend::copyThemePrompt() {
  QFile file(root + "/app/theme-prompt.txt");
  if (!file.exists())
    file.setFileName(":/qt/qml/Umeko/App/app/theme-prompt.txt");
  if (file.open(QIODevice::ReadOnly))
    copy(QString::fromUtf8(file.readAll()));
}
