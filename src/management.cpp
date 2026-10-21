#include "backend.hpp"
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#include <QDesktopServices>
#include <QtConcurrent>

void Backend::saveAgent(QString raw) {
  if (view->busy()) {
    emit toast("请先等待当前任务结束");
    return;
  }
  QJsonParseError err;
  auto values =
      QJsonDocument::fromJson(raw.toUtf8(), &err).object().toVariantMap();
  if (err.error != QJsonParseError::NoError ||
      values["maxTokens"].toInt() < 256 ||
      values["maxTokens"].toInt() > 32768 ||
      values["instructions"].toString().size() > 16000) {
    emit toast(
        "请检查运行配置：回答上限为 256–32768，自定义指令最多 16000 字符");
    return;
  }
  auto agent = store.data["agent"].toMap();
  for (auto key : {"maxTokens", "instructions", "autoCompaction", "agentDir",
                   "skillPaths", "extensionPaths"})
    if (values.contains(key))
      agent[key] = values[key];
  auto path = agent["agentDir"].toString();
  agent["agentDir"] =
      path.isEmpty() ? QDir::homePath() + "/.pi/agent" : U::cleanPath(path);
  for (auto key : {"skillPaths", "extensionPaths"}) {
    QStringList paths;
    for (auto v : agent[key].toList())
      if (!v.toString().trimmed().isEmpty())
        paths << U::cleanPath(v.toString().trimmed());
    agent[key] = paths;
  }
  store.data["agent"] = agent;
  invalidate();
  notify();
  emit toast("Pi 设置已保存，下次加载生效");
}
void Backend::openPiFolder(QString kind) {
  auto path = agentDir();
  if (kind == "skills" || kind == "extensions")
    path += "/" + kind;
  QDir().mkpath(path);
  QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}
void Backend::openPiGithub() {
  QDesktopServices::openUrl(QUrl("https://github.com/earendil-works/pi"));
}
QString Backend::mcpConfigPath(QString scope) {
  if (scope == "global")
    return agentDir() + "/mcp.json";
  if (scope == "project" && !view->workspace.isEmpty())
    return view->workspace + "/.pi/mcp.json";
  return "";
}
QString Backend::readMcp(QString scope) {
  QFile f(mcpConfigPath(scope));
  return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll())
                                     : QString("{\n  \"mcpServers\": {}\n}");
}
void Backend::saveMcp(QString scope, QString raw) {
  if (view->busy()) {
    emit toast("请等待当前任务结束");
    return;
  }
  auto path = mcpConfigPath(scope);
  QJsonParseError err;
  auto doc = QJsonDocument::fromJson(raw.toUtf8(), &err);
  if (path.isEmpty() || err.error != QJsonParseError::NoError ||
      !doc.isObject() ||
      (doc.object().contains("mcpServers") &&
       !doc.object()["mcpServers"].isObject()) ||
      raw.size() > 1000000) {
    emit toast("请提供包含 mcpServers 对象的 JSON，并先打开项目目录");
    return;
  }
  auto s = view;
  s->operation = true;
  emit chatChanged();
  QVariantMap input{{"global", scope == "global" ? raw : readMcp("global")},
                    {"project", scope == "project" ? raw : readMcp("project")}};
  U::run(
      this, U::node(), {release + "/bridge.mjs", "--check-mcp"},
      QJsonDocument::fromVariant(input).toJson(QJsonDocument::Compact),
      [this, s, path, doc](bool ok, QByteArray raw, QString error) {
        s->operation = false;
        auto result = QJsonDocument::fromJson(raw).object();
        if (!ok || !result["ok"].toBool()) {
          emit toast(result.value("error").toString(error));
          emit chatChanged();
          return;
        }
        if (!U::writeJson(path, doc.toVariant()))
          emit toast("MCP 配置保存失败");
        else {
          invalidate();
          emit toast("MCP 配置已保存，下次加载生效");
        }
        emit chatChanged();
      },
      30000);
}
void Backend::pollCapabilities() {
  if (view->process && !view->busy())
    writePi(view, {{"type", "resources"}});
}
void Backend::loadCapabilities() {
  if (!view->busy()) {
    if (view->process)
      pollCapabilities();
    else if (!view->model.isEmpty())
      command(view, "inspect");
  }
}
void Backend::refreshAgent() {
  if (!gateway)
    gatewayStart();
  if (!view->busy()) {
    retire(view);
    if (!view->model.isEmpty())
      command(view, "inspect");
    else
      management->scan();
  }
}
void Backend::reconnectMcp(QString name) {
  if (QRegularExpression("^[A-Za-z0-9_-]+$").match(name).hasMatch() && !busy())
    command(view, "mcp-command", {{"text", "/mcp reconnect " + name}});
}
static QString npmCli() {
  auto directory = QFileInfo(U::node()).absolutePath();
  for (auto candidate : {directory + "/node_modules/npm/bin/npm-cli.js",
                         directory + "/../lib/node_modules/npm/bin/npm-cli.js"})
    if (QFileInfo(candidate).isFile())
      return candidate;
  auto path = QStandardPaths::findExecutable("npm");
  if (path.isEmpty())
    path = QStandardPaths::findExecutable("npm.cmd");
  if (path.endsWith(".js"))
    return path;
  auto canonical = QFileInfo(path).canonicalFilePath();
  if (canonical.endsWith(".js"))
    return canonical;
  for (auto candidate :
       {QFileInfo(path).absolutePath() + "/node_modules/npm/bin/npm-cli.js",
        QFileInfo(U::node()).absolutePath() +
            "/node_modules/npm/bin/npm-cli.js",
        QString("/usr/share/nodejs/npm/bin/npm-cli.js"),
        QString("/usr/local/lib/node_modules/npm/bin/npm-cli.js")})
    if (QFileInfo(candidate).isFile())
      return candidate;
  return {};
}
void Backend::checkKernel() {
  if (kernelUpdating)
    return;
  auto npm = npmCli();
  if (npm.isEmpty()) {
    emit toast("未找到 npm，请安装带 npm 的 Node.js");
    return;
  }
  kernelUpdating = true;
  kernelMessage = "正在检查 Pi 最新版本…";
  emit changed();
  U::run(
      this, U::node(),
      {npm, "view", "@earendil-works/pi-coding-agent", "version"}, {},
      [this](bool ok, QByteArray raw, QString) {
        kernelUpdating = false;
        auto version = QString::fromUtf8(raw).trimmed();
        if (ok && QRegularExpression("^\\d+\\.\\d+\\.\\d+$")
                      .match(version)
                      .hasMatch()) {
          latestKernel = version;
          kernelMessage = version == kernelVersion() ? "已是最新 Pi 版本"
                                                     : "可更新至 Pi " + version;
        } else
          kernelMessage = "检查失败，请检查 npm 软件源与网络";
        emit changed();
      },
      45000);
}
void Backend::updateKernel() {
  if (kernelUpdating || modelsBusy()) {
    emit toast("请等待所有任务结束后更新内核");
    return;
  }
  if (latestKernel.isEmpty()) {
    checkKernel();
    return;
  }
  auto npm = npmCli();
  if (npm.isEmpty())
    return;
  auto staging = store.directory + "/kernels/staging-" + U::uuid();
  QDir().mkpath(staging);
  kernelUpdating = true;
  kernelMessage = "正在安装并验证 Pi " + latestKernel;
  emit changed();
  U::run(
      this, U::node(),
      {npm, "install", "--prefix", staging, "--ignore-scripts", "--no-audit",
       "--no-fund", "@earendil-works/pi-coding-agent@" + latestKernel},
      {},
      [this, staging](bool ok, QByteArray, QString error) {
        if (!ok) {
          kernelUpdating = false;
          kernelMessage = "Pi 安装失败：" + error;
          emit changed();
          return;
        }
        auto target = store.directory + "/kernels/coding-" + latestKernel +
                      "-" + U::uuid().left(8);
        U::run(
            this, U::node(),
            {root + "/scripts/prepare-runtime.mjs", "--source", staging,
             "--output", target},
            {},
            [this, target](bool projected, QByteArray, QString error) {
              if (!projected) {
                kernelUpdating = false;
                kernelMessage = "Pi 适配失败：" + error;
                emit changed();
                return;
              }
              U::run(
                  this, U::node(), {target + "/bridge.mjs", "--self-test"}, {},
                  [this, target](bool passed, QByteArray raw, QString) {
                    kernelUpdating = false;
                    if (passed && raw.contains("\"ok\":true")) {
                      U::writeJson(store.directory + "/kernels/active.json",
                                   QVariantMap{{"directory",
                                                QFileInfo(target).fileName()},
                                               {"version", latestKernel}});
                      release = target;
                      invalidate();
                      kernelMessage = "Pi 已更新至 " + latestKernel;
                      management->scan();
                    } else
                      kernelMessage =
                          "新 Pi 版本未通过兼容性验证，已保留当前内核";
                    emit changed();
                  },
                  60000);
            },
            120000);
      },
      240000);
}

PiManagement::PiManagement(Backend *backend) : QObject(backend), b(backend) {
  auto base = QDir(b->store.directory + "/pi-backups");
  auto entries = base.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
  if (!entries.isEmpty())
    m_backup = base.filePath(entries.last());
}
void PiManagement::resource(QVariantMap action, bool reload) {
  if (m_busy || b->kernelUpdating)
    return;
  m_busy = true;
  m_status = "正在读取 Pi 扩展…";
  emit changed();
  auto cwd = b->view->workspace.isEmpty() ? b->store.directory + "/workspace"
                                          : b->view->workspace;
  QDir().mkpath(cwd);
  action["cwd"] = cwd;
  action["agentDir"] = b->agentDir();
  action["agent"] = b->store.data["agent"];
  U::run(
      this, U::node(), {b->release + "/resources.mjs"},
      QJsonDocument::fromVariant(action).toJson(QJsonDocument::Compact),
      [this, reload](bool ok, QByteArray raw, QString error) {
        m_busy = false;
        auto result = QJsonDocument::fromJson(raw).object();
        if (ok && result["ok"].toBool()) {
          m_extensions = result["extensions"].toArray().toVariantList();
          m_status = QString("已发现 %1 个扩展").arg(m_extensions.size());
          if (reload)
            b->invalidate();
        } else
          m_status = result["error"].toString(
              error.isEmpty()
                  ? "Pi 配置读取失败，可在原生终端修复或恢复出厂内核"
                  : error);
        emit changed();
        emit b->changed();
      },
      45000);
}
void PiManagement::scan() { resource({{"action", "scan"}}); }
void PiManagement::refresh() { resource({{"action", "scan"}}, true); }
void PiManagement::setExtensionEnabled(QString id, bool enabled) {
  resource({{"action", "toggle"}, {"id", id}, {"enabled", enabled}}, true);
}
void PiManagement::openCatalog() {
  QDesktopServices::openUrl(QUrl("https://pi.dev/packages"));
}
void PiManagement::resetInstall() {
  if (m_installState == "installing")
    return;
  m_installState = "idle";
  m_installMessage.clear();
  emit changed();
}
void PiManagement::installExtension(QString text, bool local) {
  if (m_busy || b->kernelUpdating)
    return;
  text = text.trimmed();
  if (text.isEmpty() || text.size() > 4096 ||
      text.contains(QRegularExpression("[\\r\\n\\x00;&|`$]"))) {
    m_installState = "failed";
    m_installMessage =
        "请粘贴一条 pi install 命令，例如 pi install npm:billion-context";
    emit changed();
    return;
  }
  auto tokens = QProcess::splitCommand(text);
  if (tokens.size() >= 2 && tokens[0] == "pi" && tokens[1] == "install") {
    tokens.removeFirst();
    tokens.removeFirst();
  }
  for (int i = tokens.size() - 1; i >= 0; --i)
    if (tokens[i] == "-l" || tokens[i] == "--local") {
      local = true;
      tokens.removeAt(i);
    }
  if (tokens.size() != 1 || tokens[0].startsWith('-')) {
    m_installState = "failed";
    m_installMessage = "一次安装一个扩展，支持 --local / -l";
    emit changed();
    return;
  }
  auto source = tokens[0];
  if (source.startsWith('\'') && source.endsWith('\''))
    source = source.mid(1, source.size() - 2);
  auto cwd = b->view->workspace.isEmpty() ? b->store.directory + "/workspace"
                                          : b->view->workspace;
  QDir().mkpath(cwd);
  m_busy = true;
  m_installState = "installing";
  m_installMessage = "正在通过内嵌 Pi 安装扩展…";
  emit changed();
  auto p = new QProcess(this);
  U::hideConsole(p);
  auto timer = new QTimer(p);
  timer->setSingleShot(true);
  auto env = U::environment();
  env.remove("PI_OFFLINE");
  env.insert("PI_CODING_AGENT_DIR", b->agentDir());
  env.insert("PI_SKIP_VERSION_CHECK", "1");
  env.insert("NO_COLOR", "1");
  p->setProcessEnvironment(env);
  p->setWorkingDirectory(cwd);
  QStringList args{
      b->release + "/node_modules/@earendil-works/pi-coding-agent/dist/cli.js",
      "install", source, local ? "--approve" : "--no-approve"};
  if (local)
    args << "--local";
  auto settled = std::make_shared<bool>(false);
  auto finish = [this, p, timer, settled](bool ok) {
    if (*settled)
      return;
    *settled = true;
    timer->stop();
    m_busy = false;
    m_installState = ok ? "succeeded" : "failed";
    m_installMessage = ok ? "安装成功，已应用到 Pi 原生配置"
                          : "安装失败，请检查扩展来源、依赖和网络";
    p->readAllStandardError();
    p->readAllStandardOutput();
    emit changed();
    if (ok) {
      b->invalidate();
      scan();
    }
    p->deleteLater();
  };
  connect(p, &QProcess::started, p, [p] { p->closeWriteChannel(); });
  connect(p, &QProcess::readyReadStandardOutput, p,
          [p] { p->readAllStandardOutput(); });
  connect(p, &QProcess::readyReadStandardError, p,
          [p] { p->readAllStandardError(); });
  connect(p, &QProcess::finished, this,
          [finish](int code, QProcess::ExitStatus status) {
            finish(code == 0 && status == QProcess::NormalExit);
          });
  connect(p, &QProcess::errorOccurred, this,
          [finish](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart)
              finish(false);
          });
  connect(timer, &QTimer::timeout, this, [p, finish] {
    p->kill();
    finish(false);
  });
  timer->start(180000);
  p->start(U::node(), args);
}
void PiManagement::setRecoveryMode(bool enabled) {
  if (m_busy || b->kernelUpdating)
    return;
  auto agent = b->store.data["agent"].toMap();
  agent["safeMode"] = enabled;
  b->store.data["agent"] = agent;
  b->store.save();
  b->invalidate();
  emit b->changed();
  scan();
}
void PiManagement::restoreFactory() {
  if (m_busy || b->kernelUpdating || b->modelsBusy()) {
    emit b->toast("请等待所有任务结束后恢复 Pi 内核");
    return;
  }
  m_busy = true;
  b->kernelUpdating = true;
  m_status = "正在备份配置并恢复内嵌 Pi…";
  emit changed();
  emit b->changed();
  auto stamp = QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + "-" +
               U::uuid().left(8);
  auto backup = b->store.directory + "/pi-backups/" + stamp;
  auto fresh = b->store.directory + "/pi-profiles/" + stamp;
  auto target = b->store.directory + "/kernels/factory-" + stamp;
  QDir().mkpath(backup);
  QDir().mkpath(fresh);
  U::writeJson(backup + "/restore.json",
               QVariantMap{{"agent", b->store.data["agent"]},
                           {"workspace", b->view->workspace}});
  for (auto pair : QList<QPair<QString, QString>>{
           {b->agentDir() + "/settings.json", "personal-settings.json"},
           {b->view->workspace + "/.pi/settings.json", "project-settings.json"},
           {b->store.directory + "/kernels/active.json", "active.json"}})
    if (QFileInfo(pair.first).isFile())
      QFile::copy(pair.first, backup + "/" + pair.second);
  auto source = b->root + "/runtime/releases/coding-1.3.0";
  auto watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [=, this] {
    auto error = watcher->result();
    watcher->deleteLater();
    if (!error.isEmpty()) {
      m_busy = false;
      b->kernelUpdating = false;
      m_status = "恢复失败，原内核仍在使用";
      emit changed();
      emit b->changed();
      return;
    }
    U::run(
        this, U::node(), {target + "/bridge.mjs", "--self-test"}, {},
        [=, this](bool ok, QByteArray raw, QString) {
          m_busy = false;
          b->kernelUpdating = false;
          if (ok && raw.contains("\"ok\":true")) {
            auto agent = Store::defaults()["agent"].toMap();
            agent["agentDir"] = fresh;
            agent["safeMode"] = true;
            b->store.data["agent"] = agent;
            b->store.save();
            U::writeJson(
                b->store.directory + "/kernels/active.json",
                QVariantMap{{"directory", QFileInfo(target).fileName()},
                            {"version", "1.3.0"}});
            b->release = target;
            b->invalidate();
            m_backup = backup;
            m_status = "出厂内核已恢复，恢复模式已启用；原配置已保留";
          } else
            m_status = "出厂内核验证未通过，未切换当前配置";
          emit changed();
          emit b->changed();
          scan();
        },
        60000);
  });
  watcher->setFuture(QtConcurrent::run([source, target] {
    QString error;
    U::copyTree(source, target, &error);
    return error;
  }));
}
void PiManagement::openBackup() {
  if (!m_backup.isEmpty())
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_backup));
}
static QString psQuote(QString value) {
  return "'" + value.replace("'", "''") + "'";
}
static QString shQuote(QString value) {
  return "'" + value.replace("'", "'\"'\"'") + "'";
}
void PiManagement::openTerminal() {
  if (m_busy || b->kernelUpdating)
    return;
  auto program = U::node();
  if (program.isEmpty()) {
    emit b->toast("需要 Node.js 22.19+");
    return;
  }
  m_busy = true;
  m_status = "正在准备原生 Pi 管理终端…";
  emit changed();
  auto source = b->release,
       target =
           b->store.directory + "/pi-console/runtime-" + b->kernelVersion();
  auto watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcher<QString>::finished, this, [=, this] {
    auto error = watcher->result();
    watcher->deleteLater();
    m_busy = false;
    if (!error.isEmpty()) {
      m_status = "终端运行时准备失败：" + error;
      emit changed();
      return;
    }
    auto cwd = b->store.data["agent"].toMap()["safeMode"].toBool()
                   ? b->store.directory + "/pi-console/safe-workspace"
                   : b->view->workspace;
    if (cwd.isEmpty())
      cwd = b->store.directory + "/workspace";
    QDir().mkpath(cwd);
    auto console = b->store.directory + "/pi-console";
    QDir().mkpath(console);
    auto cli =
        target + "/node_modules/@earendil-works/pi-coding-agent/dist/cli.js";
    QString script;
#if defined(Q_OS_WIN)
    auto path = console + "/manage-pi.ps1";
    script = "$env:PI_CODING_AGENT_DIR = " + psQuote(b->agentDir()) +
             "\n$env:PI_SKIP_VERSION_CHECK = '1'\n$env:PI_OFFLINE = "
             "'1'\nSet-Location -LiteralPath " +
             psQuote(cwd) + "\nfunction global:pi { & " + psQuote(program) +
             " " + psQuote(cli) +
             " @args }\npi config\nRemove-Item Env:PI_OFFLINE -ErrorAction "
             "SilentlyContinue\nWrite-Host 'Embedded Pi is ready. Use pi "
             "install, pi list, pi remove or pi config.'\n";
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
      file.write("\xef\xbb\xbf");
      file.write(script.toUtf8());
      file.commit();
    }
    QProcess terminal;
    terminal.setProgram("powershell.exe");
    terminal.setArguments({"-NoLogo", "-NoProfile", "-NoExit",
                           "-ExecutionPolicy", "Bypass", "-File", path});
    terminal.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) {
          args->flags = (args->flags & ~0x00000008UL) | 0x00000010UL;
          args->startupInfo->dwFlags &= ~0x00000100UL;
        });
    if (!terminal.startDetached())
      m_status = "无法打开本机终端";
#else
 auto path=console+"/manage-pi.sh";script="#!/bin/sh\nexport PI_CODING_AGENT_DIR="+shQuote(b->agentDir())+"\nexport PI_SKIP_VERSION_CHECK=1\ncd "+shQuote(cwd)+" || exit 1\npi() { "+shQuote(program)+" "+shQuote(cli)+" \"$@\"; }\nexport PI_OFFLINE=1\npi config\nunset PI_OFFLINE\nprintf '\\nEmbedded Pi: pi install, pi list, pi remove, pi config\\n'\nexec \"${SHELL:-/bin/sh}\"\n";
 // Persist the function in a session-only shell RC, so it remains available interactively.
 script=script.left(script.lastIndexOf("exec "))+"ENV="+shQuote(console+"/pi-env.sh")+" exec /bin/sh -i\n";QSaveFile envFile(console+"/pi-env.sh");if(envFile.open(QIODevice::WriteOnly)){envFile.write(("pi() { "+shQuote(program)+" "+shQuote(cli)+" \"$@\"; }\n").toUtf8());envFile.commit();}QSaveFile file(path);if(file.open(QIODevice::WriteOnly)){file.write(script.toUtf8());file.commit();}QFile::setPermissions(path,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
#ifdef Q_OS_MACOS
 auto command=console+"/manage-pi.command"; QFile::remove(command); QFile::copy(path,command); QFile::setPermissions(command,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
 if(!QProcess::startDetached("open",{"-a","Terminal",command})) m_status="无法打开本机终端";
#else
 auto terminal=QStandardPaths::findExecutable("x-terminal-emulator");QStringList args{"-e","sh",path};if(terminal.isEmpty()){terminal=QStandardPaths::findExecutable("gnome-terminal");args={"--","sh",path};}if(terminal.isEmpty()){terminal=QStandardPaths::findExecutable("konsole");args={"-e","sh",path};}if(terminal.isEmpty())m_status="未找到本机终端，请安装终端模拟器";else QProcess::startDetached(terminal,args);
#endif
#endif
    if (m_status.startsWith("正在"))
      m_status = "已打开内嵌 Pi 原生配置终端";
    emit changed();
  });
  watcher->setFuture(QtConcurrent::run([source, target] {
    QString error;
    if (!QFileInfo(target + "/manifest.json").isFile())
      U::copyTree(source, target, &error);
    return error;
  }));
}
