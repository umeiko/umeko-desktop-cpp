#include "document_preview.hpp"
#include "common.hpp"
#include <QtConcurrent>
#include <QTextDocument>
#include <QStringDecoder>

namespace {
constexpr qsizetype ReadLimit = 256 * 1024, TextLimit = 48000, LineLimit = 1000, LineLength = 2000;
struct LocalDocument : QTextDocument {
  QVariant loadResource(int, const QUrl &) override { return {}; }
};
}
void DocumentPreview::cancel() {
  ++generation; pending.clear(); current.clear(); emit changed();
}
void DocumentPreview::load(QString path, QString kind) {
  const auto token = ++generation;
  pending = {{"token", QVariant::fromValue(token)}, {"path", path}, {"kind", kind}};
  current = {{"path", path}, {"busy", true}};
  emit changed(); start();
}
void DocumentPreview::start() {
  if (reading || pending.isEmpty()) return;
  reading = true;
  const auto job = pending; pending.clear();
  auto watcher = new QFutureWatcher<QVariantMap>(this);
  connect(watcher, &QFutureWatcher<QVariantMap>::finished, this, [this, watcher, job] {
    auto result = watcher->result(); watcher->deleteLater(); reading = false;
    if (job["token"].toULongLong() == generation) { current = result; emit changed(); }
    start();
  });
  watcher->setFuture(QtConcurrent::run([job] { return read(job["path"].toString(), job["kind"].toString()); }));
}
QVariantMap DocumentPreview::read(QString path, QString kind) {
  QVariantMap out{{"path", path}, {"busy", false}};
  auto fail = [&](QString error) { out["error"] = error; return out; };
  QFileInfo info(path);
  kind = U::documentKind(path, kind);
  if (!info.isAbsolute() || kind.isEmpty() || !info.isFile()) return fail("文件不存在或不支持文本预览");
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return fail("文件暂时无法读取");
  const auto size = file.size();
  auto bytes = file.read(ReadLimit);
  if (file.error() != QFileDevice::NoError) return fail("文件读取失败");
  bool truncated = size > bytes.size();
  QString text;
  if (bytes.startsWith("\xff\xfe") || bytes.startsWith("\xfe\xff")) {
    QStringDecoder decode(bytes.startsWith("\xff\xfe") ? QStringDecoder::Utf16LE : QStringDecoder::Utf16BE);
    text = decode(bytes.mid(2));
  } else {
    if (bytes.contains('\0')) return fail("这是二进制文件，无法作为文本预览");
    QStringDecoder decode(QStringDecoder::Utf8);
    text = decode(bytes);
  }
  text.replace("\r\n", "\n"); text.replace('\r', '\n');
  // Limit both the document layout and each individual block before it reaches QML.
  if (text.size() > TextLimit) { text.truncate(TextLimit); truncated = true; }
  QStringList lines;
  for (const auto &line : text.split('\n')) {
    if (lines.size() >= LineLimit) { truncated = true; break; }
    if (line.size() > LineLength) { lines << line.left(LineLength) + " …"; truncated = true; }
    else lines << line;
  }
  text = lines.join('\n');
  QString html;
  bool simple = false;
  if (kind == "markdown") {
    simple = text.size() > 24000 || lines.size() > 400 || text.count('|') > 256 || text.count('[') > 256;
    for (const auto &line : lines) if (line.size() > 1000) simple = true;
    if (!simple) {
      LocalDocument document;
      document.setMarkdown(text);
      html = document.toHtml();
      // Do not load local/remote images, stylesheets or other embedded resources.
      html.replace(QRegularExpression("<img\\b[^>]*>", QRegularExpression::CaseInsensitiveOption), "[图片]");
      html.replace(QRegularExpression("<body[^>]*>"), "<body>");
      html.replace(QRegularExpression("\\b(?:src|background)\\s*=\\s*(?:\"[^\"]*\"|'[^']*'|[^\\s>]+)", QRegularExpression::CaseInsensitiveOption), "");
      html.replace(QRegularExpression("url\\([^)]*\\)", QRegularExpression::CaseInsensitiveOption), "");
      if (html.size() > 256 * 1024) { html.clear(); simple = true; }
    }
  }
  out.insert("kind", kind); out.insert("text", text); out.insert("html", html);
  bool highlight = kind == "code" && text.size() <= 24000 && lines.size() <= 500;
  for (const auto &line : lines) if (line.size() > 1000) highlight = false;
  out.insert("highlight", highlight);
  out.insert("language", QFileInfo(path).suffix().toLower()); out.insert("bytes", size);
  out.insert("truncated", truncated); out.insert("simple", simple);
  return out;
}
