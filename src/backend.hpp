#pragma once
#include "chatmodel.hpp"
#include "store.hpp"
#include <QtQuick>

class Backend;
class Providers : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
  Q_PROPERTY(QVariantList packages READ packages NOTIFY changed)
  Q_PROPERTY(QVariantMap importError READ importError NOTIFY changed)
  Q_PROPERTY(QString pythonExecutable READ pythonExecutable NOTIFY changed)
public:
  explicit Providers(Backend *backend);
  QVariantList rows() const;
  QVariantList packages() const;
  QVariantMap importError() const { return m_importError; }
  QString pythonExecutable() const;
  QString resolvePython() const;
  Q_INVOKABLE bool importZip(QString url);
  Q_INVOKABLE bool addAccount(QString plugin, QString version);
  Q_INVOKABLE bool uninstall(QString plugin, QString version);
  Q_INVOKABLE void login(QString id, QString raw);
  Q_INVOKABLE void refreshModels(QString id);
  Q_INVOKABLE QString configJson(QString id) const;
  Q_INVOKABLE bool saveConfig(QString id, QString raw);
  Q_INVOKABLE bool saveModels(QString id, QString raw);
  Q_INVOKABLE void remove(QString id);
  Q_INVOKABLE bool exportResource(QString kind, QString url);
  Q_INVOKABLE QString resourceFile(QString kind) const;
  void complete(QString request, QString provider, QVariantMap body);
  void cancel(QString request);
  void close();
signals:
  void changed();

private:
  Backend *b;
  QSet<QString> busy;
  QVariantMap m_importError;
  QHash<QString, QString> statuses;
  QHash<QString, QProcess *> workers;
  QVariantMap manifest(QVariantMap provider) const;
  QString packageDir(QVariantMap provider) const;
  void invoke(QString id, QString method, QVariantMap params,
              std::function<void(QVariantMap)> done,
              std::function<void(QVariantMap)> event = {},
              QString requestKey = {});
  void applyModels(QString id, QVariantList models, QString label = {});
};

class PiManagement : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QVariantList extensions READ extensions NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  Q_PROPERTY(QString backupPath READ backupPath NOTIFY changed)
  Q_PROPERTY(QString installState READ installState NOTIFY changed)
  Q_PROPERTY(QString installMessage READ installMessage NOTIFY changed)
  Q_PROPERTY(QVariantMap npmConfig READ npmConfig NOTIFY changed)
  Q_PROPERTY(QString npmStatus READ npmStatus NOTIFY changed)
public:
  explicit PiManagement(Backend *backend);
  bool busy() const { return m_busy; }
  QVariantList extensions() const { return m_extensions; }
  QString status() const { return m_status; }
  QString backupPath() const { return m_backup; }
  QString installState() const { return m_installState; }
  QString installMessage() const { return m_installMessage; }
  QVariantMap npmConfig() const;
  QString npmStatus() const { return m_npmStatus; }
  Q_INVOKABLE bool saveNpmConfig(QString raw);
  Q_INVOKABLE void testNpmConfig();
  Q_INVOKABLE void scan();
  Q_INVOKABLE void refresh();
  Q_INVOKABLE void setExtensionEnabled(QString id, bool enabled);
  Q_INVOKABLE void openCatalog();
  Q_INVOKABLE void resetInstall();
  Q_INVOKABLE void installExtension(QString command, bool local);
  Q_INVOKABLE void openTerminal();
  Q_INVOKABLE void setRecoveryMode(bool enabled);
  Q_INVOKABLE void restoreFactory();
  Q_INVOKABLE void openBackup();

private:
  QString m_npmStatus;
  Backend *b;
  bool m_busy = false;
  QVariantList m_extensions;
  QString m_status, m_backup, m_installState = "idle", m_installMessage;
  void resource(QVariantMap action, bool invalidate = false);
signals:
  void changed();
};

class AppAbout : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap info READ info CONSTANT)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QVariantMap release READ release NOTIFY changed)
  Q_PROPERTY(QString checkedAt READ checkedAt NOTIFY changed)
public:
  explicit AppAbout(Backend *backend);
  QVariantMap info() const { return m_info; }
  bool busy() const { return m_busy; }
  QVariantMap release() const { return m_release; }
  QString checkedAt() const { return m_checked; }
  Q_INVOKABLE void checkUpdates();
  Q_INVOKABLE void openProject();
  Q_INVOKABLE void openAuthor();
  Q_INVOKABLE void openReleases();
  Q_INVOKABLE void copyInfo();
signals:
  void changed();

private:
  Backend *b;
  QVariantMap m_info, m_release;
  bool m_busy = false;
  QString m_checked;
  QNetworkAccessManager network;
};

struct Session {
  QString key = U::uuid(), id, file, workspace, model, provider,
          title = "新会话", draft, phase = "idle", action, status, greeting;
  QVariantList messages, attachments, queue, tree;
  QVariantMap resources{
      {"tools", QVariantList{}},       {"allTools", QVariantList{}},
      {"skills", QVariantList{}},      {"extensions", QVariantList{}},
      {"diagnostics", QVariantList{}}, {"commands", QVariantList{}},
      {"prompts", QVariantList{}},     {"mcp", QVariantList{}}},
      usage, approval;
  QPointer<QProcess> process;
  QByteArray buffer;
  bool ready = false, stale = false, optimistic = false, failed = false,
       cancelled = false, operation = false, followTail = true;
  int portrait = 0, scrollIndex = -1;
  double scrollY = -1;
  bool busy() const { return phase != "idle" || operation; }
};

class SessionPreview : public QObject {
  Q_OBJECT
public:
  explicit SessionPreview(Backend *backend);
  Q_INVOKABLE void prepare(QString id, int token);
  Q_INVOKABLE bool activate(int token);
  Q_INVOKABLE void cancel();
signals:
  void ready(int token, bool ok);

private:
  Backend *b;
  int request = 0;
  bool reading = false;
  QVariantMap pending, prepared;
  void start();
};

class Backend : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantMap config READ config NOTIFY changed)
  Q_PROPERTY(QVariantList customThemes READ customThemes NOTIFY changed)
  Q_PROPERTY(QVariantMap themeStyle READ themeStyle NOTIFY changed)
  Q_PROPERTY(QVariantList messages READ messages NOTIFY chatChanged)
  Q_PROPERTY(
      QVariantList displayMessages READ displayMessages NOTIFY chatChanged)
  Q_PROPERTY(QObject *messageModel READ messageModel CONSTANT)
  Q_PROPERTY(QObject *providerPlugins READ providerPlugins CONSTANT)
  Q_PROPERTY(QObject *piManagement READ piManagement CONSTANT)
  Q_PROPERTY(QObject *sessionPreview READ sessionPreview CONSTANT)
  Q_PROPERTY(QObject *appAbout READ appAbout CONSTANT)
  Q_PROPERTY(QString language READ language NOTIFY appearanceChanged)
  Q_PROPERTY(double fontScale READ fontScale NOTIFY appearanceChanged)
  Q_PROPERTY(QString userDataDirectory READ userDataDirectory CONSTANT)
  Q_PROPERTY(QVariantList projectRows READ projectRows NOTIFY sidebarChanged)
  Q_PROPERTY(int sidebarWidth READ sidebarWidth NOTIFY layoutChanged)
  Q_PROPERTY(bool sidebarCollapsed READ sidebarCollapsed NOTIFY layoutChanged)
  Q_PROPERTY(bool showArchived READ showArchived NOTIFY chatChanged)
  Q_PROPERTY(QString greeting READ greeting NOTIFY chatChanged)
  Q_PROPERTY(int portrait READ portrait NOTIFY chatChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY chatChanged)
  Q_PROPERTY(bool running READ running NOTIFY chatChanged)
  Q_PROPERTY(bool loading READ loading NOTIFY chatChanged)
  Q_PROPERTY(bool kernelBusy READ kernelBusy NOTIFY changed)
  Q_PROPERTY(QString kernelStatus READ kernelStatus NOTIFY changed)
  Q_PROPERTY(QString kernelVersion READ kernelVersion NOTIFY changed)
  Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY changed)
  Q_PROPERTY(QString activeSession READ activeSession NOTIFY chatChanged)
  Q_PROPERTY(QString sessionTitle READ sessionTitle NOTIFY chatChanged)
  Q_PROPERTY(QString workspaceName READ workspaceName NOTIFY changed)
  Q_PROPERTY(QString toolStatus READ toolStatus NOTIFY chatChanged)
  Q_PROPERTY(
      QVariantList attachments READ attachments NOTIFY attachmentsChanged)
  Q_PROPERTY(QVariantList queuedMessages READ queuedMessages NOTIFY chatChanged)
  Q_PROPERTY(
      QVariantMap pendingApproval READ pendingApproval NOTIFY approvalChanged)
  Q_PROPERTY(QVariantMap agentResources READ agentResources NOTIFY changed)
  Q_PROPERTY(
      QVariantMap contextUsage READ contextUsage NOTIFY contextUsageChanged)
  Q_PROPERTY(double scrollPosition READ scrollPosition NOTIFY viewChanged)
  Q_PROPERTY(int scrollAnchor READ scrollAnchor NOTIFY viewChanged)
  Q_PROPERTY(bool followTail READ followTail NOTIFY viewChanged)
  Q_PROPERTY(bool serverRunning READ serverRunning NOTIFY changed)
  Q_PROPERTY(QVariantList proxyKeys READ proxyKeys NOTIFY proxyKeysChanged)
  Q_PROPERTY(QStringList proxyAddresses READ proxyAddresses NOTIFY changed)
public:
  explicit Backend(QString root, QString directory = {},
                   QObject *parent = nullptr);
  ~Backend() override;
  Store store;
  QString root;
  QVariantMap config() const;
  QVariantList customThemes() const;
  QVariantMap themeStyle() const;
  Q_INVOKABLE bool importTheme(QString url);
  Q_INVOKABLE void removeTheme(QString id);
  Q_INVOKABLE void copyThemePrompt();
  QVariantList messages() const { return view->messages; }
  QVariantList displayMessages() const { return chat.rows; }
  QObject *messageModel() { return &chat; }
  QObject *providerPlugins() { return providers; }
  QObject *piManagement() { return management; }
  QObject *sessionPreview() { return previewLoader; }
  QObject *appAbout() { return about; }
  QString language() const {
    return store.data["ui"].toMap()["language"].toString();
  }
  double fontScale() const {
    return store.data["ui"].toMap().value("fontScale", 1.0).toDouble();
  }
  QString userDataDirectory() const { return store.directory; }
  Q_INVOKABLE void setLanguage(QString language);
  Q_INVOKABLE void setFontScale(double scale);
  Q_INVOKABLE bool setProviderProxy(QString id, QString mode, QString address);
  Q_INVOKABLE QString requestReport(QString id) const;
  QVariantList projectRows() const;
  int sidebarWidth() const {
    return store.data["ui"].toMap().value("sidebarWidth", 244).toInt();
  }
  bool sidebarCollapsed() const {
    return store.data["ui"].toMap()["sidebarCollapsed"].toBool();
  }
  bool showArchived() const { return archived; }
  QString greeting() const { return view->greeting; }
  int portrait() const { return view->portrait; }
  bool busy() const { return view->busy(); }
  bool running() const { return view->phase == "running"; }
  bool loading() const {
    return view->phase == "loading" || view->phase == "working";
  }
  bool kernelBusy() const { return kernelUpdating || gatewayPort == 0; }
  QString kernelStatus() const { return kernelMessage; }
  QString kernelVersion() const;
  QString latestVersion() const { return latestKernel; }
  QString activeSession() const { return view->id; }
  QString sessionTitle() const { return view->title; }
  QString workspaceName() const {
    return QFileInfo(store.data["workspace"].toString()).fileName();
  }
  QString toolStatus() const { return view->status; }
  QVariantList attachments() const { return view->attachments; }
  QVariantList queuedMessages() const;
  QVariantMap pendingApproval() const { return view->approval; }
  QVariantMap agentResources() const { return view->resources; }
  QVariantMap contextUsage() const { return view->usage; }
  double scrollPosition() const { return view->scrollY; }
  int scrollAnchor() const { return view->scrollIndex; }
  bool followTail() const { return view->followTail; }
  bool serverRunning() const { return publicRunning; }
  QVariantList proxyKeys() const;
  QStringList proxyAddresses() const;
  Q_INVOKABLE bool saveSidebarLayout(int width, bool collapsed);
  Q_INVOKABLE void saveViewport(int index, double offset, bool follow);
  Q_INVOKABLE void setConversationVisible(bool visible);
  Q_INVOKABLE void setDraft(QString text) { view->draft = text; }
  Q_INVOKABLE QVariantMap contextWindowSuggestion(QString name) {
    return U::contextSuggestion(name);
  }
  Q_INVOKABLE QVariantMap visionSuggestion(QString name) {
    return U::visionSuggestion(name);
  }
  Q_INVOKABLE void setTheme(QString theme);
  Q_INVOKABLE void copy(QString text);
  Q_INVOKABLE void setWorkspace(QString url);
  Q_INVOKABLE void openChatDirectory();
  Q_INVOKABLE void openChatboxPath(QString path, bool reveal);
  Q_INVOKABLE void copyChatboxFile(QString path);
  Q_INVOKABLE void toggleArchived();
  Q_INVOKABLE void togglePinned(QString kind, QString id);
  Q_INVOKABLE void archiveSession(QString id, bool archived);
  Q_INVOKABLE void archiveProject(QString id, bool archived);
  Q_INVOKABLE void removeProject(QString id);
  Q_INVOKABLE void openProject(QString id);
  Q_INVOKABLE void revealProject(QString id);
  Q_INVOKABLE void renameProject(QString id, QString title);
  Q_INVOKABLE void renameSession(QString id, QString title);
  Q_INVOKABLE void exportSession(QString id, QString url);
  Q_INVOKABLE void forkSessionById(QString id);
  Q_INVOKABLE void forkSession();
  Q_INVOKABLE void deleteSession(QString id);
  Q_INVOKABLE void toggleProject(QString id);
  Q_INVOKABLE void dropProjects(QString urls);
  Q_INVOKABLE void addAttachments(QString urls);
  Q_INVOKABLE bool pasteAttachments();
  Q_INVOKABLE void removeAttachment(QString id);
  Q_INVOKABLE void newSession();
  Q_INVOKABLE void openSession(QString id);
  Q_INVOKABLE void send(QString text);
  Q_INVOKABLE void steer(QString text);
  Q_INVOKABLE void editQueued(QString id);
  Q_INVOKABLE void steerQueued(QString id);
  Q_INVOKABLE void removeQueued(QString id);
  Q_INVOKABLE void stop();
  Q_INVOKABLE void selectModel(QString provider, QString model);
  Q_INVOKABLE bool saveProvider(QString id, QString name, QString api,
                                QString url, QString key, QString models);
  Q_INVOKABLE void removeProvider(QString id);
  Q_INVOKABLE void saveMappings(QString raw);
  Q_INVOKABLE void saveAgent(QString raw);
  Q_INVOKABLE void openPiFolder(QString kind);
  Q_INVOKABLE void openPiGithub();
  Q_INVOKABLE QString readMcp(QString scope);
  Q_INVOKABLE QString mcpConfigPath(QString scope);
  Q_INVOKABLE void saveMcp(QString scope, QString raw);
  Q_INVOKABLE void reconnectMcp(QString name);
  Q_INVOKABLE void pollCapabilities();
  Q_INVOKABLE void loadCapabilities();
  Q_INVOKABLE void refreshAgent();
  Q_INVOKABLE void respondApproval(bool allowed);
  Q_INVOKABLE void respondPiDialog(QString value);
  Q_INVOKABLE void checkKernel();
  Q_INVOKABLE void updateKernel();
  Q_INVOKABLE bool saveServer(int port, bool openai, bool anthropic,
                              bool lan = false);
  Q_INVOKABLE void toggleServer();
  Q_INVOKABLE void refreshProxyAddresses() { emit changed(); }
  Q_INVOKABLE QString generateProxyKey();
  Q_INVOKABLE QVariantMap takeCreatedProxyKey();
  Q_INVOKABLE bool saveProxyKey(QString id, QString name, QString value);
  Q_INVOKABLE void setProxyKeyEnabled(QString id, bool enabled);
  Q_INVOKABLE void revokeProxyKey(QString id);
  void loadNativeIndex();
  void close();
  void injectDemo();
  void gatewayWrite(QVariantMap payload);
  void configureGateway();
  void repairSelection();
  QVariantMap provider(QString id) const;
  bool modelsBusy(QString id = {}) const;
  QString agentDir() const;
  QString releaseDir() const { return release; }
  QVariantList modelCatalog() const;
  void invalidate();
  void notify();
signals:
  void appearanceChanged();
  void changed();
  void chatChanged();
  void transcriptChanged();
  void viewChanged();
  void sidebarChanged();
  void layoutChanged();
  void toast(QString message);
  void approvalChanged();
  void attachmentsChanged();
  void contextUsageChanged();
  void proxyKeysChanged();
  void editorText(QString text);

private:
  friend class Providers;
  friend class PiManagement;
  friend class AppAbout;
  friend class SessionPreview;
  ChatModel chat;
  Providers *providers;
  PiManagement *management;
  AppAbout *about;
  SessionPreview *previewLoader;
  std::shared_ptr<Session> view;
  QHash<QString, std::shared_ptr<Session>> records, drafts;
  QList<QString> lru;
  QVariantList sessionIndex;
  QSet<QString> collapsed;
  bool archived = false, chatVisible = true, closing = false,
       kernelUpdating = false, publicRunning = false;
  QString release, kernelMessage = "就绪", latestKernel, privateToken;
  QProcess *gateway = nullptr;
  QByteArray gatewayBuffer;
  int gatewayPort = 0;
  QVariantMap createdKey;
  QTimer streaming;
  QTimer appearanceSave;
  std::shared_ptr<Session> fresh(QString workspace = {});
  std::shared_ptr<Session> findSession(QString id);
  QVariantMap ensureProject(QString path, bool reopen = false);
  void activate(std::shared_ptr<Session> session);
  void trimPool();
  void retire(std::shared_ptr<Session> session);
  void indexSession(std::shared_ptr<Session> session);
  void markRead(std::shared_ptr<Session> session);
  void gatewayStart();
  void gatewayEvent(QVariantMap event);
  void writePi(std::shared_ptr<Session> session, QVariantMap payload);
  bool command(std::shared_ptr<Session> session, QString action,
               QVariantMap values = {});
  void event(std::shared_ptr<Session> session, QVariantMap payload);
  void sendText(QString text, bool steering);
  void sessionInfo(std::shared_ptr<Session> session, QVariantMap info);
  void restoreQueued(std::shared_ptr<Session> session, QStringList texts,
                     QVariantList images = {});
  void saveSessions();
  QVariantMap metadata(QString id) const;
  void setMetadata(QString id, QVariantMap values);
  void operateSession(QString id, QVariantMap action);
};
