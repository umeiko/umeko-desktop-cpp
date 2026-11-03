#pragma once
#include <QtCore>

// The view requests individual bounded blocks; the full document never crosses
// the QML property boundary on load or when source/rendered mode is changed.
class DocumentBlocks : public QAbstractListModel {
  Q_OBJECT
public:
  using QAbstractListModel::QAbstractListModel;
  QVariantList rendered, source, rows;
  int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : rows.size(); }
  QVariant data(const QModelIndex &index, int role) const override {
    return index.isValid() && index.row() >= 0 && index.row() < rows.size() && role == Qt::UserRole+1 ? rows[index.row()] : QVariant();
  }
  QHash<int,QByteArray> roleNames() const override { return {{Qt::UserRole+1,"block"}}; }
  void load(QVariantList rich, QVariantList plain, QString mode) {
    beginResetModel(); rendered=std::move(rich);source=std::move(plain);rows=mode=="source" ? source : rendered;endResetModel();
  }
  void setMode(QString mode) { beginResetModel();rows=mode=="source" ? source : rendered;endResetModel(); }
};
