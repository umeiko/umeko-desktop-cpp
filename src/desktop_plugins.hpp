#pragma once
#include "common.hpp"
class Backend;
class DesktopPlugins : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
  Q_PROPERTY(QVariantList sessionPlugins READ sessionPlugins NOTIFY changed)
  Q_PROPERTY(QVariantMap session READ session NOTIFY sessionChanged)
  Q_PROPERTY(QVariantMap page READ page NOTIFY pageChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QString status READ status NOTIFY changed)
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
  Q_INVOKABLE void importZip(QString url);
  Q_INVOKABLE void setEnabled(QString id, bool enabled);
  Q_INVOKABLE void setSessionEnabled(QString id, bool enabled);
  Q_INVOKABLE bool openPage(QString id);
  Q_INVOKABLE void closePage();
  Q_INVOKABLE QString call(QString method, QString arguments);
  Q_INVOKABLE void saveConfig(QString id, QString raw);
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

private:
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
  void manage(QVariantMap request, std::function<void(QVariantMap)> done);
};
