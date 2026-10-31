#pragma once
#include <QtCore>

class ChatModel : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(QVariantList questions READ questions NOTIFY questionsChanged)
public:
  enum { MessageRole = Qt::UserRole + 1 };
  using QAbstractListModel::QAbstractListModel;
  QVariantList rows;
  QVariantList questions() const { return questionRows; }
  int rowCount(const QModelIndex &parent = {}) const override {
    return parent.isValid() ? 0 : rows.size();
  }
  QVariant data(const QModelIndex &index, int role) const override {
    return index.isValid() && index.row() < rows.size() && role == MessageRole
               ? rows[index.row()]
               : QVariant();
  }
  QHash<int, QByteArray> roleNames() const override {
    return {{MessageRole, "modelData"}};
  }
  void update(const QVariantList &messages);
  int rowForEntry(const QString &entry) const;
  static QVariantList project(const QVariantList &messages);
signals:
  void questionsChanged();

private:
  QVariantList questionRows;
};
