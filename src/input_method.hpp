#pragma once

#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QQuickItem>
#include <QTimer>

class QGuiApplication;

class InputMethodUpdates : public QObject {
  Q_OBJECT
public:
  explicit InputMethodUpdates(QGuiApplication *application);

private:
  void schedule();
  void update();

  QGuiApplication *application;
  QList<QQuickItem *> items;
  QList<QMetaObject::Connection> connections;
  QTimer pending;
};
