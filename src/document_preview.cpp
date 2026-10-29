#include "document_preview.hpp"
#include "common.hpp"
#include "store.hpp"
#include "webview_support.hpp"
#include <QStringDecoder>
#include <QTextDocument>
#include <QtConcurrent>

namespace {
constexpr qsizetype ReadLimit = 256 * 1024, TextLimit = 48000, LineLimit = 1000,
                    LineLength = 2000;
struct LocalDocument : QTextDocument {
  QVariant loadResource(int, const QUrl &) override { return {}; }
};
} // namespace
DocumentPreview::DocumentPreview(QObject *parent, Store *settings)
    : QObject(parent), store(settings) {
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
  ++generation;
  pending.clear();
  current.clear();
  staticToken.clear();
  staticRoot.clear();
  emit changed();
}
void DocumentPreview::load(QString path, QString kind) {
  const auto token = ++generation;
  pending = {
      {"token", QVariant::fromValue(token)}, {"path", path}, {"kind", kind}};
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
              current = result;
              presentation();
              emit changed();
            }
            start();
          });
  watcher->setFuture(QtConcurrent::run(
      [job] { return read(job["path"].toString(), job["kind"].toString()); }));
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
  auto bytes = file.read(ReadLimit);
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
