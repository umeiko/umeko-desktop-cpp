#pragma once
#include <QtCore>
class Backend;
class SlashCommands : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList entries READ entries NOTIFY changed)
public:
  explicit SlashCommands(Backend *backend);
  QVariantList entries() const;
  Q_INVOKABLE void refresh();
  Q_INVOKABLE void invalidate();
  Q_INVOKABLE bool choose(QString id);
  Q_INVOKABLE bool execute(QString text);
signals:
  void changed();
private:
  Backend *b;
  QVariantList skills;
  QByteArray loadedContext;
  bool pending=false;
  QElapsedTimer loaded;
  QByteArray contextKey() const;
};
