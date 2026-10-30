#include "desktop_pet.hpp"
#include "backend.hpp"
#include "desktop_plugins.hpp"
#include "miniz.h"
#include <QFutureWatcher>
#include <QImageReader>
#include <QScreen>
#include <QtConcurrent>
#include <stdexcept>
namespace {
const QStringList states{"idle",    "thinking", "working",
                         "waiting", "done",     "error"};
const QStringList builtinAppearances{"yanami", "komari"};
const QVariantMap defaults{{"visible", false},      {"appearance", "yanami"},
                           {"size", 192},           {"alwaysOnTop", true},
                           {"paused", false},       {"sampleIntervalMs", 1500},
                           {"minimumStateMs", 3000}};
void need(bool ok, QString error) {
  if (!ok)
    throw std::runtime_error(error.toStdString());
}
QString relative(QString path) {
  need(!path.isEmpty() && !path.startsWith('/') && !path.contains('\\') &&
           !path.contains(':'),
       "形象文件必须使用包内相对路径：" + path);
  const QRegularExpression device(
      "^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$",
      QRegularExpression::CaseInsensitiveOption);
  for (auto part : path.split('/')) {
    need(!part.isEmpty() && part != "." && part != ".." &&
             !part.endsWith('.') && !part.endsWith(' ') &&
             !device.match(part).hasMatch(),
         "形象路径包含不安全的目录段：" + path);
    for (auto c : part)
      need(c.unicode() >= 32, "形象路径包含控制字符");
  }
  return path;
}
QVariantMap inspect(QString path) {
  need(QStringList{"png", "webp", "gif"}.contains(
           QFileInfo(path).suffix().toLower()) &&
           QFileInfo(path).isFile() &&
           QFileInfo(path).size() <= 32 * 1024 * 1024,
       "形象支持最多 32 MiB 的 WebP、GIF 或 PNG：" +
           QFileInfo(path).fileName());
  QImageReader reader(path);
  auto size = reader.size();
  auto count = qMax(1, reader.imageCount());
  need(reader.canRead() && size.width() >= 1 && size.height() >= 1 &&
           size.width() <= 1024 && size.height() <= 1024 && count <= 240 &&
           qint64(size.width()) * size.height() * count <= 60000000,
       "无法读取形象，或动画超过 1024px / 240 帧 / 6000 万像素限制");
  int duration = 0;
  for (int i = 0; i < count; ++i) {
    auto image = reader.read();
    need(!image.isNull(), "形象包含无法解码的帧");
    duration += qMax(0, reader.nextImageDelay());
  }
  need(duration <= 30000, "单段动画不能超过 30 秒");
  return {{"animated", count > 1},
          {"width", size.width()},
          {"height", size.height()},
          {"frames", count}};
}
QVariantMap readPack(QString folder, bool check = false) {
  QFile file(folder + "/pet.json");
  need(file.open(QIODevice::ReadOnly) && file.size() <= 65536,
       "pet.json 无法读取或超过 64 KiB");
  QJsonParseError error;
  auto doc = QJsonDocument::fromJson(file.readAll(), &error);
  need(error.error == QJsonParseError::NoError && doc.isObject(),
       "pet.json JSON 无效");
  auto value = doc.object().toVariantMap();
  need(value["format"] == "umeko-pet" &&
           value["version"].typeId() != QMetaType::Bool &&
           value["version"].typeId() != QMetaType::QString &&
           value["version"].toDouble() == 1,
       "需要 format 为 umeko-pet、version 为 1 的 pet.json");
  need(QRegularExpression("^[a-z][a-z0-9-]{0,47}$")
           .match(value["id"].toString())
           .hasMatch(),
       "形象 id 需要 1–48 位小写字母、数字或短横线，以字母开头");
  need(!value["name"].toString().trimmed().isEmpty() &&
           value["name"].toString().size() <= 48,
       "形象名称需要 1–48 个字符");
  auto map = value["states"].toMap();
  need(map.contains("idle"), "states 必须包含 idle");
  QSet<QString> seen;
  QVariantMap info;
  qint64 total = 0, pixels = 0;
  const auto base = QFileInfo(folder).canonicalFilePath();
  for (auto it = map.cbegin(); it != map.cend(); ++it) {
    need(states.contains(it.key()) && it.value().typeId() == QMetaType::QString,
         "形象状态无效");
    auto name = relative(it.value().toString());
    if (seen.contains(name))
      continue;
    seen.insert(name);
    auto path = folder + "/" + name;
    auto canonical = QFileInfo(path).canonicalFilePath();
    need(!base.isEmpty() && canonical.startsWith(base + "/") &&
             QFileInfo(path).isFile(),
         "形象不能指向包外：" + name);
    auto node = QFileInfo(path);
    while (node.absoluteFilePath() != base) {
      need(!node.isSymLink(), "形象不能使用链接：" + name);
      auto parent = node.absolutePath();
      if (parent == node.absoluteFilePath())
        break;
      node = QFileInfo(parent);
    }
    total += QFileInfo(path).size();
    if (check) {
      auto image = inspect(path);
      pixels += qint64(image["width"].toInt()) * image["height"].toInt() *
                image["frames"].toInt();
      info[name] = image;
    }
  }
  need(total <= 128 * 1024 * 1024 && pixels <= 220000000,
       "形象资源总量超过 128 MiB 或 2.2 亿像素");
  auto dialogue = value.value("dialogue").toMap();
  need(!value.contains("dialogue") ||
           value["dialogue"].typeId() == QMetaType::QVariantMap,
       "dialogue 必须为对象");
  for (auto it = dialogue.cbegin(); it != dialogue.cend(); ++it) {
    need(QStringList{"tap", "hover", "drag"}.contains(it.key()) &&
             it.value().typeId() == QMetaType::QVariantList,
         "dialogue 支持 tap、hover、drag");
    auto lines = it.value().toList();
    need(!lines.isEmpty() && lines.size() <= 16, "每种互动台词需要 1–16 条");
    for (auto line : lines)
      need(line.typeId() == QMetaType::QString &&
               !line.toString().trimmed().isEmpty() &&
               line.toString().size() <= 120,
           "台词最多 120 字");
  }
  if (check)
    value["images"] = info;
  return value;
}
QString unzip(QString source, QString stage) {
  QFile input(source);
  need(input.open(QIODevice::ReadOnly) && input.size() <= 128 * 1024 * 1024,
       "形象 ZIP 不能超过 128 MiB");
  auto raw = input.readAll();
  mz_zip_archive zip{};
  need(mz_zip_reader_init_mem(&zip, raw.constData(), raw.size(), 0),
       "ZIP 文件无效");
  auto cleanup = qScopeGuard([&] { mz_zip_reader_end(&zip); });
  auto count = mz_zip_reader_get_num_files(&zip);
  need(count > 0 && count <= 64, "形象最多包含 64 个文件");
  struct File {
    mz_uint index;
    QString name;
    quint64 size;
  };
  QList<File> files;
  QMap<QString, bool> seen;
  QStringList manifests;
  quint64 total = 0;
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat stat{};
    need(mz_zip_reader_file_stat(&zip, i, &stat), "ZIP 索引无效");
    auto length = mz_zip_reader_get_filename(&zip, i, nullptr, 0);
    need(length > 1 && length <= 4096, "ZIP 路径无效");
    QByteArray bytes(length, '\0');
    need(mz_zip_reader_get_filename(&zip, i, bytes.data(), length),
         "ZIP 路径无效");
    auto name = QString::fromUtf8(bytes.constData(), length - 1);
    bool dir = name.endsWith('/');
    if (dir)
      name.chop(1);
    relative(name);
    auto key = name.normalized(QString::NormalizationForm_C).toCaseFolded();
    auto kind = (stat.m_external_attr >> 16) & 0170000;
    need((kind == 0 || kind == 0100000 || kind == 0040000) &&
             (!kind || (kind == 0040000) == dir),
         "形象不能包含链接或特殊文件：" + name);
    need(!seen.contains(key), "形象包含重复路径：" + name);
    for (auto it = seen.cbegin(); it != seen.cend(); ++it)
      need(!(key.startsWith(it.key() + "/") && !it.value()) &&
               !(it.key().startsWith(key + "/") && !dir),
           "形象包含冲突路径：" + name);
    seen[key] = dir;
    need(!(stat.m_bit_flag & 1) && (stat.m_method == 0 || stat.m_method == 8),
         "不支持加密或特殊压缩的形象 ZIP");
    if (dir)
      continue;
    need(stat.m_uncomp_size <= 128 * 1024 * 1024 - total,
         "形象解压后超过 128 MiB");
    total += stat.m_uncomp_size;
    files << File{i, name, stat.m_uncomp_size};
    if (name.section('/', -1) == "pet.json")
      manifests << name;
  }
  need(manifests.size() == 1 && manifests.first().count('/') <= 1,
       "ZIP 根目录或单层目录中需要 pet.json");
  auto prefix = manifests.first().chopped(8);
  for (auto f : files) {
    need(f.name.startsWith(prefix), "形象 ZIP 包含目录外文件：" + f.name);
    auto name = relative(f.name.mid(prefix.size()));
    QByteArray bytes(f.size, '\0');
    need(mz_zip_reader_extract_to_mem(&zip, f.index, bytes.data(), bytes.size(),
                                      0),
         "ZIP 数据校验失败");
    auto path = stage + "/" + name;
    need(QDir().mkpath(QFileInfo(path).absolutePath()), "无法创建形象目录");
    QFile out(path);
    need(out.open(QIODevice::WriteOnly) && out.write(bytes) == bytes.size(),
         "无法保存形象资源");
  }
  return stage;
}
QVariantMap importPack(QString source, QString destination) {
  try {
    need(QFileInfo(source).isFile(), "请选择本机形象 ZIP、pet.json 或动画文件");
    need(QDir().mkpath(destination), "无法创建形象目录");
    QTemporaryDir temp(destination + "/.import-XXXXXX");
    need(temp.isValid(), "无法暂存形象");
    auto stage = temp.path() + "/pack";
    QDir().mkpath(stage);
    if (QFileInfo(source).suffix().compare("zip", Qt::CaseInsensitive) == 0)
      unzip(source, stage);
    else if (QFileInfo(source).fileName().compare("pet.json",
                                                  Qt::CaseInsensitive) == 0) {
      auto pack = readPack(QFileInfo(source).absolutePath());
      need(QFile::copy(source, stage + "/pet.json"), "无法复制形象清单");
      for (auto item : pack["states"].toMap()) {
        auto name = relative(item.toString());
        auto path = stage + "/" + name;
        QDir().mkpath(QFileInfo(path).absolutePath());
        if (!QFileInfo::exists(path))
          need(QFile::copy(QFileInfo(source).absolutePath() + "/" + name, path),
               "无法复制形象资源");
      }
    } else {
      inspect(source);
      auto name = "animation." + QFileInfo(source).suffix().toLower();
      need(QFile::copy(source, stage + "/" + name), "无法复制形象资源");
      need(U::writeJson(
               stage + "/pet.json",
               QVariantMap{
                   {"format", "umeko-pet"},
                   {"version", 1},
                   {"id", "custom-" + U::uuid().remove('-').left(12)},
                   {"name", QFileInfo(source).completeBaseName().left(48)},
                   {"states", QVariantMap{{"idle", name}}}}),
           "无法写入形象清单");
    }
    auto pack = readPack(stage, true);
    auto id = pack["id"].toString();
    need(!builtinAppearances.contains(id) && !QFileInfo::exists(destination + "/" + id),
         "该形象 id 已存在，请修改 pet.json 中的 id 后再导入");
    QSet<QString> keep{"pet.json"};
    for (auto value : pack["states"].toMap())
      keep.insert(value.toString());
    QDirIterator it(stage, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
      auto path = it.next();
      if (!keep.contains(QDir(stage).relativeFilePath(path)))
        need(QFile::remove(path), "无法清理未声明的形象文件");
    }
    need(U::writeJson(stage + "/pet.json", pack) &&
             QDir().rename(stage, destination + "/" + id),
         "无法安装形象");
    return {{"ok", true}, {"appearance", pack}};
  } catch (const std::exception &e) {
    return {{"ok", false}, {"error", QString::fromUtf8(e.what())}};
  }
}
} // namespace
DesktopPet::DesktopPet(DesktopPlugins *manager) : QObject(manager), m(manager) {
  clock.start();
  directory = m->root + "/state/desktop-pet";
  packDirectory = directory + "/appearances";
  options = defaults;
  activity = {{"state", "idle"}, {"activeChats", 0}, {"activeSubagents", 0}};
  loadPacks();
  timer.setTimerType(Qt::CoarseTimer);
  connect(&timer, &QTimer::timeout, this, &DesktopPet::sample);
  try {
    configure(m->settings["config"].toMap()["desktop-pet"].toMap(), false);
  } catch (...) {
    lastError = "桌宠配置或形象不可用，已恢复默认设置";
    options = defaults;
  }
}
void DesktopPet::loadPacks() {
  packs.clear();
  QStringList folders;
  QSet<QString> builtinFolders;
  for (const auto &id : builtinAppearances) {
    const auto folder = m->b->root + "/umeko-plugins/desktop-pet/assets/" + id;
    folders << folder;
    builtinFolders.insert(folder);
  }
  for (auto name :
       QDir(packDirectory).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
    if (!name.startsWith('.'))
      folders << packDirectory + "/" + name;
  for (auto folder : folders)
    try {
      auto pack = readPack(folder);
      bool builtin = builtinFolders.contains(folder);
      if (!builtin && builtinAppearances.contains(pack["id"].toString()))
        continue;
      pack["directory"] = folder;
      pack["builtin"] = builtin;
      packs[pack["id"].toString()] = pack;
    } catch (...) {
    }
}
QVariantList DesktopPet::appearances() const {
  QVariantList rows;
  for (auto v : packs) {
    auto p = v.toMap();
    rows << QVariantMap{
        {"id", p["id"]}, {"name", p["name"]}, {"builtin", p["builtin"]}};
  }
  return rows;
}
QVariantMap DesktopPet::state() const {
  auto pack =
      packs.value(options["appearance"].toString(), packs["yanami"]).toMap();
  auto name = pack["states"]
                  .toMap()
                  .value(action, pack["states"].toMap()["idle"])
                  .toString();
  return {{"action", action},
          {"source",
           name.isEmpty()
               ? ""
               : QUrl::fromLocalFile(pack["directory"].toString() + "/" + name)
                     .toString()},
          {"animated",
           pack["images"].toMap()[name].toMap().value("animated", true)},
          {"appearance", pack["id"]},
          {"name", pack["name"]},
          {"running", running},
          {"activity", activity},
          {"importing", importing},
          {"error", lastError}};
}
QVariantMap DesktopPet::status() const {
  return {{"config", options},
          {"state", state()},
          {"appearances", appearances()},
          {"visible", visible()}};
}
QVariantMap DesktopPet::configure(QVariantMap patch, bool persist) {
  auto value = options;
  for (auto it = patch.cbegin(); it != patch.cend(); ++it) {
    need(defaults.contains(it.key()), "未知的桌宠配置项");
    value[it.key()] = it.value();
  }
  for (auto key : {"visible", "alwaysOnTop", "paused"})
    need(value[key].typeId() == QMetaType::Bool,
         QString(key) + " 必须是布尔值");
  const QList<std::tuple<QString, int, int>> limits{
      {"size", 96, 320},
      {"sampleIntervalMs", 1000, 2000},
      {"minimumStateMs", 3000, 30000}};
  for (auto [key, low, high] : limits)
    need(value[key].typeId() != QMetaType::Bool &&
             value[key].typeId() != QMetaType::QString &&
             value[key].toDouble() == value[key].toInt() &&
             value[key].toInt() >= low && value[key].toInt() <= high,
         key + " 数值超出允许范围");
  need(packs.contains(value["appearance"].toString()),
       "形象不存在，请先导入并通过 appearances 查询可用 id");
  if (persist) {
    auto old = m->settings;
    auto config = m->settings["config"].toMap();
    config["desktop-pet"] = value;
    m->settings["config"] = config;
    if (!m->save()) {
      m->settings = old;
      need(false, "桌宠设置保存失败");
    }
  }
  options = value;
  timer.setInterval(value["sampleIntervalMs"].toInt());
  emit changed();
  emit m->changed();
  return status();
}
void DesktopPet::start() {
  if (running || closed)
    return;
  running = true;
  timer.start(options["sampleIntervalMs"].toInt());
  sample();
  emit changed();
}
void DesktopPet::stop() {
  running = false;
  timer.stop();
  emit changed();
}
void DesktopPet::close() {
  closed = true;
  stop();
}
bool DesktopPet::step() {
  auto now = clock.elapsed();
  if (!interaction.isEmpty() && now >= until)
    interaction.clear();
  auto wanted = interaction.isEmpty() ? target : interaction;
  if (!options["paused"].toBool() && wanted != action &&
      now - since >= options["minimumStateMs"].toInt()) {
    action = wanted;
    since = now;
    return true;
  }
  return false;
}
void DesktopPet::sample() {
  if (!running)
    return;
  auto records = m->b->records.values();
  if (!records.contains(m->b->view))
    records << m->b->view;
  int active = 0, subagents = 0;
  bool waiting = false, working = false;
  QHash<QString, bool> next;
  for (auto r : records) {
    bool busy = r->busy();
    if (observed.value(r->key, false) && !busy && !r->cancelled) {
      terminal = r->failed ? "error" : "done";
      terminalUntil = clock.elapsed() + 6000;
    }
    next[r->key] = busy;
    if (busy) {
      ++active;
      waiting |= !r->approval.isEmpty() || !r->networkRetry.isEmpty();
      working |= r->petActivity == "working";
    }
    for (auto value : r->subagents)
      if (QStringList{"running", "starting", "queued", "waiting"}.contains(
              value.toMap()["status"].toString()))
        ++subagents;
  }
  observed = next;
  target = waiting     ? "waiting"
           : active    ? (working ? "working" : "thinking")
           : subagents ? "working"
           : clock.elapsed() < terminalUntil ? terminal
                                             : "idle";
  QVariantMap nextActivity{{"state", target},
                           {"activeChats", active},
                           {"activeSubagents", subagents}};
  bool changedActivity = nextActivity != activity;
  activity = nextActivity;
  if (step() || changedActivity)
    emit changed();
}
void DesktopPet::interact(QString kind) {
  auto now = clock.elapsed();
  if (!running || (kind == lastKind && now - lastInteraction < 400))
    return;
  lastKind = kind;
  lastInteraction = now;
  auto lines = packs[options["appearance"].toString()]
                   .toMap()["dialogue"]
                   .toMap()[kind]
                   .toList();
  if (!lines.isEmpty())
    emit reaction(lines[QRandomGenerator::global()->bounded(int(lines.size()))]
                      .toString());
  if (QStringList{"tap", "hover", "drag"}.contains(kind) &&
      (kind != "hover" || target == "idle")) {
    interaction = kind == "tap" ? "done" : "waiting";
    until = qMax(now, since + options["minimumStateMs"].toInt()) +
            options["minimumStateMs"].toInt();
    if (step())
      emit changed();
  }
}
QVariantMap DesktopPet::ui(QString method, QVariantMap args) {
  try {
    return {{"ok", true}, {"result", handle(method, args)}};
  } catch (const std::exception &e) {
    lastError = QString::fromUtf8(e.what());
    emit changed();
    return {{"ok", false}, {"error", lastError}};
  }
}
QVariantMap DesktopPet::handle(QString method, QVariantMap args) {
  auto spec = m->entry("desktop-pet")["agent"]
                  .toMap()["methods"]
                  .toMap()[method]
                  .toMap();
  need(!spec.isEmpty(), "未知的桌宠方法，请读取 widgets.docs");
  auto schema = spec["parameters"].toMap(),
       properties = schema["properties"].toMap();
  for (auto key : schema["required"].toStringList())
    need(args.contains(key), "桌宠方法参数缺失：" + key);
  for (auto it = args.cbegin(); it != args.cend(); ++it) {
    need(properties.contains(it.key()), "未知的桌宠方法参数：" + it.key());
    auto rule = properties[it.key()].toMap();
    if (rule["type"] == "string")
      need(it.value().typeId() == QMetaType::QString &&
               it.value().toString().size() <=
                   rule.value("maxLength", 65536).toInt(),
           "桌宠方法参数无效：" + it.key());
    if (rule.contains("enum"))
      need(rule["enum"].toList().contains(it.value()),
           "桌宠方法参数无效：" + it.key());
  }
  if (method == "status")
    return status();
  if (method == "appearances")
    return {{"appearances", appearances()}};
  if (method == "configure")
    return configure(args);
  if (method == "show" || method == "hide")
    return configure({{"visible", method == "show"}});
  if (method == "interact") {
    interact(args.value("kind", "tap").toString());
    return status();
  }
  if (method == "selectAppearance")
    return configure({{"appearance", args["id"]}});
  if (method == "resetPosition") {
    emit resetPositionRequested();
    savePosition(-1, -1);
    return status();
  }
  if (method == "removeAppearance") {
    auto id = args["id"].toString();
    need(!importing && packs.contains(id) &&
             !packs[id].toMap()["builtin"].toBool(),
         "不能删除内置形象，或形象正在导入");
    if (options["appearance"] == id)
      configure({{"appearance", "yanami"}});
    auto folder = packs[id].toMap()["directory"].toString();
    need(folder.startsWith(packDirectory + "/") &&
             QDir(folder).removeRecursively(),
         "无法删除形象");
    loadPacks();
    emit changed();
    return status();
  }
  if (method == "importAppearance") {
    need(!importing && !closed, "正在导入形象，请稍候");
    auto source = args["path"].toString();
    if (QUrl(source).isLocalFile())
      source = QUrl(source).toLocalFile();
    need(QDir::isAbsolutePath(source), "path 必须是本机绝对路径");
    importing = true;
    lastError.clear();
    auto token = U::uuid();
    auto watcher = new QFutureWatcher<QVariantMap>(this);
    connect(watcher, &QFutureWatcher<QVariantMap>::finished, this,
            [this, watcher, token] {
              auto result = watcher->result();
              watcher->deleteLater();
              importing = false;
              if (!closed) {
                lastError =
                    result["ok"].toBool() ? "" : result["error"].toString();
                loadPacks();
                emit changed();
                if (result["ok"].toBool())
                  result = {{"ok", true},
                            {"result",
                             QVariantMap{{"appearance",
                                          result["appearance"].toMap()["id"]},
                                         {"appearances", appearances()}}}};
                emit imported(token, result);
              }
            });
    watcher->setFuture(QtConcurrent::run(importPack, source, packDirectory));
    emit changed();
    return {{"status", "importing"}, {"importId", token}};
  }
  need(false, "未知的桌宠方法");
  return {};
}
void DesktopPet::savePosition(int x, int y) {
  if (!closed && !U::writeJson(directory + "/position.json",
                               QVariantMap{{"x", x}, {"y", y}}))
    emit m->b->toast("桌宠位置保存失败");
}
QVariantMap DesktopPet::position() const {
  return U::readJson(directory + "/position.json",
                     QVariantMap{{"x", -1}, {"y", -1}})
      .toMap();
}
QVariantMap DesktopPet::screenBounds(int x, int y) const {
  auto screens = QGuiApplication::screens();
  if (screens.isEmpty())
    return {{"x", 0}, {"y", 0}, {"width", 1920}, {"height", 1080}};
  auto selected = QGuiApplication::screenAt(QPoint(x, y));
  if (!selected) {
    qint64 best = std::numeric_limits<qint64>::max();
    for (auto screen : screens) {
      auto area = screen->availableGeometry();
      qint64 dx = std::max({area.left() - x, 0, x - area.right()}),
             dy = std::max({area.top() - y, 0, y - area.bottom()});
      if (dx * dx + dy * dy < best) {
        best = dx * dx + dy * dy;
        selected = screen;
      }
    }
  }
  auto area = selected->availableGeometry();
  return {{"x", area.x()},
          {"y", area.y()},
          {"width", area.width()},
          {"height", area.height()}};
}
