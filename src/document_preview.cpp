#include "document_preview.hpp"
#include "common.hpp"
#include "store.hpp"
#include "webview_support.hpp"
#include <QStringDecoder>
#include <QTextDocument>
#include <QtConcurrent>
#include <QClipboard>
#include <QGuiApplication>

namespace {
constexpr qsizetype ReadLimit = 256 * 1024, TextLimit = 48000, LineLimit = 1000,
                    LineLength = 2000;
struct LocalDocument : QTextDocument {
  QVariant loadResource(int, const QUrl &) override { return {}; }
};
} // namespace
DocumentPreview::DocumentPreview(QObject *parent, Store *settings, QString resourceRoot)
    : QObject(parent), model(this), root(std::move(resourceRoot)), store(settings) {
  connect(&server, &QTcpServer::newConnection, this, [this] {
    while (server.hasPendingConnections()) {
      auto socket = server.nextPendingConnection();
      connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      connect(socket, &QTcpSocket::readyRead, this,
              [this, socket] { serve(socket); });
      QTimer::singleShot(10000, socket,
                         [socket] { socket->disconnectFromHost(); });
    }
  });
}
void DocumentPreview::setMode(QString type, QString mode) {
  if (!QStringList{"html", "markdown"}.contains(type) ||
      !QStringList{"source", "rendered"}.contains(mode))
    return;
  if (store) {
    auto ui = store->data["ui"].toMap();
    auto modes = ui["documentPreviewModes"].toMap();
    modes[type] = mode;
    ui["documentPreviewModes"] = modes;
    store->data["ui"] = ui;
    store->save();
  }
  if (current["fileType"] == type) {
    current["mode"] = mode;
    presentation();
    model.setMode(mode);
    emit changed();
  }
}
void DocumentPreview::presentation() {
  const auto type = current["fileType"].toString();
  auto modes = store ? store->data["ui"].toMap()["documentPreviewModes"].toMap()
                     : QVariantMap{};
  current["mode"] = modes.value(
      type, current.value("mode", QStringList{"markdown", "html"}.contains(type)
                                      ? "rendered"
                                      : "source"));
  current["previewUrl"] = "";
  if (type != "html" || current["busy"].toBool() || current.contains("error"))
    return;
  const bool available = webviewConfiguration()["available"].toBool();
  bool renderable =
      available && current["bytes"].toLongLong() <= 4 * 1024 * 1024;
  current["renderable"] = renderable;
  if (current["mode"] == "rendered" && renderable) {
    if (!server.isListening() && !server.listen(QHostAddress::LocalHost, 0)) {
      current["renderable"] = false;
      current["renderNote"] = "网页预览组件尚未安装或无法加载";
      current["mode"] = "source";
      return;
    }
    staticRoot = QFileInfo(current["path"].toString()).canonicalPath();
    staticToken = QString::fromLatin1(U::random(32).toHex());
    current["previewUrl"] =
        QString("http://127.0.0.1:%1/%2/%3")
            .arg(server.serverPort())
            .arg(staticToken,
                 QString::fromLatin1(QUrl::toPercentEncoding(
                     QFileInfo(current["path"].toString()).fileName())));
  } else if (!renderable) {
    current["mode"] = "source";
    current["renderNote"] = available ? "HTML 较大，已使用源码预览"
                                      : "网页预览组件尚未安装或无法加载";
  }
}
void DocumentPreview::serve(QTcpSocket *socket) {
  if (socket->property("reading").toBool())
    return;
  auto buffer = socket->property("request").toByteArray() + socket->readAll();
  socket->setProperty("request", buffer);
  if (buffer.size() > 16384) {
    socket->disconnectFromHost();
    return;
  }
  if (!buffer.contains("\r\n\r\n"))
    return;
  socket->setProperty("reading", true);
  auto request = buffer.split('\n').first().trimmed().split(' ');
  auto path = request.size() == 3
                  ? QUrl::fromEncoded(request[1]).path(QUrl::FullyDecoded)
                  : QString();
  auto parts = path.split('/');
  const bool head = request.value(0) == "HEAD";
  QString target;
  const auto token = staticToken;
  if ((request.value(0) == "GET" || head) && !token.isEmpty() &&
      parts.size() >= 3 && parts[1] == token) {
    bool valid = true;
    for (auto part : parts.mid(2))
      if (part.isEmpty() || part.startsWith('.') || part.contains('\\') ||
          part.contains(QChar(0)))
        valid = false;
    if (valid)
      target = QDir(staticRoot).filePath(parts.mid(2).join('/'));
  }
  auto watcher = new QFutureWatcher<QVariantMap>(this);
  QPointer<QTcpSocket> weak = socket;
  const auto root = staticRoot;
  connect(watcher, &QFutureWatcher<QVariantMap>::finished, this,
          [this, watcher, weak, token, head] {
            auto result = watcher->result();
            watcher->deleteLater();
            if (!weak)
              return;
            if (token != staticToken) {
              weak->disconnectFromHost();
              return;
            }
            const auto status = result.value("status", 404).toInt();
            const auto body = result["body"].toByteArray();
            QByteArray response =
                "HTTP/1.1 " + QByteArray::number(status) +
                (status == 200 ? " OK\r\n" : " Not Found\r\n");
            response +=
                "Content-Type: " +
                result.value("mime", "text/plain").toByteArray() +
                "\r\nContent-Length: " + QByteArray::number(body.size()) +
                "\r\nCache-Control: no-store\r\nReferrer-Policy: "
                "no-referrer\r\nX-Content-Type-Options: nosniff\r\nConnection: "
                "close\r\n\r\n";
            weak->write(response);
            if (!head)
              weak->write(body);
            weak->disconnectFromHost();
          });
  watcher->setFuture(QtConcurrent::run([target, root] {
    const auto canonical = QFileInfo(target).canonicalFilePath();
    const auto relative = QDir(root).relativeFilePath(canonical);
    const auto suffix = QFileInfo(canonical).suffix().toLower();
    const auto allowed =
        QString("html htm css js mjs json map svg png jpg jpeg gif webp avif "
                "ico bmp woff woff2 ttf otf eot wasm csv tsv txt mp4 webm mp3 "
                "ogg wav pdf")
            .split(' ');
    if (target.isEmpty() || canonical.isEmpty() || relative == ".." ||
        relative.startsWith("../") || QFileInfo(relative).isAbsolute() ||
        !allowed.contains(suffix) || !QFileInfo(canonical).isFile())
      return QVariantMap{{"status", 404}};
    QFile file(canonical);
    const qint64 limit = QStringList{"html", "htm"}.contains(suffix)
                             ? 4 * 1024 * 1024
                             : 32 * 1024 * 1024;
    if (QFileInfo(canonical).size() > limit)
      return QVariantMap{{"status", 413}};
    if (!file.open(QIODevice::ReadOnly))
      return QVariantMap{{"status", 404}};
    auto body = file.read(limit + 1);
    if (body.size() > limit)
      return QVariantMap{{"status", 413}};
    return QVariantMap{
        {"status", 200},
        {"body", body},
        {"mime", QMimeDatabase().mimeTypeForFile(canonical).name().toUtf8()}};
  }));
}
void DocumentPreview::cancel() {
  epoch->store(++generation);
  model.load({}, {}, "source");
  pending.clear();
  current.clear();
  staticToken.clear();
  staticRoot.clear();
  emit changed();
}
void DocumentPreview::load(QString path, QString kind) {
  const auto token = ++generation;
  epoch->store(token);
  QFileInfo info(path);const auto key=info.canonicalFilePath()+"|"+QString::number(info.size())+"|"+QString::number(info.lastModified().toMSecsSinceEpoch())+"|"+kind;
  if(auto saved=cache.object(key)) {
    pending.clear();auto result=*saved;auto rich=result.take("_rendered").toList(),plain=result.take("_source").toList();
    current=result;presentation();model.load(rich,plain,current["mode"].toString());emit changed();return;
  }
  pending = {
      {"token", QVariant::fromValue(token)}, {"path", path}, {"kind", kind}, {"cacheKey",key}};
  current = {{"path", path}, {"busy", true}};
  emit changed();
  start();
}
void DocumentPreview::start() {
  if (reading || pending.isEmpty())
    return;
  reading = true;
  const auto job = pending;
  pending.clear();
  auto watcher = new QFutureWatcher<QVariantMap>(this);
  connect(watcher, &QFutureWatcher<QVariantMap>::finished, this,
          [this, watcher, job] {
            auto result = watcher->result();
            watcher->deleteLater();
            reading = false;
            if (job["token"].toULongLong() == generation) {
              if(!result.contains("error"))cache.insert(job["cacheKey"].toString(),new QVariantMap(result),qMax(1,int(result["bytes"].toLongLong()/256)));
              auto rich=result.take("_rendered").toList(), plain=result.take("_source").toList();
              current = result;
              presentation();
              model.load(rich, plain, current["mode"].toString());
              emit changed();
            }
            start();
          });
  const auto script=root+"/runtime/markdown-preview.mjs", node=U::node();
  const auto cancel=epoch;
  watcher->setFuture(QtConcurrent::run([job,script,node,cancel] {
    auto result=read(job["path"].toString(),job["kind"].toString());
    if(!result["chunked"].toBool()) return result;
    const auto text=result["text"].toString();
    QVariantList source;
    for(qsizetype start=0;start<text.size();) {
      auto end=qMin(start+8192,text.size());
      auto boundary=text.lastIndexOf('\n',end-1);
      if(boundary>start+4096)end=boundary+1;
      if(end<text.size() && text[end-1].isHighSurrogate())--end;
      source<<QVariantMap{{"kind",result["kind"]=="code"?"code":"source"},{"language",result["language"]},{"text",text.mid(start,end-start)}};start=end;
    }
    result["_source"]=source;result["_rendered"]=source;
    if(result["kind"]=="markdown") {
      QProcess parser;U::hideConsole(&parser);parser.setProcessEnvironment(U::environment());
      parser.start(node,{script});
      if(parser.waitForStarted(5000)) {
        parser.write(text.toUtf8());parser.closeWriteChannel();QElapsedTimer timer;timer.start();
        QByteArray output,error;
        while(parser.state()!=QProcess::NotRunning && timer.elapsed()<30000 && cancel->load()==job["token"].toULongLong()) {
          parser.waitForReadyRead(30);output+=parser.readAllStandardOutput();error+=parser.readAllStandardError();
          if(output.size()>128*1024*1024)break;
        }
        if(parser.state()!=QProcess::NotRunning){parser.kill();parser.waitForFinished(1000);}
        output+=parser.readAllStandardOutput();
        if(parser.exitStatus()==QProcess::NormalExit && parser.exitCode()==0) {
          auto rows=QJsonDocument::fromJson(output).object()["blocks"].toArray().toVariantList();
          if(!rows.isEmpty() || text.isEmpty())result["_rendered"]=rows;
        } else result["renderNote"]="文档渲染暂时不可用，已保留完整源码预览";
      } else result["renderNote"]="文档渲染暂时不可用，已保留完整源码预览";
    }
    result["text"]="";result["html"]="";result["simple"]=false;
    return result;
  }));
}
QVariantMap DocumentPreview::read(QString path, QString kind) {
  QVariantMap out{{"path", path}, {"busy", false}};
  auto fail = [&](QString error) {
    out["error"] = error;
    return out;
  };
  QFileInfo info(path);
  kind = U::documentKind(path, kind);
  if (!info.isAbsolute() || kind.isEmpty() || !info.isFile())
    return fail("文件不存在或不支持文本预览");
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return fail("文件暂时无法读取");
  const auto size = file.size();
  const bool full = size <= 16 * 1024 * 1024;
  auto bytes = file.read(full ? size : ReadLimit);
  if (file.error() != QFileDevice::NoError)
    return fail("文件读取失败");
  bool truncated = size > bytes.size();
  QString text;
  if (bytes.startsWith("\xff\xfe") || bytes.startsWith("\xfe\xff")) {
    QStringDecoder decode(bytes.startsWith("\xff\xfe")
                              ? QStringDecoder::Utf16LE
                              : QStringDecoder::Utf16BE);
    text = decode(bytes.mid(2));
  } else {
    if (bytes.contains('\0'))
      return fail("这是二进制文件，无法作为文本预览");
    QStringDecoder decode(QStringDecoder::Utf8);
    text = decode(bytes);
  }
  text.replace("\r\n", "\n");
  text.replace('\r', '\n');
  if(full && (text.size()>24000 || text.count('\n')>400 || (kind=="markdown" && (text.count('|')>256 || text.count('[')>256)))) {
    out.insert("kind",kind);out.insert("fileType",QStringList{"html","htm"}.contains(info.suffix().toLower()) ? "html" : kind);
    out.insert("text",text);out.insert("chunked",true);out.insert("bytes",size);out.insert("truncated",false);
    out.insert("language",info.suffix().toLower());out.insert("highlight",kind=="code");return out;
  }
  // Limit both the document layout and each individual block before it reaches
  // QML.
  if (text.size() > TextLimit) {
    text.truncate(TextLimit);
    truncated = true;
  }
  QStringList lines;
  for (const auto &line : text.split('\n')) {
    if (lines.size() >= LineLimit) {
      truncated = true;
      break;
    }
    if (line.size() > LineLength) {
      lines << line.left(LineLength) + " …";
      truncated = true;
    } else
      lines << line;
  }
  text = lines.join('\n');
  QString html;
  bool simple = false;
  if (kind == "markdown") {
    simple = text.size() > 24000 || lines.size() > 400 ||
             text.count('|') > 256 || text.count('[') > 256;
    for (const auto &line : lines)
      if (line.size() > 1000)
        simple = true;
    if (!simple) {
      LocalDocument document;
      document.setMarkdown(text);
      html = document.toHtml();
      // Do not load local/remote images, stylesheets or other embedded
      // resources.
      html.replace(
          QRegularExpression("<img\\b[^>]*>",
                             QRegularExpression::CaseInsensitiveOption),
          "[图片]");
      html.replace(QRegularExpression("<body[^>]*>"), "<body>");
      html.replace(
          QRegularExpression(
              "\\b(?:src|background)\\s*=\\s*(?:\"[^\"]*\"|'[^']*'|[^\\s>]+)",
              QRegularExpression::CaseInsensitiveOption),
          "");
      html.replace(
          QRegularExpression("url\\([^)]*\\)",
                             QRegularExpression::CaseInsensitiveOption),
          "");
      if (html.size() > 256 * 1024) {
        html.clear();
        simple = true;
      }
    }
  }
  out.insert("kind", kind);
  out.insert("text", text);
  out.insert("html", html);
  out.insert("fileType",
             QStringList{"html", "htm"}.contains(info.suffix().toLower())
                 ? "html"
                 : kind);
  bool highlight =
      kind == "code" && text.size() <= 24000 && lines.size() <= 500;
  for (const auto &line : lines)
    if (line.size() > 1000)
      highlight = false;
  out.insert("highlight", highlight);
  out.insert("language", QFileInfo(path).suffix().toLower());
  out.insert("bytes", size);
  out.insert("truncated", truncated);
  out.insert("simple", simple);
  return out;
}

int DocumentPreview::anchorIndex(QString anchor) const {
  if(anchor.startsWith('#'))anchor.remove(0,1);
  anchor=QUrl::fromPercentEncoding(anchor.toUtf8());
  for(int i=0;i<model.rows.size();++i)if(model.rows[i].toMap()["anchor"]==anchor)return i;
  return -1;
}
void DocumentPreview::copySelection(QVariantMap range) {
  auto a=range["anchor"].toMap(),z=range["cursor"].toMap();
  if(a["row"].toInt()>z["row"].toInt() || (a["row"]==z["row"] && a["offset"].toInt()>z["offset"].toInt()))qSwap(a,z);
  QStringList parts;
  for(int i=qMax(0,a["row"].toInt());i<model.rows.size() && i<=z["row"].toInt();++i) {
    const auto row=model.rows[i].toMap();auto text=row["text"].toString();
    if(!row["html"].toString().isEmpty()){LocalDocument doc;doc.setHtml(row["html"].toString());text=doc.toPlainText();}
    int start=i==a["row"].toInt() ? a["offset"].toInt() : 0;
    int end=i==z["row"].toInt() && z["block"].toInt()!=2147483647 ? z["offset"].toInt() : text.size();
    parts<<text.mid(qMax(0,start),qMax(0,end-start));
  }
  QGuiApplication::clipboard()->setText(parts.join(current["mode"]=="source" ? QString() : QString("\n")));
}

void DocumentPreview::copyAll() {
  if(!current["chunked"].toBool()){QGuiApplication::clipboard()->setText(current["text"].toString());return;}
  QString text;for(const auto &row:model.source)text+=row.toMap()["text"].toString();QGuiApplication::clipboard()->setText(text);
}
