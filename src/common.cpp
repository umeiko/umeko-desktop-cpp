#include "common.hpp"
#include <QImageReader>
#include <stdexcept>

QString U::uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString U::stamp() {
  return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
QByteArray U::random(int count) {
  QByteArray raw(count, '\0');
  for (int i = 0; i < count; ++i)
    raw[i] = char(QRandomGenerator::system()->generate() & 255);
  return raw;
}
QString U::sha(const QByteArray &raw) {
  return QString::fromLatin1(
      QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex());
}
QString U::localPath(QString path) {
  return path.startsWith("file:") ? QUrl(path).toLocalFile() : path;
}
QString U::cleanPath(QString path) {
  path = localPath(path);
  if (path.startsWith("~/"))
    path = QDir::homePath() + path.mid(1);
  QFileInfo f(path);
  return QDir::cleanPath(f.exists() ? f.canonicalFilePath()
                                    : f.absoluteFilePath());
}
QString U::node() {
#ifdef Q_OS_WIN
  auto bundled =
      QCoreApplication::applicationDirPath() + "/runtime/node/node.exe";
#elif defined(Q_OS_MACOS)
  auto bundled = QCoreApplication::applicationDirPath() +
                 "/../Resources/runtime/node/bin/node";
#else
  auto bundled =
      QCoreApplication::applicationDirPath() + "/runtime/node/bin/node";
#endif
  if (QFileInfo(bundled).isFile() && QFileInfo(bundled).isExecutable())
    return cleanPath(bundled);
  auto path = QStandardPaths::findExecutable("node");
  if (path.isEmpty())
    for (auto p : {"/opt/homebrew/bin/node", "/usr/local/bin/node"})
      if (QFileInfo(p).isExecutable())
        return p;
  return path;
}
QString U::python(QString configured) {
  if (!configured.trimmed().isEmpty()) {
    auto path = localPath(configured.trimmed());
    if (path.startsWith("~/"))
      path = QDir::homePath() + path.mid(1);
    return QFileInfo(path).isFile() && QFileInfo(path).isExecutable()
               ? cleanPath(path)
               : QStandardPaths::findExecutable(path);
  }
  for (auto name : {"python3", "python"}) {
    auto p = QStandardPaths::findExecutable(name);
    if (!p.isEmpty() && !p.contains("WindowsApps", Qt::CaseInsensitive))
      return p;
  }
  for (auto path : {"/opt/homebrew/bin/python3", "/usr/local/bin/python3"})
    if (QFileInfo(path).isExecutable())
      return path;
  return {};
}
QVariant U::readJson(const QString &file, QVariant fallback) {
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly))
    return fallback;
  QJsonParseError error;
  auto doc = QJsonDocument::fromJson(f.readAll(), &error);
  return error.error == QJsonParseError::NoError ? doc.toVariant() : fallback;
}
bool U::writeJson(const QString &file, const QVariant &data, QString *error) {
  QDir().mkpath(QFileInfo(file).absolutePath());
  QSaveFile f(file);
  if (!f.open(QIODevice::WriteOnly)) {
    if (error)
      *error = f.errorString();
    return false;
  }
  auto raw = QJsonDocument::fromVariant(data).toJson(QJsonDocument::Indented);
  if (f.write(raw) != raw.size() || !f.commit()) {
    if (error)
      *error = f.errorString();
    return false;
  }
  return true;
}
bool U::copyTree(const QString &source, const QString &dest, QString *error) {
  QDir dir(source);
  if (!dir.exists()) {
    if (error)
      *error = "Source directory is missing";
    return false;
  }
  QDir().mkpath(dest);
  for (const auto &entry :
       dir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
    auto target = QDir(dest).filePath(entry.fileName());
    if (entry.isSymLink()) {
      if (error)
        *error = "Linked source directory is unsupported";
      return false;
    }
    if (entry.isDir()) {
      if (!copyTree(entry.filePath(), target, error))
        return false;
    } else if (!QFileInfo::exists(target) &&
               !QFile::copy(entry.filePath(), target)) {
      if (error)
        *error = "Could not copy " + entry.fileName();
      return false;
    }
  }
  return true;
}
QVariantMap U::validateNpmConfig(QVariantMap values, QString *error) {
  QVariantMap result;
  for (auto key : {"npmRegistry", "npmUserconfig", "npmCafile"}) {
    auto value = values[key].toString().trimmed();
    if (value.contains('\n') || value.contains('\r') ||
        value.contains(QChar(0))) {
      *error = "npm 配置不能包含换行或空字符";
      return {};
    }
    if (QString(key) == "npmRegistry" && !value.isEmpty()) {
      QUrl url(value, QUrl::StrictMode);
      if (!url.isValid() ||
          !QStringList{"http", "https"}.contains(url.scheme()) ||
          url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() ||
          url.hasFragment()) {
        *error = "npm 源须为完整的 HTTP(S) 地址；登录凭据请放在 .npmrc 中";
        return {};
      }
    } else if (QString(key) != "npmRegistry" && !value.isEmpty()) {
      value = cleanPath(value);
      if (!QFileInfo(value).isFile() || !QFileInfo(value).isReadable()) {
        *error = "npm 配置文件或 CA 证书不存在或不可读";
        return {};
      }
    }
    result[key] = value;
  }
  return result;
}
QMap<QString, QString> U::npmOverrides(QVariantMap values) {
  QMap<QString, QString> result;
  for (auto key : {"npmRegistry", "npmUserconfig", "npmCafile"}) {
    auto value = values[key].toString();
    if (!value.isEmpty()) {
      QString environment =
          QString(key) == "npmRegistry"     ? "npm_config_registry"
          : QString(key) == "npmUserconfig" ? "npm_config_userconfig"
                                            : "npm_config_cafile";
      result[environment] = value;
    }
  }
  return result;
}
QProcessEnvironment U::environment(QVariantMap npm) {
  auto env = QProcessEnvironment::systemEnvironment();
  for (auto key : env.keys()) {
    auto k = key.toUpper();
    if (k == "NODE_OPTIONS" || k == "NODE_PATH" || k.endsWith("API_KEY") ||
        k.endsWith("ACCESS_TOKEN"))
      env.remove(key);
  }
  env.remove("PI_OFFLINE");
  const auto overrides = npmOverrides(npm);
  for (auto it = overrides.begin(); it != overrides.end(); ++it) {
    for (auto key : env.keys())
      if (key.compare(it.key(), Qt::CaseInsensitive) == 0)
        env.remove(key);
    env.insert(it.key(), it.value());
  }
  auto executable = node();
  if (!executable.isEmpty())
    env.insert("PATH", QFileInfo(executable).absolutePath() +
                           QDir::listSeparator() + env.value("PATH"));
  return env;
}
void U::hideConsole(QProcess *process) {
#ifdef Q_OS_WIN
  process->setCreateProcessArgumentsModifier(
      [](QProcess::CreateProcessArguments *args) {
        args->flags |= 0x08000000;
      });
#else
  Q_UNUSED(process)
#endif
}
void U::run(QObject *owner, const QString &program, const QStringList &args,
            const QByteArray &input,
            std::function<void(bool, QByteArray, QString)> done, int timeout,
            const QString &cwd, QVariantMap npm) {
  auto process = new QProcess(owner);
  hideConsole(process);
  auto timer = new QTimer(process);
  timer->setSingleShot(true);
  auto delivered = std::make_shared<bool>(false);
  auto finish = [=](bool ok, QString error) {
    if (*delivered)
      return;
    *delivered = true;
    timer->stop();
    done(ok, process->readAllStandardOutput(), error);
    process->deleteLater();
  };
  process->setProcessEnvironment(environment(npm));
  if (!cwd.isEmpty())
    process->setWorkingDirectory(cwd);
  QObject::connect(process, &QProcess::started, process, [=] {
    if (!input.isNull())
      process->write(input);
    process->closeWriteChannel();
  });
  QObject::connect(process, &QProcess::finished, process,
                   [=](int code, QProcess::ExitStatus status) {
                     finish(code == 0 && status == QProcess::NormalExit,
                            code ? QString("进程未成功完成（%1）").arg(code)
                                 : QString());
                   });
  QObject::connect(process, &QProcess::errorOccurred, process,
                   [=](QProcess::ProcessError error) {
                     if (error == QProcess::FailedToStart)
                       finish(false, "无法启动运行时，请检查可执行文件路径");
                   });
  QObject::connect(timer, &QTimer::timeout, process, [=] {
    process->kill();
    finish(false, "操作超时，请检查运行时或连接配置");
  });
  timer->start(timeout);
  process->start(program, args);
}
int U::parseContext(const QVariant &value) {
  if (value.metaType().id() == QMetaType::Bool)
    return 0;
  auto m = QRegularExpression("^\\s*(\\d+(?:\\.\\d+)?)\\s*([kKmM]?)\\s*$")
               .match(value.toString());
  if (!m.hasMatch())
    return 0;
  double n = m.captured(1).toDouble();
  auto unit = m.captured(2).toLower();
  n *= unit == "k" ? 1024 : unit == "m" ? 1048576 : 1;
  return n >= 1024 && n <= 16777216 && std::floor(n) == n ? int(n) : 0;
}
QVariantMap U::contextSuggestion(QString name) {
  name = name.trimmed().toLower().replace(QRegularExpression("[\\s_/:]+"), "-");
  auto capacities = QRegularExpression("(?:^|-)(\\d+(?:\\.\\d+)?)([km])(?=$|-)")
                        .globalMatch(name);
  int count = 0;
  while (capacities.hasNext()) {
    auto m = capacities.next();
    auto n = parseContext(m.captured(1) + m.captured(2));
    if (n)
      count = n;
  }
  if (count)
    return {{"tokens", count}, {"matched", true}};
  const QList<QPair<QString, int>> rules = {
      {"(?:^|-)k3-256k?(?=$|-)", 262144},
      {"(?:^|-)k3(?=$|-(?!\\d))", 1048576},
      {"(?:^|-)kimi-for-coding-highspeed(?=$|-)", 262144},
      {"(?:^|-)kimi-for-coding(?=$|-)", 1048576},
      {"(?:^|-)kimi-k2\\.[57](?=$|-)", 262144},
      {"(?:^|-)kimi-k2\\.8(?=$|-)", 1048576},
      {"(?:^|-)deepseek-v4(?:\\.1)?-(?:flash|pro)(?=$|-)", 1048576},
      {"(?:^|-)gpt-4\\.1(?=$|-)", 1047576},
      {"(?:^|-)gpt-4o(?=$|-)", 128000},
      {"(?:^|-)claude-(?:sonnet|opus)-4-6(?=$|-)", 1000000},
      {"(?:^|-)claude-(?:sonnet|opus|haiku)-4-5(?=$|-)", 200000},
      {"(?:^|-)gemini-2\\.5-(?:pro|flash)(?=$|-)", 1048576}};
  for (auto rule : rules)
    if (QRegularExpression(rule.first).match(name).hasMatch())
      return {{"tokens", rule.second}, {"matched", true}};
  return {{"tokens", 128000}, {"matched", false}};
}
QVariantMap U::visionSuggestion(QString name) {
  name = name.toLower().replace(QRegularExpression("[\\s_/:]+"), "-");
  if (QRegularExpression("(?:^|-)(embedding\\w*|rerank\\w*|tts|transcrib\\w*|"
                         "whisper|audio|realtime)(?=$|-)")
          .match(name)
          .hasMatch())
    return {{"vision", false}, {"matched", true}};
  auto positive = QRegularExpression(
      "(?:^|-)(vision|vl\\d*|internvl\\d*|llava|k3(?:-256k?)?|kimi-(?:k2\\.["
      "578]|for-coding(?:-highspeed)?)|gpt-(?:4o|4\\.1|4-turbo|4-vision|5(?:\\."
      "\\d+)?)|claude-(?:(?:3|4|5)(?:-[a-z\\d.]+)*|(?:sonnet|opus|haiku)-(?:3|"
      "4|5)(?:-[a-z\\d.]+)*)|gemini-(?:1\\.5|2(?:\\.\\d+)?|3(?:\\.\\d+)?)(?:-["
      "a-z\\d.]+)*|qwen3\\.[5678](?:-[a-z\\d.]+)*|glm-4(?:\\.\\d+)?v|glm-5\\.3-"
      "flash|deepseek-flash)(?=$|-)");
  bool yes = positive.match(name).hasMatch();
  return {
      {"vision", yes},
      {"matched", yes || QRegularExpression("deepseek|qwen|kimi-k2|gpt-4|glm")
                             .match(name)
                             .hasMatch()}};
}
QVariantMap U::normalizeModel(QVariantMap m, QVariantMap previous) {
  auto ctx = m.contains("contextWindow") || m.contains("contextWindowAuto")
                 ? m
                 : previous;
  bool automatic =
      ctx.value("contextWindowAuto", !ctx.contains("contextWindow")).toBool();
  int count = automatic ? contextSuggestion(m.value("upstream").toString())
                              .value("tokens")
                              .toInt()
                        : parseContext(ctx.value("contextWindow"));
  if (!count)
    throw std::runtime_error("上下文长度需为 1024–16777216，可填写 256K、1M");
  auto vis = m.contains("vision") || m.contains("visionAuto") ? m : previous;
  bool va = vis.value("visionAuto", !vis.contains("vision")).toBool();
  m["contextWindow"] = count;
  m["contextWindowAuto"] = automatic;
  m["visionAuto"] = va;
  m["vision"] =
      va ? visionSuggestion(m.value("upstream").toString()).value("vision")
         : vis.value("vision");
  return m;
}
QVariantList U::mediaView(QVariantList items) {
  static QCache<QString, QVariantMap> cache(256);
  QVariantList out;
  for (auto value : items.mid(0, 12)) {
    auto item = value.toMap();
    const auto path = item["path"].toString();
    const auto kind = item["kind"].toString();
    if (!QFileInfo(path).isAbsolute() || path.contains(QChar(0)) ||
        path.contains('\n') || path.contains('\r') ||
        !QStringList{"image", "file", "folder"}.contains(kind))
      continue;
    QFileInfo file(path);
    auto title = item.value("title", file.fileName()).toString().left(160);
    auto caption = item["caption"].toString().left(1000);
    const auto key = path + QChar(0) + kind + QChar(0) + title + QChar(0) +
                     caption + QChar(0) + item["modified"].toString();
    if (auto cached = cache.object(key)) {
      out << *cached;
      continue;
    }
    QVariantMap row{
        {"path", path},
        {"name", file.fileName()},
        {"kind", kind},
        {"title", title},
        {"caption", caption},
        {"bytes", file.size()},
        {"url", QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded)},
        {"preview", false},
        {"previewNote", ""}};
    if (kind == "image") {
      QImageReader reader(path);
      const auto size = reader.size();
      const bool preview = file.exists() && file.size() > 0 &&
                           file.size() <= 20 * 1024 * 1024 &&
                           size.width() > 0 && size.height() > 0 &&
                           qint64(size.width()) * size.height() <= 16000000;
      row["pixelWidth"] = size.width();
      row["pixelHeight"] = size.height();
      auto format = QString::fromLatin1(reader.format());
      row["mime"] = "image/" + (format == "jpg" ? QString("jpeg") : format);
      row["preview"] = preview;
      row["previewNote"] =
          preview ? "" : "图片过大或暂不可预览，可打开原文件查看";
    }
    cache.insert(key, new QVariantMap(row));
    out << row;
  }
  return out;
}
QVariantMap U::toolView(QString id, QString name, QVariantMap args, QString cwd,
                        QVariantMap result) {
  auto path =
      args.value("path", args.value("file_path", args.value("filePath")))
          .toString();
  if (path.contains("://"))
    path.clear();
  if (!path.isEmpty()) {
    if (!QFileInfo(path).isAbsolute())
      path = QDir(cwd).filePath(path);
    path = cleanPath(path);
  }
  QVariantMap row{{"id", id},
                  {"name", name},
                  {"arguments", args},
                  {"path", path},
                  {"state", result.isEmpty()                   ? "running"
                            : result.value("isError").toBool() ? "error"
                                                               : "done"},
                  {"result", result.value("text").toString().left(8192)}};
  auto payload = result["details"].toMap()["umekoChatbox"].toMap();
  if (name == "submit_to_chatbox" && !result["isError"].toBool() &&
      payload["version"].toInt() == 1 && !payload["items"].toList().isEmpty())
    row["media"] = payload["items"].toList().mid(0, 12);
  return row;
}
static QString textOf(QVariant content, QString type = "text",
                      QString field = "text") {
  if (content.metaType().id() == QMetaType::QString)
    return type == "text" ? content.toString() : QString();
  QStringList out;
  for (auto part : content.toList()) {
    auto p = part.toMap();
    if (p.value("type") == type)
      out << p.value(field).toString();
  }
  return out.join('\n');
}
QPair<QVariantList, QVariantList> U::preview(const QString &file) {
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  QString cwd = QFileInfo(file).absolutePath();
  QMap<QString, QVariantMap> entries;
  QString last;
  while (!f.atEnd()) {
    auto e = QJsonDocument::fromJson(f.readLine()).object().toVariantMap();
    if (e.value("type") == "session")
      cwd = e.value("cwd", cwd).toString();
    else if (e.contains("id")) {
      last = e["id"].toString();
      entries[last] = e;
    }
  }
  QList<QVariantMap> branch;
  QSet<QString> seen;
  while (entries.contains(last) && !seen.contains(last)) {
    seen.insert(last);
    auto e = entries[last];
    branch.prepend(e);
    last = e["parentId"].toString();
  }
  QMap<QString, QVariantMap> results;
  QVariantList deliveries;
  QSet<QString> retryOmissions;
  for (auto e : branch) {
    if (e["type"] == "context_edit" && e["replacement"].isNull())
      retryOmissions.insert(e["targetId"].toString());
    auto data = e["data"].toMap();
    if (e["type"] == "custom" && e["customType"] == "umeko-chatbox" &&
        data["version"].toInt() == 1 &&
        !data["toolCallId"].toString().isEmpty())
      deliveries << data;
    auto m = e["message"].toMap();
    if (m["role"] == "toolResult") {
      m["text"] = textOf(m["content"]);
      results[m["toolCallId"].toString()] = m;
    }
  }
  QVariantList rows, tree;
  for (auto e : branch) {
    if (e["type"] != "message")
      continue;
    auto m = e["message"].toMap();
    auto content = m["content"];
    if (retryOmissions.contains(e["id"].toString()) &&
        m["role"] == "assistant" && m["stopReason"] == "error")
      continue;
    auto text = textOf(content);
    if (text.isEmpty() && m["stopReason"] != "aborted")
      text = m["errorMessage"].toString();
    auto thinking = textOf(content, "thinking", "thinking");
    tree << QVariantMap{
        {"id", e["id"]}, {"role", m["role"]}, {"text", text.left(80)}};
    if (m["role"] != "user" && m["role"] != "assistant")
      continue;
    QVariantList tools;
    for (auto v : content.toList()) {
      auto c = v.toMap();
      if (c["type"] != "toolCall")
        continue;
      auto id = c["id"].toString();
      auto tool = toolView(id, c["name"].toString(), c["arguments"].toMap(),
                           cwd, results.value(id));
      if (!results.contains(id))
        tool["state"] = "interrupted";
      tools << tool;
      for (auto delivery : deliveries) {
        auto data = delivery.toMap();
        if (data["toolCallId"].toString().startsWith(id + "/"))
          tools << QVariantMap{{"id", data["toolCallId"]},
                               {"name", "submit_to_chatbox"},
                               {"arguments", QVariantMap{}},
                               {"path", ""},
                               {"state", "done"},
                               {"result", "已提交到聊天框"},
                               {"media", data["items"].toList().mid(0, 12)}};
      }
    }
    if (text.isEmpty() && thinking.isEmpty() && tools.isEmpty())
      continue;
    QVariantMap row{{"role", m["role"]},
                    {"text", text},
                    {"entryId", e["id"]},
                    {"timestamp", m.value("timestamp", e["timestamp"])},
                    {"error", m["stopReason"] != "aborted" &&
                                  !m["errorMessage"].toString().isEmpty()}};
    if (m["role"] == "assistant")
      row["stopReason"] = m.value("stopReason", "");
    if (!thinking.isEmpty())
      row["thinking"] = thinking;
    if (!tools.isEmpty())
      row["tools"] = tools;
    rows << row;
  }
  return {rows, tree};
}
QString U::attachmentPrompt(QString text, QVariantList items) {
  if (items.isEmpty())
    return text;
  QStringList lines;
  for (auto v : items) {
    auto item = v.toMap();
    lines << "- " + item["kind"].toString() + ": " + item["path"].toString();
  }
  return (text.isEmpty() ? QString("请查看这些附件。") : text) +
         "\n\n附件（用户提供的路径；按需使用原生工具读取）：\n" +
         lines.join('\n');
}
QPair<QString, QVariantList> U::splitAttachments(QString text) {
  auto match =
      QRegularExpression("\\n?<!-- umeko-attachments:([A-Za-z0-9+/=]+) -->")
          .match(text);
  if (!match.hasMatch()) {
    const QString header =
        "\n\n附件（用户提供的路径；按需使用原生工具读取）：\n";
    auto index = text.lastIndexOf(header);
    if (index < 0)
      return {text, {}};
    QVariantList attachments;
    auto tail = text.mid(index + header.size());
    for (auto line : tail.split('\n')) {
      auto part =
          QRegularExpression("^- (file|folder|image): (.+)$").match(line);
      if (!part.hasMatch())
        return {text, {}};
      auto path = part.captured(2);
      auto name = QString(path).replace('\\', '/').section('/', -1);
      attachments << QVariantMap{{"kind", part.captured(1)},
                                 {"path", path},
                                 {"name", name.isEmpty() ? path : name}};
    }
    if (attachments.isEmpty())
      return {text, {}};
    auto body = text.left(index);
    return {body == "请查看这些附件。" ? QString() : body, attachments};
  }
  auto items = QJsonDocument::fromJson(
                   QByteArray::fromBase64(match.captured(1).toLatin1()))
                   .array()
                   .toVariantList();
  auto note = text.lastIndexOf("\n\n附件（用户提供的路径");
  text =
      note >= 0 ? text.left(note) : text.left(match.capturedStart()).trimmed();
  return {text, items};
}
