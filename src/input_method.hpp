#pragma once

#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QQuickItem>
#include <QTimer>

class QApplication;

class InputMethodUpdates : public QObject {
  Q_OBJECT
public:
  explicit InputMethodUpdates(QApplication *application);

private:
  void schedule();
  void update();

  QApplication *application;
  QList<QQuickItem *> items;
  QList<QMetaObject::Connection> connections;
  QTimer pending;
};
