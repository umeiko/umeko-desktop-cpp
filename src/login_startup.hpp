#pragma once
#include "common.hpp"
class Backend;
class LoginStartupSettings : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap state READ state NOTIFY changed)
public:
  explicit LoginStartupSettings(Backend *backend);
  QVariantMap state() const {return current;}
  Q_INVOKABLE void refresh();
  Q_INVOKABLE void setEnabled(bool umeko,bool proxy);
signals:
  void changed();
private:
  Backend *b;
  bool busy=false, available=false;
  QVariantMap current{{"available",false},{"umeko",false},{"proxy",false},{"reason",""}};
  void invoke(QString operation,bool umeko=false,bool proxy=false);
};
