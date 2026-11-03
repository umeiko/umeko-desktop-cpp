#pragma once
#include <QtCore>
#include <QtNetwork>
#include "document_blocks.hpp"
#include <atomic>
class Store;

// One bounded reader and one latest pending request; no file I/O on the GUI
// thread.
class DocumentPreview : public QObject {
  Q_OBJECT
  Q_PROPERTY(QObject *blocks READ blocks CONSTANT)
  Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
public:
  explicit DocumentPreview(QObject *parent = nullptr,
                           Store *settings = nullptr, QString resourceRoot = {});
  QVariantMap state() const { return current; }
  Q_INVOKABLE void load(QString path, QString kind = {});
  Q_INVOKABLE void cancel();
  Q_INVOKABLE void setMode(QString type, QString mode);
  QObject *blocks() { return &model; }
  Q_INVOKABLE void copyAll();
  Q_INVOKABLE int anchorIndex(QString anchor) const;
  Q_INVOKABLE void copySelection(QVariantMap range);
  static QVariantMap read(QString path, QString kind);
signals:
  void changed();

private:
  DocumentBlocks model;
  QString root;
  std::shared_ptr<std::atomic<quint64>> epoch = std::make_shared<std::atomic<quint64>>(0);
  quint64 generation = 0;
  bool reading = false;
  QVariantMap current, pending;
  QCache<QString,QVariantMap> cache{32768};
  Store *store;
  QTcpServer server;
  QString staticRoot, staticToken;
  void presentation();
  void serve(QTcpSocket *socket);
  void start();
};
