#pragma once
#include <QtCore>

// One bounded reader and one latest pending request; no file I/O on the GUI thread.
class DocumentPreview : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
public:
  explicit DocumentPreview(QObject *parent = nullptr) : QObject(parent) {}
  QVariantMap state() const { return current; }
  Q_INVOKABLE void load(QString path, QString kind = {});
  Q_INVOKABLE void cancel();
  static QVariantMap read(QString path, QString kind);
signals:
  void changed();
private:
  quint64 generation = 0;
  bool reading = false;
  QVariantMap current, pending;
  void start();
};
