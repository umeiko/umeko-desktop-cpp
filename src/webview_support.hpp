#pragma once
#include <QtCore>

// The frontend is loaded before QGuiApplication only when a backend is available.
// Missing Linux components therefore never start Chromium or load its libraries.
void configureWebView(const QString &resourceRoot);
QVariantMap webviewConfiguration();
