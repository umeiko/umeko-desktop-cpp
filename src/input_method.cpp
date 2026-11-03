#include "input_method.hpp"

#include <QApplication>
#include <QInputMethod>
#include <QQuickWindow>

InputMethodUpdates::InputMethodUpdates(QApplication *application)
    : QObject(application), application(application), pending(this) {
  pending.setSingleShot(true);
  pending.setInterval(0);
  connect(&pending, &QTimer::timeout, this, &InputMethodUpdates::update);
  connect(application, &QApplication::focusObjectChanged, this,
          &InputMethodUpdates::schedule);
  connect(application, &QApplication::focusWindowChanged, this,
          &InputMethodUpdates::schedule);
}

void InputMethodUpdates::schedule() { pending.start(); }

void InputMethodUpdates::update() {
  auto *focus = qobject_cast<QQuickItem *>(application->focusObject());
  const bool editor = focus && focus->flags().testFlag(QQuickItem::ItemAcceptsInputMethod);
  QList<QQuickItem *> chain;
  if (editor)
    for (auto *item = focus; item; item = item->parentItem())
      chain.append(item);
  if (chain != items) {
    for (const auto &connection : connections)
      QObject::disconnect(connection);
    connections.clear();
    items = chain;
    for (auto *item : items) {
      for (auto signal : {&QQuickItem::xChanged, &QQuickItem::yChanged,
                          &QQuickItem::widthChanged, &QQuickItem::heightChanged,
                          &QQuickItem::scaleChanged, &QQuickItem::rotationChanged,
                          &QQuickItem::transformOriginChanged})
        connections.append(QObject::connect(item, signal, this,
                                            &InputMethodUpdates::schedule));
      connections.append(QObject::connect(item, &QQuickItem::parentChanged, this,
                                          &InputMethodUpdates::schedule));
    }
  }
  if (!editor)
    return;
  auto *window = qobject_cast<QQuickWindow *>(application->focusWindow());
  if (!window)
    return;
  auto *inputMethod = application->inputMethod();
  bool valid = false;
  const auto transform = focus->itemTransform(window->contentItem(), &valid);
  if (valid)
    inputMethod->setInputItemTransform(transform);
  inputMethod->update(Qt::ImCursorRectangle | Qt::ImAnchorRectangle | Qt::ImEnabled);
}
