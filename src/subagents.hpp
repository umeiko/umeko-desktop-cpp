#pragma once
#include "chatmodel.hpp"
#include <memory>
class Backend;
struct Session;

class Subagents : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList agents READ agents NOTIFY changed)
  Q_PROPERTY(QVariantList activeAgents READ activeAgents NOTIFY changed)
  Q_PROPERTY(int activeCount READ activeCount NOTIFY changed)
  Q_PROPERTY(QVariantMap selected READ selected NOTIFY changed)
  Q_PROPERTY(QVariantMap context READ context NOTIFY contextChanged)
  Q_PROPERTY(QObject *messageModel READ messageModel CONSTANT)
  Q_PROPERTY(
      QVariantList displayMessages READ displayMessages NOTIFY contextChanged)
  Q_PROPERTY(
      QVariantList queuedMessages READ queuedMessages NOTIFY queueChanged)
public:
  explicit Subagents(Backend *backend);
  QVariantList agents() const;
  QVariantList activeAgents() const;
  int activeCount() const { return activeAgents().size(); }
  QVariantMap selected() const;
  QVariantMap context() const;
  QObject *messageModel() { return &model; }
  QVariantList displayMessages() const { return model.rows; }
  QVariantList queuedMessages() const;
  Q_INVOKABLE void open(QString id);
  Q_INVOKABLE void closeView();
  Q_INVOKABLE void setDraft(QString text);
  Q_INVOKABLE bool addAttachments(QString raw);
  Q_INVOKABLE void removeAttachment(QString id);
  Q_INVOKABLE bool send(QString text);
  Q_INVOKABLE bool steer(QString text);
  Q_INVOKABLE void stop();
  Q_INVOKABLE void editQueued(QString id);
  Q_INVOKABLE void removeQueued(QString id);
  Q_INVOKABLE void steerQueued(QString id);
  Q_INVOKABLE void copy(QString text);
  void merge(std::shared_ptr<Session> record, QVariantMap row);
  void sync(std::shared_ptr<Session> record);
  void handle(std::shared_ptr<Session> record, QVariantMap event);
  void processReady(std::shared_ptr<Session> record);
  void processExit(std::shared_ptr<Session> record);
  void close();
signals:
  void changed();
  void contextChanged();
  void queueChanged();
  void requested();
  void messageSent(QString id);
  void draftRestored(QString id);

private:
  Backend *b;
  ChatModel model;
  std::shared_ptr<Session> owner;
  QString selectedId, error;
  bool loading = false, truncated = false;
  quint64 generation = 0;
  QVariantList messages;
  QHash<QString, QString> drafts;
  QHash<QString, QVariantList> attachments, queues, deferred;
  QHash<QString, QVariantMap> pending;
  QTimer stream, deadline, save;
  QString key(std::shared_ptr<Session> record, QString id) const;
  QVariantList project(std::shared_ptr<Session> record) const;
  bool command(std::shared_ptr<Session> record, QVariantMap input);
  bool submit(QString text, bool interrupt);
  bool queueCommand(QString action, QString id = {});
  void restore(std::shared_ptr<Session> record, QString id, QVariantList items);
  void loadFile(QString path);
  void publish();
  void liveEvent(QVariantMap event);
};
