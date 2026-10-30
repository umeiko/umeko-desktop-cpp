#pragma once
#include "common.hpp"
class Backend;
class WidgetChannel;
class DesktopPet;
class GlobalAgents;
class DesktopPlugins : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
  Q_PROPERTY(QVariantList sessionPlugins READ sessionPlugins NOTIFY changed)
  Q_PROPERTY(QVariantMap session READ session NOTIFY sessionChanged)
  Q_PROPERTY(QVariantMap page READ page NOTIFY pageChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)
  Q_PROPERTY(QVariantMap recentPlugin READ recentPlugin NOTIFY changed)
  Q_PROPERTY(QVariantList iconCatalog READ iconCatalog CONSTANT)
  Q_PROPERTY(QObject *pet READ pet CONSTANT)
public:
  explicit DesktopPlugins(Backend *backend);
  ~DesktopPlugins() override;
  QVariantList rows() const;
  QVariantList sessionPlugins() const;
  QVariantMap session() const;
  QVariantMap page() const;
  bool busy() const { return m_busy; }
  QString status() const { return m_status; }
  Q_INVOKABLE QVariantMap details(QString id) const;
  Q_INVOKABLE QString importZip(QString url);
  Q_INVOKABLE void setEnabled(QString id, bool enabled);
  Q_INVOKABLE void setSessionEnabled(QString id, bool enabled);
  Q_INVOKABLE bool openPage(QString id);
  Q_INVOKABLE void closePage();
  Q_INVOKABLE QString call(QString method, QString arguments);
  Q_INVOKABLE QString saveConfig(QString id, QString raw);
  Q_INVOKABLE QString settingsCall(QString id, QString method, QString raw);
  QVariantMap recentPlugin() const;
  QVariantList iconCatalog() const { return icons; }
  QObject *pet() const;
  GlobalAgents *globalAgents() const { return coordinators; }
  QString callFor(QString id, QString method, QString raw, bool agent = false,
                  int timeout = 20000, QVariantMap session = {});
  QVariantMap execute(QString operation, QVariantMap args, QString callerKey);
  void observe(QString key, QVariantMap event);
  Q_INVOKABLE bool uninstall(QString id);
  Q_INVOKABLE bool exportSdk(QString url);
  void synchronize();
  void close();
signals:
  void changed();
  void sessionChanged();
  void pageChanged();
  void requested();
  void dismissed();
  void response(QString request, QVariantMap result);
  void managed(QString token, QVariantMap result);
  void event(QString topic, QVariantMap data);
  void openWithRequested(QString path);

private:
  friend class WidgetChannel;
  friend class DesktopPet;
  friend class GlobalAgents;
  friend class NativeUiTests;
  struct Worker {
    QPointer<QProcess> process;
    QByteArray buffer;
    QString token;
    bool ready = false;
    QVariantList queue;
    QSet<QString> pending;
  };
  QTimer contextRefresh;
  Backend *b;
  QString root, registry, m_page, m_error, m_status, storageKey;
  bool m_busy = false, m_closed = false;
  QVariantMap settings, entries, m_session;
  QHash<QString, std::shared_ptr<Worker>> workers;
  WidgetChannel *channel;
  DesktopPet *companion;
  GlobalAgents *coordinators;
  QVariantList icons;
  QSet<QString> failedServices;
  QString applicationToken, selection;
  bool application(QString id) const;
  QVariantMap iconFor(QString id) const;
  QVariantMap entry(QString id) const;
  QString packagePath(QString id) const;
  bool enabled(QString id) const;
  bool sessionEnabled(QString id) const;
  bool save();
  void start(QString id);
  void stop(QString id);
  void send(QString id, QVariantMap request);
  void receive(QString id, std::shared_ptr<Worker> worker);
  void hostEvent(QString id, QVariantMap event);
  QString manage(QVariantMap request, std::function<bool(QVariantMap)> done);
};
