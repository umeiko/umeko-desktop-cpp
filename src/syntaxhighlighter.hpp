#pragma once
#include <QQuickTextDocument>
#include <QtCore>
#include <QtGui>

class SyntaxHighlighter : public QObject {
  Q_OBJECT
  Q_PROPERTY(QQuickTextDocument *document READ document WRITE setDocument NOTIFY
                 changed)
  Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY changed)
  Q_PROPERTY(bool dark READ dark WRITE setDark NOTIFY changed)
  Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
  Q_PROPERTY(bool colored READ colored NOTIFY changed)
public:
  explicit SyntaxHighlighter(QObject *parent = nullptr);
  QQuickTextDocument *document() const { return m_document; }
  QString language() const { return m_language; }
  bool dark() const { return m_dark; }
  bool enabled() const { return m_enabled; }
  bool colored() const { return m_colored; }
  void setDocument(QQuickTextDocument *doc);
  void setLanguage(QString value);
  void setDark(bool value);
  void setEnabled(bool value);
signals:
  void changed();

private:
  QPointer<QQuickTextDocument> m_document;
  QString m_language;
  bool m_dark = false, m_enabled = true, m_colored = false;
  QSyntaxHighlighter *m_highlighter = nullptr;
  QTimer m_debounce;
  void apply();
};
