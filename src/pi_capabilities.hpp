#pragma once
#include <QVariantMap>
#include <QStringList>

// In-memory per-chat selections. Personal/project Pi settings stay untouched.
namespace PiCapabilities {
inline const QString nativeId = "builtin:pi-tools";
inline const QStringList core{"read","bash","powershell","edit","write","grep","find","ls"};
QVariantMap normalize(QVariantMap value, QStringList legacyBlocked = {});
QVariantMap effective(QVariantMap value);
bool pluginEnabled(QVariantMap value, QString source, bool fallback = true);
bool toolEnabled(QVariantMap value, QVariantMap tool, bool fallback = true);
QVariantList catalog(QVariantMap resources);
QVariantMap mergeResources(QVariantMap previous, QVariantMap current, QVariantMap profile);
QVariantMap change(QVariantMap profile, QString action, QVariantMap args, QVariantList extensions, QVariantMap resources);
}
