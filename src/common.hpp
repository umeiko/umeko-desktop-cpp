#pragma once
#include <QtCore>
#include <QtNetwork>
#include <cmath>
#include <functional>

namespace U {
QString uuid();
QString stamp();
QString cleanPath(QString path);
QString localPath(QString url);
QString node();
QString python(QString configured = {});
QByteArray random(int count);
QString sha(const QByteArray &data);
QVariant readJson(const QString &file, QVariant fallback = {});
bool writeJson(const QString &file, const QVariant &data,
               QString *error = nullptr);
bool copyTree(const QString &source, const QString &destination,
              QString *error = nullptr);
QVariantMap validateNpmConfig(QVariantMap values, QString *error);
QMap<QString, QString> npmOverrides(QVariantMap values);
QProcessEnvironment environment(QVariantMap npm = {});
void hideConsole(QProcess *process);
void run(QObject *owner, const QString &program, const QStringList &arguments,
         const QByteArray &input,
         std::function<void(bool, QByteArray, QString)> done,
         int timeout = 45000, const QString &cwd = {}, QVariantMap npm = {});
QVariantMap contextSuggestion(QString name);
QVariantMap visionSuggestion(QString name);
QVariantMap normalizeModel(QVariantMap model, QVariantMap previous = {});
int parseContext(const QVariant &value);
QVariantMap toolView(QString id, QString name, QVariantMap arguments,
                     QString cwd, QVariantMap result = {});
QPair<QVariantList, QVariantList> preview(const QString &file);
QString attachmentPrompt(QString text, QVariantList attachments);
QPair<QString, QVariantList> splitAttachments(QString text);
} // namespace U
