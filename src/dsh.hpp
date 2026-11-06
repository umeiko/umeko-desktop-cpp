#pragma once
#include "common.hpp"
class Backend;
class DshRuntime : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString path READ path NOTIFY changed)
  Q_PROPERTY(bool useOwnConfig READ useOwnConfig NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(bool ready READ ready NOTIFY changed)
  Q_PROPERTY(bool failed READ failed NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  Q_PROPERTY(QString hint READ hint NOTIFY changed)
  Q_PROPERTY(QString version READ version NOTIFY changed)
  Q_PROPERTY(QString resolvedPath READ resolvedPath NOTIFY changed)
  Q_PROPERTY(double progress READ progress NOTIFY changed)
public:
  explicit DshRuntime(Backend *backend);
  QString path() const;
  bool useOwnConfig() const;
  bool busy() const {return process;}
  bool ready() const {return m_ready;}
  bool failed() const {return m_failed;}
  QString status() const {return m_status;}
  QString hint() const {return m_hint;}
  QString version() const {return m_version;}
  QString resolvedPath() const {return m_path;}
  double progress() const {return m_progress;}
  Q_INVOKABLE void setPath(QString value);
  Q_INVOKABLE void setUseOwnConfig(bool value);
  Q_INVOKABLE void check();
  Q_INVOKABLE void download();
  Q_INVOKABLE void cancel();
  void close();
signals:
  void changed();
private:
  Backend *b;
  QPointer<QProcess> process;
  bool m_ready=false,m_failed=false,cancelled=false;
  QString m_status="尚未检查 dsh",m_hint="选择安装路径，或下载官方版本。",m_version,m_path;
  double m_progress=0;
  void start(bool download);
};
