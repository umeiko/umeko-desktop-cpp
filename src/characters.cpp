#include "backend.hpp"
#include "miniz.h"
#include <QImageReader>
#include <stdexcept>

namespace {
const QSet<QString> artworkSlots{
    "welcome",        "avatarSmile",   "avatarWink",     "avatarHappy",
    "reply",          "loading",       "catch",          "push",
    "pull",           "background",    "subagent0",      "subagent1",
    "subagent2",      "subagent3",     "subagentCatch0", "subagentCatch1",
    "subagentCatch2", "subagentCatch3"};
void requireCharacter(bool ok, const char *text) {
  if (!ok)
    throw std::runtime_error(text);
}
const QStringList builtInCharacters{"yanami", "komari", "kaju", "tiara", "whale-girl", "kazuhiko"};
QString unpackCharacterZip(const QString &source, const QString &destination) {
  constexpr quint64 limit = 96 * 1024 * 1024 + 131072;
  QFile input(source);
  requireCharacter(input.open(QIODevice::ReadOnly) &&
                       input.size() <= qint64(limit),
                   "形象 ZIP 超过 96 MiB 或无法读取");
  const auto raw = input.readAll();
  mz_zip_archive zip{};
  requireCharacter(mz_zip_reader_init_mem(&zip, raw.constData(), raw.size(), 0),
                   "ZIP 文件无效");
  const auto cleanup = qScopeGuard([&] { mz_zip_reader_end(&zip); });
  const auto count = mz_zip_reader_get_num_files(&zip);
  requireCharacter(count > 0 && count <= 128, "形象 ZIP 最多包含 128 个条目");
  struct Entry {
    mz_uint index;
    QString name;
    quint64 size;
  };
  QList<Entry> files;
  QMap<QString, bool> names;
  QStringList manifests;
  quint64 total = 0;
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat info{};
    requireCharacter(mz_zip_reader_file_stat(&zip, i, &info), "ZIP 索引无效");
    const auto length = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
    requireCharacter(length > 1 && length < 4096, "ZIP 路径无效");
    QByteArray filename(length, '\0');
    requireCharacter(
        mz_zip_reader_get_filename(&zip, i, filename.data(), length),
        "ZIP 路径无效");
    auto name = QString::fromUtf8(filename.constData(), length - 1);
    const bool directory = name.endsWith('/');
    if (directory)
      name.chop(1);
    bool safe = !name.isEmpty() && !name.contains('\\') && !name.contains(':');
    for (auto c : name)
      safe &= c.unicode() >= 32;
    for (const auto &part : name.split('/'))
      safe &= !part.isEmpty() && part != "." && part != ".." &&
              !part.endsWith(' ') && !part.endsWith('.');
    requireCharacter(safe, "ZIP 包含无效路径");
    const auto kind = (info.m_external_attr >> 16) & 0170000;
    requireCharacter((kind == 0 || kind == 0100000 || kind == 0040000) &&
                         (!kind || (kind == 0040000) == directory),
                     "ZIP 不能包含符号链接或特殊文件");
    const auto key =
        name.normalized(QString::NormalizationForm_C).toCaseFolded();
    requireCharacter(!names.contains(key), "ZIP 包含重复或冲突路径");
    for (auto it = names.cbegin(); it != names.cend(); ++it)
      requireCharacter(!(key.startsWith(it.key() + "/") && !it.value()) &&
                           !(it.key().startsWith(key + "/") && !directory),
                       "ZIP 包含重复或冲突路径");
    names[key] = directory;
    if (directory)
      continue;
    requireCharacter((info.m_method == 0 || info.m_method == 8) &&
                         !(info.m_bit_flag & 1),
                     "ZIP 仅支持未加密的 Stored/Deflate 格式");
    const auto suffix = QFileInfo(name).suffix().toLower();
    requireCharacter(
        QStringList{"json", "png", "jpg", "jpeg", "gif", "webp"}.contains(
            suffix),
        "形象 ZIP 只能包含 JSON 和图片");
    const quint64 maximum = suffix == "json" ? 65536 : 12 * 1024 * 1024;
    requireCharacter(info.m_uncomp_size <= maximum &&
                         info.m_uncomp_size <= limit - total,
                     "ZIP 解压大小超过形象包限制");
    total += info.m_uncomp_size;
    files.append({i, name, info.m_uncomp_size});
    if (name.section('/', -1) == "manifest.json")
      manifests << name;
  }
  requireCharacter(manifests.size() == 1 && manifests.first().count('/') <= 1,
                   "ZIP 需要一个根目录或一级目录中的 manifest.json");
  const auto prefix =
      manifests.first().chopped(QString("manifest.json").size());
  for (const auto &entry : files)
    requireCharacter(entry.name.startsWith(prefix),
                     "ZIP 文件必须位于同一个形象目录");
  for (const auto &entry : files) {
    QByteArray bytes(entry.size, '\0');
    requireCharacter(mz_zip_reader_extract_to_mem(
                         &zip, entry.index, bytes.data(), bytes.size(), 0),
                     "ZIP 数据校验失败");
    const auto path = destination + "/" + entry.name;
    requireCharacter(QDir().mkpath(QFileInfo(path).absolutePath()),
                     "无法创建形象暂存目录");
    QFile output(path);
    requireCharacter(output.open(QIODevice::WriteOnly) &&
                         output.write(bytes) == bytes.size(),
                     "无法解压形象资源");
  }
  return destination + "/" + manifests.first();
}
QVariantMap mergeConfiguration(QVariantMap base, const QVariantMap &patch) {
  for (auto it = patch.cbegin(); it != patch.cend(); ++it) {
    if (it.value().typeId() == QMetaType::QVariantMap)
      base[it.key()] =
          mergeConfiguration(base[it.key()].toMap(), it.value().toMap());
    else if (it.key() == "subagents") {
      auto rows = base[it.key()].toList();
      const auto updates = it.value().toList();
      for (int i = 0; i < updates.size() && i < rows.size(); ++i)
        rows[i] = mergeConfiguration(rows[i].toMap(), updates[i].toMap());
      base[it.key()] = rows;
    } else
      base[it.key()] = it.value();
  }
  return base;
}
QVariantMap normalizeConfiguration(const QVariantMap &patch,
                                   const QVariantMap &schema) {
  QVariantMap result;
  for (auto it = patch.cbegin(); it != patch.cend(); ++it) {
    requireCharacter(schema.contains(it.key()), "未知的角色配置项");
    const auto expected = schema[it.key()];
    if (it.key() == "version") {
      requireCharacter(it.value().typeId() != QMetaType::Bool &&
                           it.value().typeId() != QMetaType::QString &&
                           it.value().toDouble() == 1,
                       "角色配置 version 必须为 1");
      result[it.key()] = 1;
    } else if (expected.typeId() == QMetaType::QVariantMap) {
      requireCharacter(it.value().typeId() == QMetaType::QVariantMap,
                       "角色场景必须为对象");
      result[it.key()] =
          normalizeConfiguration(it.value().toMap(), expected.toMap());
    } else if (it.key() == "subagents") {
      const auto rows = it.value().toList(), defaults = expected.toList();
      requireCharacter(it.value().typeId() == QMetaType::QVariantList &&
                           !rows.isEmpty() && rows.size() <= 4,
                       "subagents 需要 1–4 个角色配置对象");
      QVariantList values;
      for (int i = 0; i < rows.size(); ++i) {
        requireCharacter(rows[i].typeId() == QMetaType::QVariantMap,
                         "子代理配置必须为对象");
        values << normalizeConfiguration(rows[i].toMap(), defaults[i].toMap());
      }
      result[it.key()] = values;
    } else if (expected.typeId() == QMetaType::QVariantList) {
      const auto rows = it.value().toList();
      requireCharacter(it.value().typeId() == QMetaType::QVariantList &&
                           !rows.isEmpty() && rows.size() <= 32,
                       "台词需要 1–32 条短文本");
      for (const auto &text : rows)
        requireCharacter(text.typeId() == QMetaType::QString &&
                             !text.toString().trimmed().isEmpty() &&
                             text.toString().size() <= 500,
                         "台词需要短文本");
      result[it.key()] = rows;
    } else {
      requireCharacter(it.value().typeId() == QMetaType::QString &&
                           it.value().toString().size() <= 500,
                       "角色配置需要短文本");
      if (it.key() == "name")
        requireCharacter(!it.value().toString().trimmed().isEmpty() &&
                             it.value().toString().size() <= 48,
                         "角色名称应为 1–48 个字符");
      result[it.key()] = it.value();
    }
  }
  return result;
}
QVariantMap readCharacter(const QString &file, bool checkImages = false) {
  QFile input(file);
  requireCharacter(input.open(QIODevice::ReadOnly), "无法读取形象清单");
  const auto raw = input.read(65537);
  requireCharacter(raw.size() <= 65536, "形象清单不能超过 64 KiB");
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(raw, &error);
  requireCharacter(error.error == QJsonParseError::NoError &&
                       document.isObject(),
                   "形象 JSON 格式无效");
  const auto item = document.object().toVariantMap();
  requireCharacter(item["format"] == "umeko-character" &&
                       item["version"].typeId() != QMetaType::Bool &&
                       item["version"].typeId() != QMetaType::QString &&
                       item["version"].toDouble() == 1,
                   "需要 format 为 umeko-character、version 为 1 的形象清单");
  requireCharacter(item["id"].typeId() == QMetaType::QString &&
                       QRegularExpression("^[a-z0-9][a-z0-9-]{0,47}$")
                           .match(item["id"].toString())
                           .hasMatch(),
                   "形象 id 只能使用小写字母、数字和短横线，最多 48 位");
  requireCharacter(item["name"].typeId() == QMetaType::QString &&
                       !item["name"].toString().trimmed().isEmpty() &&
                       item["name"].toString().size() <= 48,
                   "形象名称应为 1–48 个字符");
  requireCharacter(!item.contains("assets") ||
                       item["assets"].typeId() == QMetaType::QVariantMap,
                   "形象资源名称无效");
  requireCharacter(!item.contains("lines") ||
                       item["lines"].typeId() == QMetaType::QVariantMap,
                   "形象台词名称无效");
  const auto assets = item["assets"].toMap(), lines = item["lines"].toMap();
  for (auto it = lines.cbegin(); it != lines.cend(); ++it) {
    requireCharacter(
        QStringList{"tap", "celebration", "greeting"}.contains(it.key()),
        "形象台词名称无效");
    const auto values =
        it.key() == "tap" ? it.value().toList() : QVariantList{it.value()};
    requireCharacter(!values.isEmpty() && values.size() <= 16 &&
                         (it.key() != "tap" ||
                          it.value().typeId() == QMetaType::QVariantList),
                     "tap 必须为 1–16 条短文本");
    for (const auto &text : values)
      requireCharacter(text.typeId() == QMetaType::QString &&
                           text.toString().size() <= 240,
                       "形象台词必须为短文本");
  }
  const auto directory = QFileInfo(file).absolutePath();
  qint64 total = 0;
  for (auto it = assets.cbegin(); it != assets.cend(); ++it) {
    requireCharacter(artworkSlots.contains(it.key()) &&
                         it.value().typeId() == QMetaType::QString,
                     "形象资源名称无效");
    const auto relative = it.value().toString();
    const auto parts = relative.split('/');
    requireCharacter(!relative.isEmpty() && !relative.contains('\\') &&
                         !relative.contains(':') && !parts.contains("") &&
                         !parts.contains(".") && !parts.contains(".."),
                     "形象资源必须是包内的正常相对路径");
    requireCharacter(QStringList{"png", "jpg", "jpeg", "webp", "gif"}.contains(
                         QFileInfo(relative).suffix().toLower()),
                     "形象支持 PNG、JPEG、GIF 和 WebP");
    auto path = directory;
    for (const auto &part : parts) {
      path += "/" + part;
      requireCharacter(!QFileInfo(path).isSymLink(),
                       "形象资源不能包含符号链接或指向包外");
    }
    if (checkImages) {
      const auto info = QFileInfo(path);
      QImageReader reader(path);
      const auto size = reader.size();
      requireCharacter(info.isFile() && info.size() <= 12 * 1024 * 1024 &&
                           reader.canRead() && size.width() > 0 &&
                           size.height() > 0 &&
                           qint64(size.width()) * size.height() <= 16777216,
                       "形象图片无法读取或过大");
      total += info.size();
    }
  }
  requireCharacter(total <= 96 * 1024 * 1024, "形象包不能超过 96 MiB");
  QVariantMap output{{"format", "umeko-character"},
                     {"version", 1},
                     {"id", item["id"]},
                     {"name", item["name"].toString().trimmed()},
                     {"assets", assets},
                     {"lines", lines}};
  if (item.contains("dialogue")) {
    const auto relative = item["dialogue"].toString();
    requireCharacter(item["dialogue"].typeId() == QMetaType::QString &&
                         QRegularExpression("^[a-zA-Z0-9_-]+\\.json$")
                             .match(relative)
                             .hasMatch(),
                     "dialogue 需要包内 JSON 文件名");
    QFile file(directory + "/" + relative);
    requireCharacter(!QFileInfo(file).isSymLink() &&
                         file.open(QIODevice::ReadOnly),
                     "无法读取台词 JSON");
    const auto text = file.read(65537);
    const auto document = QJsonDocument::fromJson(text, &error);
    requireCharacter(text.size() <= 65536 &&
                         error.error == QJsonParseError::NoError &&
                         document.isObject(),
                     "台词 JSON 无效或过大");
    output["dialogue"] = relative;
    output["_configuration"] = document.object().toVariantMap();
  }
  return output;
}
QVariantMap defaultCharacter() {
  return {{"id", "yanami"},
          {"name", "八奈见杏菜"},
          {"assets", QVariantMap{}},
          {"lines", QVariantMap{}},
          {"builtin", true}};
}
QVariantList characterLibrary(const QString &root, const QString &directory, bool subagents = false) {
  QVariantList rows{defaultCharacter()};
  const auto append = [&](const QString &file, bool builtIn) {
    try {
      auto row = readCharacter(file);
      if (!builtIn && (builtInCharacters.contains(row["id"].toString()) ||
                       row["id"] != QFileInfo(file).dir().dirName()))
        return;
      row["builtin"] = builtIn;
      row["directory"] = QFileInfo(file).absolutePath();
      row["revision"] = QFileInfo(file).lastModified().toMSecsSinceEpoch();
      rows << row;
    } catch (const std::exception &) {
    }
  };
  auto identifiers = QStringList{"komari", "kaju", "tiara", "whale-girl"};
  if (subagents) identifiers << "kazuhiko";
  for (const auto &id : identifiers) {
    auto file =
        ":/qt/qml/Umeko/App/app/icons/characters/" + id + "/manifest.json";
    if (!QFileInfo::exists(file))
      file = root + "/app/icons/characters/" + id + "/manifest.json";
    append(file, true);
  }
  for (const auto &folder :
       QDir(directory + "/characters")
           .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks))
    if (QRegularExpression("^[a-z0-9][a-z0-9-]{0,47}$")
            .match(folder.fileName())
            .hasMatch() &&
        !QFileInfo(folder.absoluteFilePath() + "/manifest.json").isSymLink())
      append(folder.absoluteFilePath() + "/manifest.json", false);
  return rows;
}
void cleanupRemovedCharacter(QString path, QString marker, int attempt = 0) {
  if (!QFileInfo(path + "/" + marker).isFile() ||
      QFileInfo(path + "/manifest.json").exists() ||
      QFileInfo(path).isSymLink())
    return;
  if (!QDir(path).removeRecursively() && attempt < 5)
    QTimer::singleShot(250, [path, marker, attempt] {
      cleanupRemovedCharacter(path, marker, attempt + 1);
    });
}
void cleanupRetiredCharacter(QString path, int attempt = 0) {
  if (QFileInfo(path).isSymLink() || !QFileInfo::exists(path))
    return;
  if (!QDir(path).removeRecursively() && attempt < 5)
    QTimer::singleShot(
        250, [path, attempt] { cleanupRetiredCharacter(path, attempt + 1); });
}
} // namespace

void Backend::initCharacters() {
  auto file = QString(":/qt/qml/Umeko/App/app/character-defaults.json");
  if (!QFileInfo::exists(file))
    file = root + "/app/character-defaults.json";
  characterDefaults = U::readJson(file, QVariantMap{}).toMap();
  const auto preferences = store.directory + "/character-config.json";
  if (QFileInfo(preferences).size() <= 65536) {
    try {
      characterOptions = normalizeConfiguration(
          U::readJson(preferences, QVariantMap{}).toMap(), characterDefaults);
    } catch (const std::exception &) {
      characterOptions = {};
    }
  }
  refreshCharacters();
}
void Backend::refreshCharacters() {
  resolvedCharacter = defaultCharacter();
  for (const auto &value : characterLibrary(root, store.directory))
    if (value.toMap()["id"] == store.data.value("character", "yanami"))
      resolvedCharacter = value.toMap();
  characterConfig = characterDefaults;
  try {
    const auto configuration = normalizeConfiguration(
        resolvedCharacter["_configuration"].toMap(), characterDefaults);
    for (const auto &value : configuration["subagents"].toList()) {
      const auto row = value.toMap();
      requireCharacter(row["avatar"].toString().isEmpty() &&
                           row["catch"].toString().isEmpty(),
                       "包内头像请放入 assets.subagent0 等资源项");
    }
    characterConfig = mergeConfiguration(characterConfig, configuration);
  } catch (const std::exception &) {
    resolvedCharacter = defaultCharacter();
  }
  const auto legacy = resolvedCharacter["lines"].toMap();
  auto scenes = characterConfig["scenes"].toMap();
  for (const auto &key : QStringList{"welcome", "reply"}) {
    auto row = scenes[key].toMap();
    if (!legacy["tap"].toList().isEmpty())
      row["tap"] = legacy["tap"];
    if (!legacy["celebration"].toString().isEmpty())
      row["celebration"] = legacy["celebration"];
    scenes[key] = row;
  }
  if (!legacy["greeting"].toString().isEmpty()) {
    auto row = scenes["welcome"].toMap();
    row["greetings"] = QVariantList{legacy["greeting"]};
    scenes["welcome"] = row;
  }
  characterConfig["scenes"] = scenes;
  characterConfig = mergeConfiguration(characterConfig, characterOptions);
  QStringList animatedSlots, animatedRoles;
  const auto assets = resolvedCharacter["assets"].toMap();
  for (auto it = assets.cbegin(); it != assets.cend(); ++it) {
    const auto path =
        resolvedCharacter["directory"].toString() + "/" + it.value().toString();
    if (QFileInfo(path).suffix().toLower() == "webp" &&
        QImageReader(path).imageCount() > 1)
      animatedSlots << it.key();
  }
  for (const auto &value : characterConfig["subagents"].toList()) {
    const auto role = value.toMap();
    for (const auto &key : QStringList{"avatar", "catch"}) {
      const auto url = role[key].toString();
      if (!url.isEmpty() &&
          QFileInfo(QUrl(url).path()).suffix().toLower() == "webp" &&
          QImageReader(QUrl(url).toLocalFile()).imageCount() > 1)
        animatedRoles << url;
    }
  }
  resolvedCharacter["_animatedSlots"] = animatedSlots;
  resolvedCharacter["_animatedRoles"] = animatedRoles;
}
QStringList Backend::characterNames() const {
  QStringList names;
  for (const auto &value : characterConfig["subagents"].toList())
    names << value.toMap()["name"].toString();
  return names;
}
QVariantMap Backend::characterConfiguration() const {
  return {{"path", store.directory + "/character-config.json"},
          {"config", characterConfig},
          {"overrides", characterOptions}};
}
bool Backend::configureCharacter(QString raw) {
  try {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(raw.toUtf8(), &error);
    requireCharacter(raw.toUtf8().size() <= 65536 &&
                         error.error == QJsonParseError::NoError &&
                         document.isObject(),
                     "角色配置 JSON 无效或过大");
    auto options = normalizeConfiguration(document.object().toVariantMap(),
                                          characterDefaults);
    auto roles = options["subagents"].toList();
    for (auto &value : roles) {
      auto row = value.toMap();
      for (const auto &kind : QStringList{"avatar", "catch"}) {
        if (row.value(kind).toString().isEmpty())
          continue;
        const auto source = U::localPath(row[kind].toString());
        const auto info = QFileInfo(source);
        QImageReader reader(source);
        const auto size = reader.size();
        requireCharacter(
            info.isAbsolute() && info.isFile() &&
                QStringList{"png", "jpg", "jpeg", "webp", "gif"}.contains(
                    info.suffix().toLower()) &&
                info.size() <= 12 * 1024 * 1024 && reader.canRead() &&
                size.width() > 0 && size.height() > 0 &&
                qint64(size.width()) * size.height() <= 16777216,
            "角色头像无法读取或过大");
        const auto directory = store.directory + "/character-assets";
        requireCharacter(!QFileInfo(directory).isSymLink() &&
                             QDir().mkpath(directory),
                         "无法创建角色资源目录");
        QFile input(source);
        requireCharacter(input.open(QIODevice::ReadOnly), "无法读取角色头像");
        const auto digest =
            QString::fromLatin1(QCryptographicHash::hash(
                                    input.readAll(), QCryptographicHash::Sha256)
                                    .toHex());
        input.close();
        const auto target =
            directory + "/" + digest + "." + info.suffix().toLower();
        requireCharacter(QFileInfo::exists(target) ||
                             QFile::copy(source, target),
                         "无法复制角色头像");
        row[kind] = QUrl::fromLocalFile(target).toString();
      }
      value = row;
    }
    if (options.contains("subagents"))
      options["subagents"] = roles;
    requireCharacter(
        U::writeJson(store.directory + "/character-config.json", options),
        "无法保存角色配置");
    characterOptions = options;
    refreshCharacters();
    emit changed();
    return true;
  } catch (const std::exception &error) {
    emit toast("角色配置失败：" + QString::fromUtf8(error.what()));
    return false;
  }
}
QVariantList Backend::characterPacks() const {
  return characterPacksFor(false);
}
QVariantList Backend::subagentCharacterPacks() const {
  return characterPacksFor(true);
}
QVariantList Backend::characterPacksFor(bool subagents) const {
  QVariantList rows;
  for (const auto &value : characterLibrary(root, store.directory, subagents)) {
    const auto row = value.toMap();
    const auto assets = row["assets"].toMap();
    QString path;
    for (const auto &key : QStringList{"avatarSmile", "reply", "welcome"})
      if (assets.contains(key)) {
        path = row["directory"].toString() + "/" + assets[key].toString();
        break;
      }
    if (path.isEmpty()) {
      path = ":/qt/qml/Umeko/App/app/icons/avatars/smile.webp";
      if (!QFileInfo::exists(path))
        path = root + "/app/icons/avatars/smile.webp";
    }
    auto reply=assets.contains("reply")?row["directory"].toString()+"/"+assets["reply"].toString():row["id"]=="yanami"?QString(":/qt/qml/Umeko/App/app/icons/yanami-pixel.webp"):path;
    if(!QFileInfo::exists(reply))reply=row["id"]=="yanami"?root+"/app/icons/yanami-pixel.webp":path;
    rows << QVariantMap{
        {"id", row["id"]},
        {"name", row["name"]},
        {"builtin", row["builtin"]},
        {"portrait", path.startsWith(':')
                         ? "qrc" + path
                         : QUrl::fromLocalFile(path).toString()},
        {"replyPortrait",reply.startsWith(':')?"qrc"+reply:QUrl::fromLocalFile(reply).toString()}};
  }
  return rows;
}
QVariantMap Backend::characterStyle() const {
  const auto selected = resolvedCharacter;
  QVariantMap resolved;
  const auto assets = selected["assets"].toMap();
  for (auto it = assets.cbegin(); it != assets.cend(); ++it) {
    const auto path =
        selected["directory"].toString() + "/" + it.value().toString();
    if (QFileInfo::exists(path))
      resolved[it.key()] = path.startsWith(':')
                               ? "qrc" + path
                               : QUrl::fromLocalFile(path).toString() +
                                     "?v=" + selected["revision"].toString();
  }
  QStringList animatedSources = selected["_animatedRoles"].toStringList();
  for (const auto &key : selected["_animatedSlots"].toStringList())
    if (resolved.contains(key))
      animatedSources << resolved[key].toString();
  const auto packWelcome =
      selected["_configuration"].toMap()["scenes"].toMap()["welcome"].toMap();
  const auto userWelcome =
      characterOptions["scenes"].toMap()["welcome"].toMap();
  const bool customGreetings =
      !packWelcome["greetings"].toList().isEmpty() ||
      !userWelcome["greetings"].toList().isEmpty() ||
      !selected["lines"].toMap()["greeting"].toString().isEmpty();
  // Only bundled scenes participate; imported character packs cannot shadow them.
  auto sceneDirectory = QString(":/qt/qml/Umeko/App/app/icons/characters/rare-scenes");
  if (!QFileInfo::exists(sceneDirectory + "/catalog.json"))
    sceneDirectory = root + "/app/icons/characters/rare-scenes";
  static const auto scenes = U::readJson(sceneDirectory + "/catalog.json", QVariantMap{}).toMap();
  auto easterEgg = scenes.value(selected["id"].toString()).toMap();
  if (!easterEgg.isEmpty()) {
    const auto path = sceneDirectory + "/" + selected["id"].toString() + ".png";
    easterEgg["source"] = path.startsWith(':') ? "qrc" + path : QUrl::fromLocalFile(path).toString();
  }
  return {{"id", selected["id"]},
          {"name", selected["name"]},
          {"assets", resolved},
          {"lines", selected["lines"]},
          {"animatedSources", animatedSources},
          {"customGreetings", customGreetings},
          {"easterEgg", easterEgg},
          {"dialogue", characterConfig["scenes"]},
          {"subagents", characterConfig["subagents"]}};
}
bool Backend::selectSubagentCharacter(int slot, QString id) {
  if (slot < 0 || slot >= 4)
    return false;
  if (id.isEmpty()) {
    auto options = characterOptions;
    auto roles = options["subagents"].toList();
    while (roles.size() < 4)
      roles << QVariantMap{};
    roles[slot] = QVariantMap{};
    options["subagents"] = roles;
    return configureCharacter(QString::fromUtf8(QJsonDocument::fromVariant(options).toJson()));
  }
  QVariantMap selected;
  for (const auto &value : characterLibrary(root, store.directory, true))
    if (value.toMap()["id"] == id)
      selected = value.toMap();
  if (selected.isEmpty())
    return false;
  try {
    const auto assets = selected["assets"].toMap();
    const auto source = [&](QString key, QString fallback) {
      const auto candidates = key == "avatarSmile" ? QStringList{"avatarSmile", "reply", "welcome"} : QStringList{key};
      for (const auto &candidate : candidates)
        if (assets.contains(candidate))
          return selected["directory"].toString() + "/" + assets[candidate].toString();
      const auto resource = ":/qt/qml/Umeko/App/app/icons/" + fallback;
      return QFileInfo::exists(resource) ? resource : root + "/app/icons/" + fallback;
    };
    QVariantMap row{{"name", selected["name"]}};
    const auto scenes = selected["_configuration"].toMap()["scenes"].toMap();
    const auto reply = scenes["reply"].toMap();
    for (const auto &key : QStringList{"tap", "celebration"})
      row[key] = reply.value(key, characterDefaults["scenes"].toMap()["reply"].toMap()[key]);
    row["fileDrop"] = scenes["files"].toMap().value("drop", QString("松开以添加附件"));
    const auto directory = store.directory + "/character-assets";
    requireCharacter(!QFileInfo(directory).isSymLink() && QDir().mkpath(directory), "无法创建角色资源目录");
    const QMap<QString, QString> sources{{"avatar", source("avatarSmile", "avatars/smile.webp")},
                                         {"catch", source("catch", "yanami-catch.png")}};
    for (auto it = sources.cbegin(); it != sources.cend(); ++it) {
      QFile input(it.value());
      requireCharacter(input.open(QIODevice::ReadOnly) && input.size() <= 12 * 1024 * 1024, "无法读取角色头像或文件过大");
      const auto bytes = input.readAll();
      const auto digest = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
      const auto target = directory + "/" + digest + "." + QFileInfo(it.value()).suffix().toLower();
      if (!QFileInfo::exists(target)) {
        QSaveFile output(target);
        requireCharacter(output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit(), "无法复制角色头像");
      }
      row[it.key()] = target;
    }
    auto options = characterOptions;
    auto roles = options["subagents"].toList();
    while (roles.size() < 4)
      roles << QVariantMap{};
    roles[slot] = row;
    options["subagents"] = roles;
    return configureCharacter(QString::fromUtf8(QJsonDocument::fromVariant(options).toJson()));
  } catch (const std::exception &error) {
    emit toast("角色配置失败：" + QString::fromUtf8(error.what()));
    return false;
  }
}
bool Backend::selectCharacter(QString id) {
  bool exists = false;
  for (const auto &value : characterPacks())
    exists |= value.toMap()["id"] == id;
  if (!exists)
    return false;
  const auto previous = store.data.value("character", "yanami");
  store.data["character"] = id;
  if (!store.save()) {
    store.data["character"] = previous;
    return false;
  }
  refreshCharacters();
  emit changed();
  return true;
}
bool Backend::importCharacter(QString url) {
  QString staging, revision, dialogue;
  bool committed = false;
  const auto previous = store.data.value("character", "yanami");
  try {
    auto file = U::localPath(url);
    QTemporaryDir extraction;
    if (file.endsWith(".zip", Qt::CaseInsensitive)) {
      requireCharacter(extraction.isValid(), "无法创建形象暂存目录");
      file = unpackCharacterZip(file, extraction.path());
    }
    auto item = readCharacter(file, true);
    const auto configuration = normalizeConfiguration(
        item["_configuration"].toMap(), characterDefaults);
    for (const auto &value : configuration["subagents"].toList()) {
      const auto row = value.toMap();
      requireCharacter(row["avatar"].toString().isEmpty() &&
                           row["catch"].toString().isEmpty(),
                       "包内头像请放入 assets.subagent0 等资源项");
    }
    const auto id = item["id"].toString();
    requireCharacter(!builtInCharacters.contains(id), "不能覆盖内置形象的 id");
    const auto library = store.directory + "/characters",
               target = library + "/" + id;
    requireCharacter(!QFileInfo(library).isSymLink() &&
                         !QFileInfo(target).isSymLink() &&
                         !QFileInfo(target + "/manifest.json").isSymLink(),
                     "形象安装目录不能是符号链接");
    requireCharacter(QDir().mkpath(target), "无法创建形象目录");
    staging = library + "/.staging-" +
              QUuid::createUuid().toString(QUuid::WithoutBraces);
    requireCharacter(QDir().mkpath(staging), "无法创建形象暂存目录");
    QSet<QString> copied;
    for (const auto &value : item["assets"].toMap()) {
      const auto relative = value.toString();
      if (copied.contains(relative))
        continue;
      copied.insert(relative);
      const auto destination = staging + "/" + relative;
      requireCharacter(
          QDir().mkpath(QFileInfo(destination).absolutePath()) &&
              QFile::copy(QFileInfo(file).absolutePath() + "/" + relative,
                          destination),
          "无法复制形象资源");
    }
    // AnimatedImage can hold GIF files open on Windows. Publish fresh assets
    // without renaming or overwriting files that are currently displayed.
    auto token = QUuid::createUuid().toString(QUuid::Id128);
    const auto revisionName = ".assets-" + token;
    revision = target + "/" + revisionName;
    requireCharacter(QDir().rename(staging, revision), "无法安装形象资源");
    auto assets = item["assets"].toMap();
    for (auto it = assets.begin(); it != assets.end(); ++it)
      it.value() = revisionName + "/" + it.value().toString();
    item["assets"] = assets;
    if (item.contains("dialogue")) {
      const auto name = "dialogue-" + token + ".json";
      dialogue = target + "/" + name;
      requireCharacter(QFile::copy(QFileInfo(file).absolutePath() + "/" +
                                       item["dialogue"].toString(),
                                   dialogue),
                       "无法复制台词 JSON");
      item["dialogue"] = name;
    }
    item.remove("_configuration");
    store.data["character"] = id;
    if (!store.save()) {
      store.data["character"] = previous;
      throw std::runtime_error("无法保存形象设置");
    }
    if (!U::writeJson(target + "/manifest.json", item)) {
      store.data["character"] = previous;
      store.save();
      throw std::runtime_error("无法保存形象清单");
    }
    committed = true;
    refreshCharacters();
    emit changed();
    for (const auto &entry :
         QDir(target).entryInfoList(QDir::AllEntries | QDir::Hidden |
                                    QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
      if (entry.absoluteFilePath() != revision &&
          QRegularExpression("^\\.assets-[a-f0-9]{32}$")
              .match(entry.fileName())
              .hasMatch()) {
        const auto path = entry.absoluteFilePath();
        QTimer::singleShot(250, [path] { cleanupRetiredCharacter(path); });
      } else if (entry.absoluteFilePath() != dialogue &&
                 QRegularExpression("^dialogue-[a-f0-9]{32}\\.json$")
                     .match(entry.fileName())
                     .hasMatch())
        QFile::remove(entry.absoluteFilePath());
    }
    return true;
  } catch (const std::exception &error) {
    if (!staging.isEmpty())
      QDir(staging).removeRecursively();
    if (!committed) {
      if (!revision.isEmpty())
        QDir(revision).removeRecursively();
      if (!dialogue.isEmpty())
        QFile::remove(dialogue);
    }
    emit toast("形象导入失败：" + QString::fromUtf8(error.what()));
    return false;
  }
}
bool Backend::removeCharacter(QString id) {
  if (builtInCharacters.contains(id) ||
      !QRegularExpression("^[a-z0-9][a-z0-9-]{0,47}$").match(id).hasMatch())
    return false;
  const auto folder = store.directory + "/characters/" + id;
  if (!QFileInfo(folder).isDir() || QFileInfo(folder).isSymLink())
    return false;
  if (store.data["character"] == id && !selectCharacter("yanami"))
    return false;
  const auto marker = ".removed-" +
                      QUuid::createUuid().toString(QUuid::WithoutBraces) +
                      ".json";
  if (!QFile::rename(folder + "/manifest.json", folder + "/" + marker))
    return false;
  refreshCharacters();
  emit changed();
  QTimer::singleShot(
      250, [folder, marker] { cleanupRemovedCharacter(folder, marker); });
  return true;
}

bool Backend::exportCharacterSkill(QString url) {
  auto name = QString("create-umeko-character.zip");
  QFile source(root + "/provider-resources/" + name);
  if (!source.exists())
    source.setFileName(root + "/build/provider-resources/" + name);
  const auto target = U::localPath(url);
  if (target.isEmpty() || !source.open(QIODevice::ReadOnly)) {
    emit toast("角色制作 Skill 不可用，请重新构建开发资源");
    return false;
  }
  const auto bytes = source.readAll();
  QSaveFile output(target);
  if (!output.open(QIODevice::WriteOnly) ||
      output.write(bytes) != bytes.size() || !output.commit()) {
    emit toast("无法导出角色制作 Skill");
    return false;
  }
  emit toast("角色制作 Skill 已导出");
  return true;
}
