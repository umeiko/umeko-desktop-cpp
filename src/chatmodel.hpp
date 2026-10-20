#pragma once
#include <QtCore>

class ChatModel : public QAbstractListModel {
  Q_OBJECT
public:
  enum { MessageRole = Qt::UserRole + 1 };
  using QAbstractListModel::QAbstractListModel;
  QVariantList rows;
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
  static QVariantList project(const QVariantList &messages);
};
