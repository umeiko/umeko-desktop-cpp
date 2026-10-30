#pragma once
#include <QFont>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QVariantMap>
class TerminalView : public QQuickPaintedItem {
  Q_OBJECT
  Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY
                 styleChanged)
  Q_PROPERTY(qreal fontSize READ fontSize WRITE setFontSize NOTIFY styleChanged)
  Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE
                 setBackgroundColor NOTIFY styleChanged)
  Q_PROPERTY(QColor foregroundColor READ foregroundColor WRITE
                 setForegroundColor NOTIFY styleChanged)
  Q_PROPERTY(QColor cursorColor READ cursorColor WRITE setCursorColor NOTIFY
                 styleChanged)
  Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
public:
  explicit TerminalView(QQuickItem *parent = nullptr);
  QString fontFamily() const { return family; }
  qreal fontSize() const { return size; }
  QColor backgroundColor() const { return background; }
  QColor foregroundColor() const { return foreground; }
  QColor cursorColor() const { return cursorInk; }
  void setFontFamily(QString value);
  void setFontSize(qreal value);
  void setBackgroundColor(QColor value);
  void setForegroundColor(QColor value);
  void setCursorColor(QColor value);
  bool hasSelection() const { return a.x() >= 0 && z.x() >= 0 && a != z; }
  void paint(QPainter *painter) override;
  Q_INVOKABLE void applyFrame(QVariantMap frame);
  Q_INVOKABLE void reset();
  Q_INVOKABLE QString selectedText() const;
  Q_INVOKABLE void copy();
  Q_INVOKABLE void paste();
  Q_INVOKABLE void selectAll();
signals:
  void input(QString text);
  void inputRejected(QString text);
  void resized(int columns, int rows);
  void contextMenuRequested(qreal x, qreal y);
  void styleChanged();
  void selectionChanged();

protected:
  void geometryChange(const QRectF &now, const QRectF &old) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void inputMethodEvent(QInputMethodEvent *event) override;
  QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;

private:
  QString family, preedit;
  qreal size = 12, cw = 8, ch = 16;
  QFont font;
  QColor background = "#ffffff", foreground = "#202020", cursorInk = "#2684ff";
  QVariantList history, screen;
  QVariantMap cursor;
  qint64 revision = -1;
  int scroll = 0, limit = 1000, columns = 0, rows = 0;
  bool bracketed = false, dragging = false, blink = true;
  QPoint a{-1, -1}, z{-1, -1};
  QTimer timer;
  void metrics();
  void dimensions();
  void clearSelection();
  void send(QString text);
  int top() const { return qMax(0, int(history.size()) - scroll); }
  QPoint point(QPointF position) const;
  QVariantMap line(int index) const;
  QColor color(QString value, QColor fallback) const;
};
