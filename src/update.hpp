#pragma once
#include "common.hpp"

// The detached installer starts without constructing Backend or any agents.
class UpdateInstaller : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap status READ status NOTIFY changed)
  Q_PROPERTY(QString language READ language CONSTANT)
public:
  explicit UpdateInstaller(QString jobFile, QString resourceRoot, QObject *parent = nullptr);
  QVariantMap status() const { return m_status; }
  QString language() const { return job.value("language", "zh").toString(); }
  void begin();
  Q_INVOKABLE void finish();
signals:
  void changed();
private:
  QString jobFile, resourceRoot;
  QVariantMap job, terminalStatus, m_status{{"state", "waiting"}, {"message", "正在准备安装…"}, {"progress", -1}};
  QProcess process;
  QByteArray buffer;
};
