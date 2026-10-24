#include "backend.hpp"

void AppAbout::cancelUpdate() {
  if (m_downloadReply && m_download["state"] == "downloading") {
    m_downloadError = "cancelled";
    m_downloadReply->abort();
  }
}
void AppAbout::installUpdate() {
  if (m_busy || m_download["state"] != "ready") return;
  const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/updates/" + U::uuid();
  QDir().mkpath(cache);
  auto directory = QCoreApplication::applicationDirPath();
#ifdef Q_OS_MACOS
  directory = U::cleanPath(directory + "/../..");
  const QString payload = "UmekoCpp/UmekoCpp.app", resources = "Contents/Resources";
#else
  const QString payload = "UmekoCpp", resources = "";
#endif
  updateJob = {{"repo", "umeko-desktop-cpp"}, {"version", m_download["version"]},
    {"archive", m_download["path"]}, {"archiveName", m_download["name"]}, {"sha256", m_download["sha256"]},
    {"cache", cache}, {"installDir", directory}, {"executable", QCoreApplication::applicationFilePath()},
    {"resourceRelative", resources}, {"payloadRelative", payload}, {"parentPid", QCoreApplication::applicationPid()},
    {"restartArgs", QStringList{"--data-dir", b->userDataDirectory()}}, {"language", b->language()},
    {"edition", m_minimal ? "minimal" : "standard"}};
  updateJobFile = cache + "/job.json";
  if (!U::writeJson(updateJobFile, updateJob)) {
    m_installation = {{"state", "error"}, {"message", "无法保存安装任务"}}; emit changed(); return;
  }
  runUpdate();
}
void AppAbout::runUpdate() {
  m_busy = true;
  m_installation = {{"state", "preparing"}, {"message", "正在准备独立安装器…"}};
  emit changed();
  auto process = new QProcess(this);
  auto buffer = std::make_shared<QByteArray>();
  connect(process, &QProcess::readyReadStandardOutput, this, [this,process,buffer] {
    *buffer += process->readAllStandardOutput();
    while (buffer->contains('\n')) {
      auto at = buffer->indexOf('\n');
      auto value = QJsonDocument::fromJson(buffer->left(at)).toVariant().toMap(); buffer->remove(0,at+1);
      if (!value.isEmpty()) {m_installation = value; emit changed();}
    }
  });
  connect(process, &QProcess::errorOccurred, this, [this,process](QProcess::ProcessError error) {
    m_busy = false; m_installation = {{"state", "error"}, {"message", "更新引擎启动失败：" + process->errorString()}}; emit changed();
    if (error == QProcess::FailedToStart) process->deleteLater();
  });
  connect(process, &QProcess::finished, this, [this,process](int code, QProcess::ExitStatus exit) {
    process->deleteLater();
    if (code || exit != QProcess::NormalExit || m_installation["state"] != "prepared") {
      if (m_installation["state"] != "error") m_installation = {{"state", "error"}, {"message", "安装准备中断，请重试。"}};
      m_busy = false; emit changed(); return;
    }
    launchInstaller(m_installation["helperExe"].toString());
  });
  process->setProcessEnvironment(U::environment()); U::hideConsole(process);
  process->start(U::node(), {b->root + "/runtime/update-engine.mjs", "prepare", updateJobFile});
}
void AppAbout::launchInstaller(QString executable) {
  QProcess helper;
  helper.setProgram(executable); helper.setArguments({"--install-update", updateJobFile});
  helper.setWorkingDirectory(QFileInfo(executable).absolutePath()); helper.setProcessEnvironment(U::environment());
  if (!helper.startDetached()) {
    m_busy = false; m_installation = {{"state", "error"}, {"message", "独立安装器启动失败，当前程序保持运行。"}}; emit changed(); return;
  }
  auto timer = new QTimer(this); auto elapsed = std::make_shared<QElapsedTimer>(); elapsed->start();
  connect(timer, &QTimer::timeout, this, [this,timer,elapsed] {
    if (QFileInfo::exists(updateJobFile + ".ready")) {
      timer->stop(); timer->deleteLater();
      if (!U::writeJson(updateJobFile + ".commit", QVariantMap{{"confirmed", true}})) {
        m_busy = false; m_installation = {{"state", "error"}, {"message", "无法确认安装交接，当前程序保持运行。"}}; emit changed(); return;
      }
      b->close(); QCoreApplication::quit();
    } else if (elapsed->elapsed() > 30000) {
      timer->stop(); timer->deleteLater(); m_busy = false;
      U::writeJson(updateJobFile + ".cancel", QVariantMap{{"cancelled", true}});
      m_installation = {{"state", "error"}, {"message", "安装器未能就绪，当前程序保持运行。"}}; emit changed();
    }
  });
  timer->start(150);
}
