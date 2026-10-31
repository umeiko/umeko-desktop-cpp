#pragma once
#include <QtCore>
class Backend;

// Flat, virtualized tree rows. New batches insert into their conversation group
// without resetting QML delegates or touching the main conversation model.
class ConversationSearch : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(QString query READ query NOTIFY changed)
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(QString phase READ phase NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(int count READ count NOTIFY changed)
  Q_PROPERTY(int conversationCount READ conversationCount NOTIFY changed)
  Q_PROPERTY(bool limitHit READ limitHit NOTIFY changed)
  Q_PROPERTY(int skipped READ skipped NOTIFY changed)
public:
  explicit ConversationSearch(Backend *backend);
  ~ConversationSearch() override;
  QString query() const { return m_query; }
  bool busy() const { return m_busy; }
  QString phase() const { return m_phase; }
  QString error() const { return m_error; }
  int count() const { return m_count; }
  int conversationCount() const { return groups.size(); }
  bool limitHit() const { return m_limit; }
  int skipped() const { return m_skipped; }
  int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : flat.size(); }
  QVariant data(const QModelIndex &index,int role) const override;
  QHash<int,QByteArray> roleNames() const override { return {{Qt::UserRole+1,"modelData"}}; }
  Q_INVOKABLE void search(QString text,QVariantMap options = {});
  Q_INVOKABLE void clear();
  Q_INVOKABLE void cancel();
  Q_INVOKABLE void toggle(QString sessionId);
  Q_INVOKABLE void collapseAll();
  Q_INVOKABLE QVariantMap item(int row) const;
  Q_INVOKABLE void open(int row);
  void close();
signals:
  void changed();
  void jumpReady(int row,QString entryId);
private:
  struct Group { QString id,title;QVariantList hits;bool collapsed=false; };
  Backend *b;
  QString m_query,m_phase,m_error;
  bool m_busy=false,m_limit=false;
  int m_count=0,m_skipped=0,generation=0,jumpToken=0,jumpGeneration=-1000000;
  QList<Group> groups;
  QList<QPair<int,int>> flat;
  QProcess *worker=nullptr;
  QByteArray buffer;
  QTimer outputPump;
  void drainOutput();
  QVariantMap pendingJump;
  void receive(QVariantMap event);
  void add(QVariantList items);
  void rebuild();
  void write(QVariantMap job);
  bool start();
  void finishJump();
};
