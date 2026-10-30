#include "terminal_view.hpp"
#include <QClipboard>
#include <QCursor>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>
namespace {
const QStringList names{
    "black",       "red",           "green",       "brown",
    "blue",        "magenta",       "cyan",        "white",
    "brightblack", "brightred",     "brightgreen", "brightbrown",
    "brightblue",  "brightmagenta", "brightcyan",  "brightwhite"};
const QStringList dark{"#000000", "#cd3131", "#0dbc79", "#e5e510",
                       "#2472c8", "#bc3fbc", "#11a8cd", "#e5e5e5",
                       "#666666", "#f14c4c", "#23d18b", "#f5f543",
                       "#3b8eea", "#d670d6", "#29b8db", "#ffffff"};
const QStringList light{"#202020", "#aa3731", "#287b2f", "#9b6a00",
                        "#1b64c8", "#a626a4", "#087b8f", "#6b6b6b",
                        "#666666", "#c73737", "#2d8a34", "#aa7500",
                        "#2675df", "#b62dac", "#00859b", "#828282"};
bool less(QPoint a, QPoint b) {
  return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
}
int cellWidth(char32_t c) {
  auto cat = QChar::category(c);
  if (cat == QChar::Mark_NonSpacing || cat == QChar::Mark_Enclosing ||
      cat == QChar::Mark_SpacingCombining)
    return 0;
  return c >= 0x1100 && (c <= 0x115f || c == 0x2329 || c == 0x232a ||
                         (c >= 0x2e80 && c <= 0xa4cf && c != 0x303f) ||
                         (c >= 0xac00 && c <= 0xd7a3) ||
                         (c >= 0xf900 && c <= 0xfaff) ||
                         (c >= 0xfe10 && c <= 0xfe19) ||
                         (c >= 0xfe30 && c <= 0xfe6f) ||
                         (c >= 0xff00 && c <= 0xff60) ||
                         (c >= 0xffe0 && c <= 0xffe6) ||
                         (c >= 0x1f300 && c <= 0x1faff) ||
                         (c >= 0x20000 && c <= 0x3fffd))
             ? 2
             : 1;
}
} // namespace
TerminalView::TerminalView(QQuickItem *parent) : QQuickPaintedItem(parent) {
  setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
  setAcceptHoverEvents(true);
  setCursor(QCursor(Qt::IBeamCursor));
  setFlag(ItemAcceptsInputMethod, true);
  auto available = QFontDatabase::families();
  for (auto name : {"Cascadia Mono", "Consolas", "Menlo", "DejaVu Sans Mono"})
    if (available.contains(name)) {
      family = name;
      break;
    }
  if (family.isEmpty())
    family = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
  timer.setInterval(500);
  connect(&timer, &QTimer::timeout, this, [this] {
    if (hasActiveFocus()) {
      blink = !blink;
      update();
    }
  });
  timer.start();
  metrics();
}
void TerminalView::metrics() {
  font = QFont(family);
  font.setPixelSize(qMax(8, qRound(size)));
  font.setStyleHint(QFont::Monospace);
  QFontMetricsF fm(font);
  cw = qMax(1., fm.horizontalAdvance('M'));
  ch = std::ceil(fm.height() + 2);
  dimensions();
  update();
}
void TerminalView::dimensions() {
  int c = qBound(20, int((width() - 8) / cw), 240),
      r = qBound(4, int((height() - 8) / ch), 100);
  if (columns != c || rows != r) {
    columns = c;
    rows = r;
    emit resized(c, r);
  }
}
void TerminalView::geometryChange(const QRectF &now, const QRectF &old) {
  QQuickPaintedItem::geometryChange(now, old);
  dimensions();
}
void TerminalView::setFontFamily(QString v) {
  if (v.isEmpty() || v == family)
    return;
  family = v;
  metrics();
  emit styleChanged();
}
void TerminalView::setFontSize(qreal v) {
  if (!std::isfinite(v) || v == size)
    return;
  size = qBound(8., v, 96.);
  metrics();
  emit styleChanged();
}
void TerminalView::setBackgroundColor(QColor v) {
  if (background == v)
    return;
  background = v;
  update();
  emit styleChanged();
}
void TerminalView::setForegroundColor(QColor v) {
  if (foreground == v)
    return;
  foreground = v;
  update();
  emit styleChanged();
}
void TerminalView::setCursorColor(QColor v) {
  if (cursorInk == v)
    return;
  cursorInk = v;
  update();
  emit styleChanged();
}
void TerminalView::clearSelection() {
  a = z = QPoint(-1, -1);
  emit selectionChanged();
}
void TerminalView::applyFrame(QVariantMap frame) {
  auto rev = frame.value("revision", 0).toLongLong();
  if (rev <= revision && !frame["reset"].toBool())
    return;
  revision = rev;
  limit = qBound(100, frame.value("historyLimit", limit).toInt(), 5000);
  if (frame["reset"].toBool()) {
    history = frame["history"].toList();
    scroll = 0;
    clearSelection();
  } else {
    auto append = frame["historyAppend"].toList();
    history.append(append);
    if (scroll)
      scroll += int(append.size());
  }
  if (history.size() > limit) {
    history = history.mid(history.size() - limit);
    clearSelection();
  }
  if (frame.contains("screen"))
    screen = frame["screen"].toList();
  if (frame.contains("cursor"))
    cursor = frame["cursor"].toMap();
  bracketed = frame["bracketedPaste"].toBool();
  scroll = qMin(scroll, int(history.size()));
  update();
}
void TerminalView::reset() {
  history.clear();
  screen.clear();
  revision = -1;
  scroll = 0;
  clearSelection();
  update();
}
QVariantMap TerminalView::line(int i) const {
  return i < int(history.size()) ? history[i].toMap()
                                 : screen[i - history.size()].toMap();
}
QColor TerminalView::color(QString value, QColor fallback) const {
  if (value.isEmpty() || value == "default")
    return fallback;
  int index = names.indexOf(value);
  auto v = index >= 0 ? (background.lightness() > 128 ? light : dark)[index]
           : value.size() == 6 ? "#" + value
                               : value;
  QColor result(v);
  return result.isValid() ? result : fallback;
}
void TerminalView::paint(QPainter *p) {
  p->fillRect(boundingRect(), background);
  auto begin = less(a, z) ? a : z, end = less(a, z) ? z : a;
  auto start = top();
  for (int y = 0; y <= rows; ++y) {
    int i = start + y;
    if (i >= history.size() + screen.size())
      break;
    auto value = line(i);
    qreal py = 4 + y * ch;
    for (auto run : value["runs"].toList()) {
      auto r = run.toList();
      if (r.size() != 6)
        continue;
      auto column = r[0].toInt(), flags = r[4].toInt(), cells = r[5].toInt();
      auto fg = color(r[2].toString(), foreground),
           bg = color(r[3].toString(), background);
      if (flags & 4)
        std::swap(fg, bg);
      p->fillRect(QRectF(4 + column * cw, py, cells * cw, ch), bg);
      auto f = font;
      f.setBold(flags & 1);
      f.setUnderline(flags & 2);
      f.setItalic(flags & 8);
      p->setFont(f);
      p->setPen(fg);
      p->drawText(QRectF(4 + column * cw, py, cells * cw + 2, ch),
                  Qt::AlignLeft | Qt::AlignVCenter, r[1].toString());
    }
    if (hasSelection() && i >= begin.x() && i <= end.x()) {
      int left = i == begin.x() ? begin.y() : 0,
          right = i == end.x() ? end.y() : columns;
      p->fillRect(QRectF(4 + left * cw, py, qMax(0, right - left) * cw, ch),
                  QColor(60, 130, 220, 105));
    }
  }
  int cy = history.size() + cursor["y"].toInt() - start;
  if (hasActiveFocus() && blink && !cursor["hidden"].toBool() && cy >= 0 &&
      cy < rows) {
    auto ink = cursorInk;
    ink.setAlpha(190);
    p->fillRect(QRectF(4 + cursor["x"].toInt() * cw, 4 + cy * ch, cw, ch), ink);
  }
  if (!preedit.isEmpty()) {
    p->setFont(font);
    p->setPen(foreground);
    p->drawText(
        QRectF(4 + cursor["x"].toInt() * cw, 4 + qMax(0, cy) * ch, width(), ch),
        Qt::AlignLeft | Qt::AlignVCenter, preedit);
  }
}
QPoint TerminalView::point(QPointF pos) const {
  return {qMin(qMax(0, int(history.size() + screen.size()) - 1),
               top() + qMax(0, int((pos.y() - 4) / ch))),
          qBound(0, int((pos.x() - 4) / cw), columns)};
}
void TerminalView::mousePressEvent(QMouseEvent *e) {
  forceActiveFocus();
  if (e->button() == Qt::RightButton) {
    emit contextMenuRequested(e->position().x(), e->position().y());
    e->accept();
    return;
  }
  a = z = point(e->position());
  dragging = true;
  emit selectionChanged();
  update();
  e->accept();
}
void TerminalView::mouseMoveEvent(QMouseEvent *e) {
  if (dragging) {
    z = point(e->position());
    emit selectionChanged();
    update();
    e->accept();
  }
}
void TerminalView::mouseReleaseEvent(QMouseEvent *e) {
  dragging = false;
  e->accept();
}
void TerminalView::wheelEvent(QWheelEvent *e) {
  scroll = qBound(0, scroll + (e->angleDelta().y() > 0 ? 3 : -3),
                  int(history.size()));
  update();
  e->accept();
}
QString TerminalView::selectedText() const {
  if (!hasSelection())
    return {};
  auto start = less(a, z) ? a : z, end = less(a, z) ? z : a;
  QStringList result;
  for (int i = start.x(); i <= end.x() && i < history.size() + screen.size();
       ++i) {
    int left = i == start.x() ? start.y() : 0,
        right = i == end.x() ? end.y() : columns, column = 0;
    QString out;
    for (auto c : line(i)["text"].toString().toUcs4()) {
      if (column >= left && column < right)
        out += QString::fromUcs4(&c, 1);
      column += cellWidth(c);
    }
    while (!out.isEmpty() && out.back().isSpace())
      out.chop(1);
    result << out;
  }
  return result.join('\n');
}
void TerminalView::copy() {
  if (hasSelection())
    QGuiApplication::clipboard()->setText(selectedText());
}
void TerminalView::selectAll() {
  a = {0, 0};
  z = {qMax(0, int(history.size() + screen.size()) - 1), columns};
  emit selectionChanged();
  update();
}
void TerminalView::send(QString text) {
  scroll = 0;
  clearSelection();
  blink = true;
  emit input(text);
  update();
}
void TerminalView::paste() {
  auto text = QGuiApplication::clipboard()->text().replace("\r\n", "\n");
  if (text.size() > 16000) {
    emit inputRejected("一次粘贴最多 16000 字符，请分段粘贴");
    return;
  }
  if (!text.isEmpty())
    send(bracketed ? "\x1b[200~" + text + "\x1b[201~"
                   : text.replace('\n', '\r'));
}
void TerminalView::keyPressEvent(QKeyEvent *e) {
  auto mod = e->modifiers();
  int key = e->key();
  bool ctrl = mod.testFlag(Qt::ControlModifier),
       shift = mod.testFlag(Qt::ShiftModifier),
       meta = mod.testFlag(Qt::MetaModifier),
       alt = mod.testFlag(Qt::AltModifier);
  if (((ctrl && shift) || meta) && key == Qt::Key_C) {
    copy();
    e->accept();
    return;
  }
  if (((ctrl && shift) || meta) && key == Qt::Key_V) {
    paste();
    e->accept();
    return;
  }
  if (ctrl && key == Qt::Key_C && hasSelection()) {
    copy();
    e->accept();
    return;
  }
  if (shift && (key == Qt::Key_PageUp || key == Qt::Key_PageDown)) {
    scroll = qBound(0, scroll + (key == Qt::Key_PageUp ? rows : -rows),
                    int(history.size()));
    update();
    e->accept();
    return;
  }
  static const QHash<int, QString> special{
      {Qt::Key_Return, "\r"},      {Qt::Key_Enter, "\r"},
      {Qt::Key_Backspace, "\x7f"}, {Qt::Key_Tab, "\t"},
      {Qt::Key_Backtab, "\x1b[Z"}, {Qt::Key_Escape, "\x1b"},
      {Qt::Key_Up, "\x1b[A"},      {Qt::Key_Down, "\x1b[B"},
      {Qt::Key_Right, "\x1b[C"},   {Qt::Key_Left, "\x1b[D"},
      {Qt::Key_Home, "\x1b[H"},    {Qt::Key_End, "\x1b[F"},
      {Qt::Key_Delete, "\x1b[3~"}, {Qt::Key_Insert, "\x1b[2~"},
      {Qt::Key_PageUp, "\x1b[5~"}, {Qt::Key_PageDown, "\x1b[6~"},
      {Qt::Key_F1, "\x1bOP"},      {Qt::Key_F2, "\x1bOQ"},
      {Qt::Key_F3, "\x1bOR"},      {Qt::Key_F4, "\x1bOS"},
      {Qt::Key_F5, "\x1b[15~"},    {Qt::Key_F6, "\x1b[17~"},
      {Qt::Key_F7, "\x1b[18~"},    {Qt::Key_F8, "\x1b[19~"},
      {Qt::Key_F9, "\x1b[20~"},    {Qt::Key_F10, "\x1b[21~"},
      {Qt::Key_F11, "\x1b[23~"},   {Qt::Key_F12, "\x1b[24~"}};
  QString text = special.value(key);
  const bool arrow = QList<int>{Qt::Key_Up,    Qt::Key_Down, Qt::Key_Left,
                                Qt::Key_Right, Qt::Key_Home, Qt::Key_End}
                         .contains(key);
  if (arrow && (ctrl || shift || alt))
    text = "\x1b[1;" +
           QString::number(1 + int(shift) + 2 * int(alt) + 4 * int(ctrl)) +
           text.right(1);
  if (ctrl && key >= Qt::Key_A && key <= Qt::Key_Z && !alt)
    text = QChar(key - Qt::Key_A + 1);
  else if (text.isEmpty())
    text = e->text();
  if (!text.isEmpty()) {
    if (alt && !ctrl && !arrow)
      text = "\x1b" + text;
    send(text);
    e->accept();
  } else
    QQuickPaintedItem::keyPressEvent(e);
}
void TerminalView::inputMethodEvent(QInputMethodEvent *e) {
  if (!e->commitString().isEmpty())
    send(e->commitString());
  preedit = e->preeditString();
  update();
  e->accept();
}
QVariant TerminalView::inputMethodQuery(Qt::InputMethodQuery q) const {
  if (q == Qt::ImEnabled)
    return true;
  if (q == Qt::ImCursorRectangle)
    return QRectF(4 + cursor["x"].toInt() * cw, 4 + cursor["y"].toInt() * ch,
                  cw, ch);
  if (q == Qt::ImSurroundingText)
    return "";
  return QQuickPaintedItem::inputMethodQuery(q);
}
