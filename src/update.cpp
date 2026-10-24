#include "update.hpp"
#include <QGuiApplication>

UpdateInstaller::UpdateInstaller(QString file, QString root, QObject *parent)
    : QObject(parent), jobFile(file), resourceRoot(root), process(this) {
  job = U::readJson(jobFile, QVariantMap{}).toMap();
  connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
    buffer += process.readAllStandardOutput();
    while (buffer.contains('\n')) {
      auto at = buffer.indexOf('\n');
      auto value = QJsonDocument::fromJson(buffer.left(at)).toVariant().toMap();
      buffer.remove(0, at + 1);
      if (!value.isEmpty()) {
        m_status = value;
        if (value["state"] == "complete" || value["state"] == "error") {
          terminalStatus = value; m_status["state"] = "finishing"; m_status["message"] = "正在完成安装…";
        }
        if (value["state"] == "waiting" && !U::writeJson(jobFile + ".ready", QVariantMap{{"pid", QCoreApplication::applicationPid()}})) {
          process.kill(); m_status = {{"state", "error"}, {"message", "无法创建安装器启动信号"}};
        }
        emit changed();
      }
    }
  });
  connect(&process, &QProcess::errorOccurred, this, [this] {
    m_status = {{"state", "error"}, {"message", "安装引擎启动失败：" + process.errorString()}};
    emit changed();
  });
  connect(&process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus exit) {
    if (!terminalStatus.isEmpty()) {m_status = terminalStatus; emit changed();}
    if ((code || exit != QProcess::NormalExit || terminalStatus.isEmpty()) && m_status["state"] != "error") {
      m_status = {{"state", "error"}, {"message", "安装中断。旧版本备份和安装日志已保留。"}};
      emit changed();
    }
  });
}
void UpdateInstaller::begin() {
  if (job.isEmpty()) {
    m_status = {{"state", "error"}, {"message", "无法读取安装任务或创建启动信号"}};
    emit changed(); return;
  }
  process.setProcessEnvironment(U::environment());
  U::hideConsole(&process);
  process.start(U::node(), {resourceRoot + "/runtime/update-engine.mjs", "install", jobFile});
}
void UpdateInstaller::finish() {
  if (m_status["state"] == "error") { QCoreApplication::quit(); return; }
  if (m_status["state"] != "complete") return;
  auto executable = job["executable"].toString();
  QStringList arguments;
  for (auto v : job["restartArgs"].toList()) arguments << v.toString();
  QProcess launch;
  launch.setProgram(executable); launch.setArguments(arguments);
  launch.setWorkingDirectory(job["installDir"].toString());
  launch.setProcessEnvironment(U::environment());
  if (launch.startDetached()) {
    const auto resources = job["installDir"].toString() + "/" + job["resourceRelative"].toString();
#ifdef Q_OS_WIN
    const auto node = resources + "/runtime/node/node.exe";
#else
    const auto node = resources + "/runtime/node/bin/node";
#endif
    QProcess cleaner;
    cleaner.setProgram(node); cleaner.setArguments({resources + "/runtime/update-engine.mjs", "cleanup", jobFile});
    cleaner.setWorkingDirectory(job["installDir"].toString()); cleaner.setProcessEnvironment(U::environment());
    cleaner.startDetached();
    QCoreApplication::quit();
  }
  else { m_status["launchError"] = (language() == "en" ? "Launch failed. Retry or open: " : "启动失败，请重试或从安装目录手动启动：") + executable; emit changed(); }
}
