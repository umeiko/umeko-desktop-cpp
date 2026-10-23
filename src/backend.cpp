#include "backend.hpp"
#include <QClipboard>
#include <QDesktopServices>
#include <QImageReader>
#include <QMimeData>
#include <QNetworkProxyFactory>
#include <stdexcept>
#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

Backend::Backend(QString resourceRoot, QString directory, QObject *parent)
    : QObject(parent), store(directory), root(U::cleanPath(resourceRoot)),
      chat(this) {
  release = root + "/runtime/releases/coding-1.3.0";
  auto active =
      U::readJson(store.directory + "/kernels/active.json", QVariantMap{})
          .toMap();
  auto folder = active["directory"].toString();
  if (QRegularExpression("^(coding|factory)-[\\w.-]+$")
          .match(folder)
          .hasMatch() &&
      QFileInfo(store.directory + "/kernels/" + folder + "/bridge.mjs")
          .isFile()) {
    const auto candidate = store.directory + "/kernels/" + folder;
    auto manifest =
        U::readJson(candidate + "/manifest.json", QVariantMap{}).toMap();
    bool compatible = manifest["projection"].toInt() == 2 &&
                      manifest["engine"] == "full-coding-agent";
    // The adapters belong to this desktop build, even when Pi itself was
    // upgraded separately. Keep web/queue fixes with the application update.
    for (const auto &name :
         {"bridge.mjs", "resources.mjs", "provider-policy.mjs", "chatbox.mjs",
          "network-retry.mjs"}) {
      if (!compatible)
        break;
      QFile source(root + "/runtime/" + name);
      if (!source.open(QIODevice::ReadOnly)) {
        compatible = false;
        break;
      }
      const auto bytes = source.readAll();
      QFile current(candidate + "/" + name);
      if (current.open(QIODevice::ReadOnly) && current.readAll() == bytes)
        continue;
      current.close();
      QSaveFile destination(candidate + "/" + name);
      compatible = destination.open(QIODevice::WriteOnly) &&
                   destination.write(bytes) == bytes.size() &&
                   destination.commit();
    }
    if (compatible)
      release = candidate;
  }
  privateToken = QString::fromLatin1(U::random(40).toBase64(
      QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
  view = fresh(store.data["workspace"].toString());
  drafts[view->workspace] = view;
  sessionIndex = store.sessions();
  for (auto v : sessionIndex) {
    auto s = v.toMap();
    if (!s["workspace"].toString().isEmpty())
      ensureProject(s["workspace"].toString());
  }
  providers = new Providers(this);
  management = new PiManagement(this);
  about = new AppAbout(this);
  previewLoader = new SessionPreview(this);
  appearanceSave.setSingleShot(true);
  appearanceSave.setInterval(250);
  connect(&appearanceSave, &QTimer::timeout, this, [this] { store.save(); });
  connect(this, &Backend::chatChanged, this, [this] {
    chat.update(view->messages);
    emit sidebarChanged();
  });
  streaming.setSingleShot(true);
  streaming.setInterval(24);
  connect(&streaming, &QTimer::timeout, this, [this] {
    emit chatChanged();
    emit transcriptChanged();
  });
  repairSelection();
  store.save();
  gatewayStart();
}
Backend::~Backend() { close(); }
QVariantMap Backend::config() const {
  auto cfg = store.data;
  auto server = cfg["server"].toMap();
  server.remove("keys");
  cfg["server"] = server;
  auto rows = cfg["providers"].toList();
  for (auto &v : rows) {
    auto p = v.toMap();
    p.remove("key");
    p.remove("secrets");
    if (!p.contains("hasKey"))
      p["hasKey"] = false;
    QVariantList models;
    for (auto m : p["models"].toList()) {
      try {
        models << U::normalizeModel(m.toMap());
      } catch (...) {
      }
    }
    p["models"] = models;
    v = p;
  }
  cfg["providers"] = rows;
  return cfg;
}
QVariantMap Backend::provider(QString id) const {
  for (auto v : store.data["providers"].toList())
    if (v.toMap()["id"] == id)
      return v.toMap();
  return {};
}
QVariantList Backend::modelCatalog() const {
  QVariantList rows;
  for (auto pv : config()["providers"].toList()) {
    auto p = pv.toMap();
    for (auto mv : p["models"].toList()) {
      auto m = mv.toMap();
      rows << QVariantMap{{"id", m["alias"]},
                          {"object", "model"},
                          {"owned_by", p["name"]},
                          {"provider", p["id"]},
                          {"contextWindow", m["contextWindow"]},
                          {"input", m["vision"].toBool()
                                        ? QStringList{"text", "image"}
                                        : QStringList{"text"}}};
    }
  }
  return rows;
}
void Backend::repairSelection() {
  auto current = store.data["selectedProvider"].toString(),
       model = store.data["selectedModel"].toString();
  QVariantList all = modelCatalog();
  for (auto v : all) {
    auto m = v.toMap();
    if (m["provider"] == current && m["id"] == model)
      return;
  }
  if (all.isEmpty()) {
    store.data["selectedProvider"] = "";
    store.data["selectedModel"] = "";
  } else {
    auto m = all.first().toMap();
    store.data["selectedProvider"] = m["provider"];
    store.data["selectedModel"] = m["id"];
  }
}
bool Backend::modelsBusy(QString id) const {
  for (auto s : records)
    if (s->busy() && (id.isEmpty() || s->provider == id))
      return true;
  return false;
}
QString Backend::agentDir() const {
  auto path = store.data["agent"].toMap()["agentDir"].toString();
  return path.isEmpty() ? QDir::homePath() + "/.pi/agent" : U::cleanPath(path);
}
QString Backend::kernelVersion() const {
  return U::readJson(release + "/manifest.json", QVariantMap{})
      .toMap()
      .value("version", "1.3.0")
      .toString();
}
void Backend::notify() {
  repairSelection();
  store.save();
  configureGateway();
  emit changed();
  emit chatChanged();
  emit proxyKeysChanged();
}
bool Backend::saveSidebarLayout(int width, bool collapse) {
  auto ui = store.data["ui"].toMap();
  ui["sidebarWidth"] = qBound(200, width, 480);
  ui["sidebarCollapsed"] = collapse;
  auto before = store.data["ui"];
  store.data["ui"] = ui;
  if (!store.save()) {
    store.data["ui"] = before;
    emit toast("侧栏布局保存失败");
    return false;
  }
  emit layoutChanged();
  return true;
}
void Backend::saveViewport(int index, double offset, bool follow) {
  view->scrollIndex = index;
  view->scrollY = qMax(0., offset);
  view->followTail = follow;
}
void Backend::setConversationVisible(bool visible) {
  chatVisible = visible;
  if (visible) {
    markRead(view);
    emit sidebarChanged();
  }
}
void Backend::setTheme(QString theme) {
  QVariantMap custom;
  for (auto value : customThemes())
    if (value.toMap()["id"] == theme)
      custom = value.toMap();
  if (!QStringList{"light", "dark", "yanami"}.contains(theme) &&
      custom.isEmpty())
    return;
  store.data["theme"] = theme;
  store.data["dark"] =
      (custom.isEmpty() ? theme : custom["base"].toString()) == "dark";
  store.save();
  emit changed();
}
void Backend::copy(QString value) {
  QGuiApplication::clipboard()->setText(value);
  emit toast("已复制到剪贴板");
}
QVariantMap Backend::metadata(QString id) const {
  return store.data["sessionMeta"].toMap()[id].toMap();
}
void Backend::setMetadata(QString id, QVariantMap values) {
  auto all = store.data["sessionMeta"].toMap();
  auto old = all[id].toMap();
  for (auto it = values.begin(); it != values.end(); ++it)
    old[it.key()] = it.value();
  all[id] = old;
  store.data["sessionMeta"] = all;
  store.save();
}
QVariantMap Backend::ensureProject(QString path, bool reopen) {
  path = U::cleanPath(path);
  auto key = path;
#ifdef Q_OS_WIN
  key = key.toLower();
#endif
  auto id = U::sha(key.toUtf8()).left(16);
  auto removed = store.data["removedProjects"].toList();
  if (removed.contains(id)) {
    if (!reopen)
      return {};
    removed.removeAll(id);
    store.data["removedProjects"] = removed;
  }
  auto projects = store.data["projects"].toList();
  for (auto v : projects)
    if (v.toMap()["id"] == id)
      return v.toMap();
  QVariantMap p{
      {"id", id}, {"name", QFileInfo(path).fileName()}, {"path", path}};
  projects << p;
  store.data["projects"] = projects;
  return p;
}
QVariantList Backend::projectRows() const {
  QVariantList rows;
  QSet<QString> grouped;
  auto projects = store.data["projects"].toList();
  std::stable_sort(projects.begin(), projects.end(), [](auto a, auto b) {
    return a.toMap()["pinned"].toBool() > b.toMap()["pinned"].toBool();
  });
  auto sessions = sessionIndex;
  std::stable_sort(sessions.begin(), sessions.end(), [this](auto a, auto b) {
    return metadata(a.toMap()["id"].toString())["pinned"].toBool() >
           metadata(b.toMap()["id"].toString())["pinned"].toBool();
  });
  auto rowFor = [this](QVariantMap s) {
    auto id = s["id"].toString();
    auto meta = metadata(id);
    s["kind"] = "session";
    s["selected"] = view->id == id;
    s["pinned"] = meta["pinned"].toBool();
    s["archived"] = meta["archived"].toBool();
    s["running"] = false;
    s["loading"] = false;
    s["waiting"] = false;
    s["busy"] = false;
    s["unread"] = meta["unread"].toBool();
    for (auto live : records)
      if (live->id == id) {
        s["running"] = live->phase == "running";
        s["loading"] = live->phase == "loading" || live->phase == "working";
        s["waiting"] = !live->approval.isEmpty();
        s["busy"] = live->busy();
      }
    return s;
  };
  for (auto pv : projects) {
    auto p = pv.toMap();
    QVariantList children;
    for (auto sv : sessions) {
      auto s = sv.toMap();
      if (U::cleanPath(s["workspace"].toString()) == p["path"].toString() &&
          metadata(s["id"].toString())["archived"].toBool() == archived)
        children << s;
    }
    if (archived && children.isEmpty())
      continue;
    p["kind"] = "project";
    p["title"] = p["name"];
    p["collapsed"] = collapsed.contains(p["id"].toString());
    p["selected"] = p["id"] == store.data["activeProject"];
    p["pinned"] = p.value("pinned", false);
    rows << p;
    for (auto sv : children) {
      auto s = sv.toMap();
      grouped.insert(s["id"].toString());
      if (!p["collapsed"].toBool())
        rows << rowFor(s);
    }
  }
  QVariantList ungrouped;
  for (auto sv : sessions) {
    auto s = sv.toMap();
    if (grouped.contains(s["id"].toString()) ||
        metadata(s["id"].toString())["archived"].toBool() != archived)
      continue;
    auto key = U::cleanPath(s["workspace"].toString());
#ifdef Q_OS_WIN
    key = key.toLower();
#endif
    if (store.data["removedProjects"].toList().contains(
            U::sha(key.toUtf8()).left(16)))
      continue;
    ungrouped << rowFor(s);
  }
  if (!ungrouped.isEmpty()) {
    rows << QVariantMap{{"id", "ungrouped"},
                        {"kind", "label"},
                        {"title", "其他会话"},
                        {"selected", false}};
    rows.append(ungrouped);
  }
  return rows;
}
void Backend::setWorkspace(QString url) {
  auto path = U::cleanPath(url);
  if (!QFileInfo(path).isDir()) {
    emit toast("请选择存在的目录");
    return;
  }
  auto p = ensureProject(path, true);
  store.data["workspace"] = path;
  store.data["activeProject"] = p["id"];
  collapsed.remove(p["id"].toString());
  auto draft = drafts.value(path);
  if (!draft || !draft->messages.isEmpty() || draft->busy()) {
    draft = fresh(path);
    drafts[path] = draft;
  }
  activate(draft);
}
void Backend::openChatDirectory() {
  auto path = view->workspace.isEmpty() ? store.data["workspace"].toString()
                                        : view->workspace;
  if (QFileInfo(path).isDir())
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
  else
    emit toast("当前会话目录不存在");
}
void Backend::openChatboxPath(QString path, bool reveal) {
  QFileInfo file(path);
  if (!file.isAbsolute() || !file.exists()) {
    emit toast("文件已移动或不存在");
    return;
  }
  if (reveal && file.isFile()) {
#if defined(Q_OS_WIN)
    if (!QProcess::startDetached("explorer.exe",
                                 {"/select,", QDir::toNativeSeparators(path)}))
      emit toast("无法打开文件夹");
    return;
#elif defined(Q_OS_MACOS)
    if (!QProcess::startDetached("/usr/bin/open", {"-R", path}))
      emit toast("无法打开文件夹");
    return;
#else
    path = file.absolutePath();
#endif
  }
  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
    emit toast("无法打开，请检查文件关联");
}
QVariantMap Backend::chatboxFileInfo(QString path) {
  QFileInfo file(path);
  if (!file.isAbsolute() || !file.exists())
    return {};
  QImageReader reader(path);
  const auto kind = file.isDir()       ? "folder"
                    : reader.canRead() ? "image"
                                       : "file";
  const auto items =
      U::mediaView({QVariantMap{{"path", path}, {"kind", kind}}});
  return items.isEmpty() ? QVariantMap{} : items.first().toMap();
}
void Backend::attachChatboxFile(QString path) {
  if (!QFileInfo(path).isAbsolute() || !QFileInfo::exists(path)) {
    emit toast("文件已移动或不存在");
    return;
  }
  addAttachments(QString::fromUtf8(
      QJsonDocument(QJsonArray{QUrl::fromLocalFile(path).toString()})
          .toJson(QJsonDocument::Compact)));
}
void Backend::activateChatWindow() {
  for (auto window : QGuiApplication::topLevelWindows()) {
    if (window->objectName() != "mainWindow")
      continue;
    if (window->visibility() == QWindow::Minimized)
      window->showNormal();
    window->raise();
    window->requestActivate();
#if defined(Q_OS_WIN)
    if (QGuiApplication::platformName() == "windows") {
      const auto handle = reinterpret_cast<HWND>(window->winId());
      BringWindowToTop(handle);
      SetForegroundWindow(handle);
    }
#endif
    return;
  }
}
void Backend::copyChatboxFile(QString path) {
  if (!QFileInfo(path).isAbsolute() || !QFileInfo(path).exists()) {
    emit toast("文件已移动或不存在");
    return;
  }
  auto mime = new QMimeData;
  const auto url = QUrl::fromLocalFile(path);
  mime->setUrls({url});
  mime->setData("application/x-qt-windows-mime;value=\"Preferred DropEffect\"",
                QByteArray::fromHex("01000000"));
  mime->setData("x-special/gnome-copied-files", "copy\n" + url.toEncoded());
  mime->setData("application/x-kde-cutselection", "0");
  QGuiApplication::clipboard()->setMimeData(mime);
  emit toast("文件已复制");
}
void Backend::toggleArchived() {
  archived = !archived;
  emit chatChanged();
}
void Backend::togglePinned(QString kind, QString id) {
  if (kind == "project") {
    auto rows = store.data["projects"].toList();
    for (auto &v : rows) {
      auto p = v.toMap();
      if (p["id"] == id) {
        p["pinned"] = !p["pinned"].toBool();
        v = p;
      }
    }
    store.data["projects"] = rows;
    store.save();
  } else
    setMetadata(id, {{"pinned", !metadata(id)["pinned"].toBool()}});
  emit chatChanged();
}
void Backend::archiveSession(QString id, bool value) {
  setMetadata(id, {{"archived", value}});
  if (view->id == id)
    newSession();
  emit chatChanged();
}
void Backend::archiveProject(QString id, bool value) {
  auto projects = store.data["projects"].toList();
  QString path;
  for (auto p : projects)
    if (p.toMap()["id"] == id)
      path = p.toMap()["path"].toString();
  for (auto s : sessionIndex)
    if (s.toMap()["workspace"] == path)
      setMetadata(s.toMap()["id"].toString(), {{"archived", value}});
  if (view->workspace == path)
    newSession();
  emit chatChanged();
}
void Backend::removeProject(QString id) {
  auto projects = store.data["projects"].toList();
  for (int i = projects.size() - 1; i >= 0; --i)
    if (projects[i].toMap()["id"] == id) {
      auto path = projects[i].toMap()["path"].toString();
      bool running = false;
      for (auto record : records)
        running |= record->busy() && record->workspace == path;
      if (running) {
        emit toast("项目仍有任务运行，请等待结束");
        return;
      }
      projects.removeAt(i);
    }
  store.data["projects"] = projects;
  auto removed = store.data["removedProjects"].toList();
  if (!removed.contains(id))
    removed << id;
  store.data["removedProjects"] = removed;
  if (store.data["activeProject"] == id) {
    store.data["activeProject"] = "";
    store.data["workspace"] = "";
    activate(fresh());
  }
  store.save();
  emit chatChanged();
}
void Backend::openProject(QString id) {
  for (auto v : store.data["projects"].toList())
    if (v.toMap()["id"] == id) {
      setWorkspace(v.toMap()["path"].toString());
      return;
    }
}
void Backend::revealProject(QString id) {
  for (auto v : store.data["projects"].toList())
    if (v.toMap()["id"] == id)
      QDesktopServices::openUrl(
          QUrl::fromLocalFile(v.toMap()["path"].toString()));
}
void Backend::renameProject(QString id, QString title) {
  title = title.trimmed().left(100);
  if (title.isEmpty())
    return;
  auto rows = store.data["projects"].toList();
  for (auto &v : rows) {
    auto p = v.toMap();
    if (p["id"] == id) {
      p["name"] = title;
      v = p;
    }
  }
  store.data["projects"] = rows;
  store.save();
  emit changed();
  emit chatChanged();
}
void Backend::toggleProject(QString id) {
  if (collapsed.contains(id))
    collapsed.remove(id);
  else
    collapsed.insert(id);
  emit chatChanged();
}
void Backend::dropProjects(QString raw) {
  for (auto v : QJsonDocument::fromJson(raw.toUtf8()).array())
    if (QFileInfo(U::localPath(v.toString())).isDir())
      setWorkspace(v.toString());
}
void Backend::addAttachments(QString raw) {
  for (auto v : QJsonDocument::fromJson(raw.toUtf8()).array()) {
    auto path = U::cleanPath(v.toString());
    QFileInfo file(path);
    if (!file.exists() || path.contains('\n'))
      continue;
    bool present = false;
    for (auto item : view->attachments)
      if (item.toMap()["path"] == path)
        present = true;
    if (present)
      continue;
    if (view->attachments.size() >= 20) {
      emit toast("最多添加 20 个附件");
      break;
    }
    QVariantMap item{{"id", U::uuid()},
                     {"name", file.fileName()},
                     {"path", path},
                     {"kind", file.isDir() ? "folder" : "file"}};
    QImageReader reader(path);
    if (file.isFile() && reader.canRead() && file.size() <= 10 * 1024 * 1024) {
      auto size = reader.size();
      if (size.width() > 0 && size.height() > 0 &&
          qint64(size.width()) * size.height() <= 40000000) {
        QFile input(path);
        if (input.open(QIODevice::ReadOnly)) {
          item["kind"] = "image";
          item["data"] = QString::fromLatin1(input.readAll().toBase64());
          auto format = reader.format();
          item["mime"] =
              QString("image/%1")
                  .arg(format == "jpg" ? "jpeg" : QString::fromLatin1(format));
        }
      }
    }
    view->attachments << item;
  }
  emit attachmentsChanged();
}
bool Backend::pasteAttachments() {
  auto mime = QGuiApplication::clipboard()->mimeData();
  if (mime->hasUrls()) {
    QVariantList urls;
    for (auto u : mime->urls())
      urls << u.toString();
    addAttachments(QString::fromUtf8(
        QJsonDocument::fromVariant(urls).toJson(QJsonDocument::Compact)));
    return true;
  }
  if (mime->hasImage()) {
    auto image = QGuiApplication::clipboard()->image();
    if (image.isNull() || image.width() * qint64(image.height()) > 40000000)
      return false;
    auto path = store.directory + "/attachments/clipboard-" +
                U::uuid().left(12) + ".png";
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (image.save(path)) {
      addAttachments(QString::fromUtf8(
          QJsonDocument(QJsonArray{QUrl::fromLocalFile(path).toString()})
              .toJson(QJsonDocument::Compact)));
      return true;
    }
  }
  return false;
}
void Backend::removeAttachment(QString id) {
  for (int i = view->attachments.size() - 1; i >= 0; --i)
    if (view->attachments[i].toMap()["id"] == id)
      view->attachments.removeAt(i);
  emit attachmentsChanged();
}
void Backend::selectModel(QString id, QString model) {
  if (busy()) {
    emit toast("请等待当前任务结束后切换模型");
    return;
  }
  for (auto v : modelCatalog()) {
    auto m = v.toMap();
    if (m["provider"] == id && m["id"] == model) {
      store.data["selectedProvider"] = id;
      store.data["selectedModel"] = model;
      view->provider = id;
      view->model = model;
      view->usage.clear();
      store.save();
      emit changed();
      emit contextUsageChanged();
      return;
    }
  }
  emit toast("请选择已配置的模型");
}
bool Backend::saveProvider(QString id, QString name, QString api, QString url,
                           QString key, QString raw) {
  if (modelsBusy(id)) {
    emit toast("该供应商仍有任务运行");
    return false;
  }
  try {
    QUrl base(url.trimmed());
    if (!base.isValid() ||
        !QStringList{"http", "https"}.contains(base.scheme()) ||
        base.host().isEmpty() || !base.userInfo().isEmpty() ||
        base.hasQuery() || base.hasFragment())
      throw std::runtime_error("请输入有效的 HTTP / HTTPS API 地址");
    if (!QStringList{"openai", "anthropic"}.contains(api) ||
        name.trimmed().isEmpty())
      throw std::runtime_error("请填写供应商名称和协议");
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(raw.toUtf8(), &error);
    if (!doc.isArray() || doc.array().isEmpty())
      throw std::runtime_error("请添加至少一个模型");
    QSet<QString> aliases;
    for (auto pv : store.data["providers"].toList())
      if (pv.toMap()["id"] != id)
        for (auto m : pv.toMap()["models"].toList())
          aliases.insert(m.toMap()["alias"].toString());
    QVariantList models;
    auto old = provider(id);
    for (auto value : doc.array()) {
      auto m = value.toObject().toVariantMap();
      m["alias"] = m["alias"].toString().trimmed();
      m["upstream"] = m["upstream"].toString().trimmed();
      auto alias = m["alias"].toString();
      if (alias.isEmpty() || m["upstream"].toString().isEmpty() ||
          aliases.contains(alias) || alias.contains(QRegularExpression("\\s")))
        throw std::runtime_error("模型名称不能为空，别名不能重复或含空白");
      aliases.insert(alias);
      QVariantMap previous;
      for (auto v : old["models"].toList())
        if (v.toMap()["upstream"] == m["upstream"])
          previous = v.toMap();
      models << U::normalizeModel(m, previous);
    }
    if (id.isEmpty())
      id = U::uuid();
    bool hasKey = old.value("hasKey", false).toBool();
    if (!key.isEmpty()) {
      if (!store.setSecret("provider:" + id, key.toUtf8()))
        throw std::runtime_error("无法保存供应商凭据，请检查系统密钥环");
      hasKey = true;
    }
    QVariantMap record{
        {"id", id},
        {"name", name.trimmed().left(100)},
        {"api", api},
        {"baseUrl", base.toString().remove(QRegularExpression("/+$"))},
        {"hasKey", hasKey},
        {"proxy", old.value("proxy", QVariantMap{})},
        {"models", models}};
    auto rows = store.data["providers"].toList();
    bool replaced = false;
    for (auto &v : rows)
      if (v.toMap()["id"] == id) {
        v = record;
        replaced = true;
      }
    if (!replaced)
      rows << record;
    store.data["providers"] = rows;
    invalidate();
    notify();
    return true;
  } catch (const std::exception &e) {
    emit toast(QString::fromUtf8(e.what()));
    return false;
  }
}
void Backend::removeProvider(QString id) {
  if (provider(id)["api"] == "plugin") {
    providers->remove(id);
    return;
  }
  if (modelsBusy(id)) {
    emit toast("该供应商仍有任务运行");
    return;
  }
  auto rows = store.data["providers"].toList();
  for (int i = rows.size() - 1; i >= 0; --i)
    if (rows[i].toMap()["id"] == id)
      rows.removeAt(i);
  store.data["providers"] = rows;
  store.setSecret("provider:" + id, {});
  invalidate();
  notify();
}
void Backend::saveMappings(QString raw) {
  if (modelsBusy()) {
    emit toast("请等待任务结束后修改模型映射");
    return;
  }
  try {
    QSet<QString> aliases;
    auto input = QJsonDocument::fromJson(raw.toUtf8()).array().toVariantList();
    auto rows = store.data["providers"].toList();
    for (auto &v : rows) {
      auto p = v.toMap();
      QVariantList models;
      QSet<QString> oldIds;
      for (auto old : p["models"].toList())
        oldIds.insert(old.toMap()["upstream"].toString());
      for (auto iv : input) {
        auto m = iv.toMap();
        if (m["provider"] != p["id"])
          continue;
        auto alias = m["alias"].toString().trimmed(),
             upstream = m["upstream"].toString().trimmed();
        if (alias.isEmpty() || upstream.isEmpty() || aliases.contains(alias) ||
            alias.contains(QRegularExpression("\\s")))
          throw std::runtime_error("模型别名不能为空、含空白或重复");
        aliases.insert(alias);
        m["alias"] = alias;
        m["upstream"] = upstream;
        models << U::normalizeModel(m);
      }
      QSet<QString> ids;
      for (auto m : models)
        ids.insert(m.toMap()["upstream"].toString());
      if ((models.isEmpty() && !oldIds.isEmpty()) ||
          (p["api"] == "plugin" && ids != oldIds))
        throw std::runtime_error(
            "保留至少一个模型；插件模型 ID 请通过刷新获取");
      p["models"] = models;
      v = p;
    }
    store.data["providers"] = rows;
    invalidate();
    notify();
    emit toast("模型映射已保存");
  } catch (const std::exception &e) {
    emit toast(QString::fromUtf8(e.what()));
  }
}
QVariantList Backend::proxyKeys() const {
  QVariantList out;
  for (auto v : store.data["server"].toMap()["keys"].toList()) {
    auto k = v.toMap();
    k["maskedKey"] = k["hint"];
    k.remove("digest");
    k.remove("hint");
    out << k;
  }
  return out;
}
QStringList Backend::proxyAddresses() const {
  QStringList out{"127.0.0.1"};
  if (!store.data["server"].toMap()["lan"].toBool())
    return out;
  for (auto iface : QNetworkInterface::allInterfaces()) {
    if (!(iface.flags() & QNetworkInterface::IsUp))
      continue;
    for (auto entry : iface.addressEntries()) {
      auto ip = entry.ip();
      if (ip.protocol() == QAbstractSocket::IPv4Protocol && !ip.isLoopback() &&
          !out.contains(ip.toString()))
        out << ip.toString();
    }
  }
  return out;
}
QString Backend::generateProxyKey() {
  return "umeko-" +
         QString::fromLatin1(U::random(32).toBase64(
             QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
QVariantMap Backend::takeCreatedProxyKey() {
  auto value = createdKey;
  createdKey.clear();
  return value;
}
bool Backend::saveProxyKey(QString id, QString name, QString value) {
  name = name.trimmed();
  value = value.trimmed();
  auto server = store.data["server"].toMap();
  auto rows = server["keys"].toList();
  if (name.isEmpty() || name.size() > 80 ||
      name.contains(QRegularExpression("[\\x00-\\x1f]"))) {
    emit toast("Key 名称需为 1–80 个字符");
    return false;
  }
  if (id.isEmpty() && rows.size() >= 100) {
    emit toast("最多创建 100 个 Key");
    return false;
  }
  QVariantMap previous;
  for (auto row : rows)
    if (row.toMap()["id"] == id)
      previous = row.toMap();
  if (id.isEmpty() && value.isEmpty())
    value = generateProxyKey();
  if (!value.isEmpty() && !QRegularExpression("^[A-Za-z0-9._~-]{16,256}$")
                               .match(value)
                               .hasMatch()) {
    emit toast("Key 需为 16–256 位字母、数字或 . _ ~ -");
    return false;
  }
  auto digest = value.isEmpty() ? previous["digest"].toString()
                                : U::sha(value.toLatin1());
  for (auto row : rows) {
    auto k = row.toMap();
    if (k["id"] != id && (k["name"] == name || k["digest"] == digest)) {
      emit toast("Key 名称或值已存在");
      return false;
    }
  }
  bool fresh = id.isEmpty();
  if (fresh)
    id = U::uuid();
  auto record = previous;
  record["id"] = id;
  record["name"] = name;
  record["digest"] = digest;
  record["hint"] = value.isEmpty() ? previous["hint"]
                                   : QVariant("••••••••" + value.right(4));
  record["enabled"] = previous.value("enabled", true);
  record["createdAt"] = previous.value("createdAt", U::stamp());
  record["lastUsedAt"] = previous.value("lastUsedAt", "");
  if (fresh)
    rows << record;
  else
    for (auto &row : rows)
      if (row.toMap()["id"] == id)
        row = record;
  server["keys"] = rows;
  store.data["server"] = server;
  createdKey = value.isEmpty()
                   ? QVariantMap{}
                   : QVariantMap{{"id", id}, {"name", name}, {"key", value}};
  notify();
  return true;
}
void Backend::setProxyKeyEnabled(QString id, bool enabled) {
  auto server = store.data["server"].toMap();
  auto rows = server["keys"].toList();
  for (auto &v : rows) {
    auto k = v.toMap();
    if (k["id"] == id) {
      k["enabled"] = enabled;
      v = k;
    }
  }
  server["keys"] = rows;
  store.data["server"] = server;
  notify();
}
void Backend::revokeProxyKey(QString id) {
  auto server = store.data["server"].toMap();
  auto rows = server["keys"].toList();
  for (int i = rows.size() - 1; i >= 0; --i)
    if (rows[i].toMap()["id"] == id)
      rows.removeAt(i);
  server["keys"] = rows;
  store.data["server"] = server;
  notify();
}
bool Backend::saveServer(int port, bool openai, bool anthropic, bool lan) {
  if (port < 1024 || port > 65535 || (!openai && !anthropic)) {
    emit toast("端口范围为 1024–65535，至少启用一种协议");
    return false;
  }
  auto server = store.data["server"].toMap();
  auto before = server;
  server["port"] = port;
  server["openai"] = openai;
  server["anthropic"] = anthropic;
  server["lan"] = lan;
  store.data["server"] = server;
  if (!store.save()) {
    store.data["server"] = before;
    return false;
  }
  gatewayWrite(
      {{"type", "server"}, {"enabled", publicRunning}, {"settings", server}});
  emit changed();
  return true;
}
void Backend::toggleServer() {
  if (!gatewayPort) {
    emit toast("请先安装 Node.js 22.19+ 并重新加载");
    return;
  }
  bool enabled = false;
  for (auto k : store.data["server"].toMap()["keys"].toList())
    enabled |= k.toMap()["enabled"].toBool();
  if (!publicRunning && !enabled) {
    emit toast("请先创建并启用至少一个 API Key");
    return;
  }
  gatewayWrite({{"type", "server"},
                {"enabled", !publicRunning},
                {"settings", store.data["server"]}});
}
void Backend::gatewayStart() {
  if (gateway || closing)
    return;
  auto program = U::node();
  if (program.isEmpty()) {
    kernelMessage = "需要 Node.js 22.19+";
    emit changed();
    return;
  }
  gateway = new QProcess(this);
  U::hideConsole(gateway);
  gateway->setProcessEnvironment(U::environment());
  auto networkEnv = U::environment();
  const auto systemEnv = QProcessEnvironment::systemEnvironment();
  for (const auto &key : systemEnv.keys())
    if (key.toUpper().endsWith("_PROXY"))
      networkEnv.insert(key, systemEnv.value(key));
  gateway->setProcessEnvironment(networkEnv);
  gateway->setProgram(program);
  gateway->setArguments({root + "/runtime/gateway.mjs"});
  connect(gateway, &QProcess::started, this, [this] { configureGateway(); });
  connect(gateway, &QProcess::readyReadStandardOutput, this, [this] {
    gatewayBuffer += gateway->readAllStandardOutput();
    while (gatewayBuffer.contains('\n')) {
      auto i = gatewayBuffer.indexOf('\n');
      auto line = gatewayBuffer.left(i);
      gatewayBuffer.remove(0, i + 1);
      gatewayEvent(QJsonDocument::fromJson(line).object().toVariantMap());
    }
  });
  connect(gateway, &QProcess::readyReadStandardError, this,
          [this] { gateway->readAllStandardError(); });
  connect(gateway, &QProcess::finished, this, [this] {
    if (closing)
      return;
    gatewayPort = 0;
    publicRunning = false;
    kernelMessage = "模型网关已退出，请重新加载 Agent";
    auto old = gateway;
    gateway = nullptr;
    old->deleteLater();
    emit changed();
    emit toast(kernelMessage);
  });
  gateway->start();
}
void Backend::gatewayWrite(QVariantMap payload) {
  if (gateway && gateway->state() == QProcess::Running)
    gateway->write(
        QJsonDocument::fromVariant(payload).toJson(QJsonDocument::Compact) +
        '\n');
}
void Backend::configureGateway() {
  QVariantList rows;
  for (auto v : store.data["providers"].toList()) {
    auto p = v.toMap();
    if (p["hasKey"].toBool())
      p["key"] =
          QString::fromUtf8(store.secret("provider:" + p["id"].toString()));
    if (p["proxy"].toMap().value("mode", "system") == "system") {
      const auto choices = QNetworkProxyFactory::systemProxyForQuery(
          QNetworkProxyQuery(QUrl(p["baseUrl"].toString())));
      for (const auto &proxy : choices)
        if (proxy.type() == QNetworkProxy::HttpProxy &&
            !proxy.hostName().isEmpty()) {
          QUrl address;
          address.setScheme("http");
          address.setHost(proxy.hostName());
          address.setPort(proxy.port());
          p["systemProxy"] = address.toString();
          break;
        }
    }
    rows << p;
  }
  gatewayWrite({{"type", "configure"},
                {"token", privateToken},
                {"diagnosticsDir", store.directory + "/request-errors"},
                {"providers", rows},
                {"models", modelCatalog()},
                {"settings", store.data["server"]}});
}
void Backend::gatewayEvent(QVariantMap payload) {
  auto type = payload["type"].toString();
  if (type == "ready") {
    gatewayPort = payload["port"].toInt();
    kernelMessage = "就绪";
    emit changed();
  } else if (type == "provider") {
    providers->complete(payload["id"].toString(),
                        payload["provider"].toString(),
                        payload["body"].toMap());
  } else if (type == "provider-cancel") {
    providers->cancel(payload["id"].toString());
  } else if (type == "server") {
    publicRunning = payload["running"].toBool();
    if (!payload.value("ok", true).toBool()) {
      auto actual = payload["settings"].toMap();
      auto server = store.data["server"].toMap();
      for (auto key : {"port", "lan", "openai", "anthropic"})
        if (actual.contains(key))
          server[key] = actual[key];
      store.data["server"] = server;
      store.save();
      emit toast(payload.value("error", "代理服务启动失败").toString());
    }
    emit changed();
  } else if (type == "key-used") {
    auto server = store.data["server"].toMap();
    auto keys = server["keys"].toList();
    for (auto &v : keys) {
      auto k = v.toMap();
      if (k["id"] == payload["id"]) {
        k["lastUsedAt"] = payload["at"];
        v = k;
      }
    }
    server["keys"] = keys;
    store.data["server"] = server;
    emit proxyKeysChanged();
  }
}
void Backend::close() {
  if (closing)
    return;
  closing = true;
  previewLoader->cancel();
  streaming.stop();
  providers->close();
  for (auto record : records)
    if (record->process) {
      writePi(record, {{"type", "shutdown"}});
      record->process->closeWriteChannel();
    }
  for (auto record : records)
    if (record->process && !record->process->waitForFinished(2500)) {
      record->process->terminate();
      if (!record->process->waitForFinished(1000)) {
        record->process->kill();
        record->process->waitForFinished(1000);
      }
    }
  if (gateway) {
    gatewayWrite({{"type", "shutdown"}});
    gateway->closeWriteChannel();
    if (!gateway->waitForFinished(2500)) {
      gateway->kill();
      gateway->waitForFinished(1000);
    }
  }
  store.save();
  saveSessions();
}
void Backend::injectDemo() {
  auto path = store.directory + "/umeko-notes";
  QDir().mkpath(path);
  auto p = ensureProject(path, true);
  store.data["workspace"] = path;
  store.data["activeProject"] = p["id"];
  store.data["theme"] = "yanami";
  store.data["dark"] = false;
  store.data["providers"] = QVariantList{QVariantMap{
      {"id", "demo"},
      {"api", "openai"},
      {"name", "My Provider"},
      {"baseUrl", "https://api.example.com/v1"},
      {"models", QVariantList{QVariantMap{{"upstream", "workspace-model"},
                                          {"alias", "workspace-model"},
                                          {"contextWindow", 262144},
                                          {"contextWindowAuto", false},
                                          {"vision", true},
                                          {"visionAuto", false}}}}}};
  store.data["selectedProvider"] = "demo";
  store.data["selectedModel"] = "workspace-model";
  view->id = "documentation-demo";
  view->workspace = path;
  view->title = "搭建一个轻量文档站";
  view->provider = "demo";
  view->model = "workspace-model";
  view->usage = {{"tokens", 18432},
                 {"contextWindow", 262144},
                 {"percent", 7.03125},
                 {"model", "workspace-model"}};
  view->messages = {
      QVariantMap{
          {"role", "user"},
          {"text",
           "帮我把笔记项目整理成一个轻量文档站，保留 Markdown，并加上搜索。"},
          {"error", false}},
      QVariantMap{
          {"role", "assistant"},
          {"thinking", "先检查现有目录，把内容与站点配置分开。保留 Markdown "
                       "源文件，使用静态索引，便于离线预览和后续维护。"},
          {"text", "我先检查笔记目录，再整理导航和搜索配置。"},
          {"tools",
           QVariantList{
               QVariantMap{
                   {"id", "read-notes"},
                   {"name", "ls"},
                   {"arguments", QVariantMap{{"path", "notes"}}},
                   {"state", "done"},
                   {"result", "getting-started.md\narchitecture.md\nideas.md"}},
               QVariantMap{
                   {"id", "write-site"},
                   {"name", "write"},
                   {"arguments", QVariantMap{{"path", "site.config.ts"}}},
                   {"path", "/workspace/umeko-notes/site.config.ts"},
                   {"state", "done"},
                   {"result", "Site configuration saved."}},
               QVariantMap{
                   {"id", "check-site"},
                   {"name", "bash"},
                   {"arguments", QVariantMap{{"command", "npm run build"}}},
                   {"state", "done"},
                   {"result",
                    "Static pages and local search index generated."}}}}},
      QVariantMap{
          {"role", "assistant"},
          {"error", false},
          {"text",
           "文档站的结构已经整理好了。\n\n- **内容保留**：继续在 `notes/` "
           "中编辑 Markdown。\n- "
           "**本地搜索**：构建时生成索引，预览时无需外部服务。\n- "
           "**清晰导航**：入门、架构与想法分开管理。\n\n```typescript\nexport "
           "default {\n  title: 'Umeko Notes',\n  contentDir: './notes',\n  "
           "search: { provider: 'local' },\n}\n```\n\n运行 `npm run dev` "
           "即可预览，下一步可以继续调整首页。"}}};
  indexSession(view);
  configureGateway();
  emit changed();
  emit chatChanged();
  emit contextUsageChanged();
  emit viewChanged();
}
