#pragma once
#include <QtCore>
#include <functional>
class Backend;

class Skills : public QObject {
  Q_OBJECT
public:
  explicit Skills(Backend *backend);
  ~Skills() override;
  bool enabled() const;
  QVariantMap context() const;
  void installBundled();
  void close();
  QVariantMap start(QString operation, QVariantMap args);
  QVariantMap job(QString id, int offset, int limit) const;
private:
  Backend *b;
  QPointer<QProcess> worker;
  QByteArray buffer;
  QString token, workspace;
  QVariantMap jobs;
  QStringList order;
  std::shared_ptr<QLockFile> installLock;
  void receive();
};
