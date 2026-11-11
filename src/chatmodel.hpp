#pragma once
#include <QtCore>

class ChatModel : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(bool paged READ paged NOTIFY questionsChanged)
  Q_PROPERTY(QVariantList questions READ questions NOTIFY questionsChanged)
public:
  enum { MessageRole = Qt::UserRole + 1 };
  explicit ChatModel(QObject *parent=nullptr);
  bool paged() const { return rows.size()>256; }
  Q_INVOKABLE void requestRange(int first,int last);
  Q_INVOKABLE bool isReady(int row) const {return rangeReady(row,row);}
  Q_INVOKABLE void setLayout(QString key){if(layout!=key){layout=key;heights.clear();}}
  Q_INVOKABLE void setGroupHeight(double pixels);
  Q_INVOKABLE int groupOf(int row);
  Q_INVOKABLE int groupFirst(int group);
  Q_INVOKABLE int groupLast(int group);
  Q_INVOKABLE int groupCount();
  Q_INVOKABLE double estimatedHeight(int row) const;
  Q_INVOKABLE double spanHeight(int first,int last) const;
  Q_INVOKABLE void requestGroups(int first,int last,int keep);
  static QVariantList recentCompactionInterval(const QVariantList &messages);

  Q_INVOKABLE void retryPage(int row) {requestRange(row,row);}
  Q_INVOKABLE void recordHeight(int row,double height);
  Q_INVOKABLE bool rangeReady(int first,int last) const;
  QVariantList rows;
  QVariantList questions() const { return questionRows; }
  int rowCount(const QModelIndex &parent = {}) const override {
    return parent.isValid() ? 0 : rows.size();
  }
  QVariant data(const QModelIndex &index, int role) const override;
  QHash<int, QByteArray> roleNames() const override {
    return {{MessageRole, "modelData"}};
  }
  void update(const QVariantList &messages);
  void switchContext(QString token);
  void seed(QVariantList messages, QVariantList projected, int anchor);
  int rowForEntry(const QString &entry) const;
  static QVariantList project(const QVariantList &messages);
signals:
  void questionsChanged();
  void windowReady();

private:
  void partition();
  double hint(int row) const;
  double groupPixels=0;
  QList<int> groups,priority;
  QString layout;
  QString context;
  QVariantList questionRows, input, pendingInput;
  QHash<int,QVariantMap> prepared;
  QHash<int,double> heights;
  QList<int> cached;
  quint64 revision=0;
  bool projecting=false, rendering=false;
  int wantedFirst=0,wantedLast=3;
  void publish(QVariantList projected);
  void projectPending();
  void renderRange();
};
