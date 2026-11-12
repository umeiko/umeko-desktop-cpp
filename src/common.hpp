#pragma once
#include <QtCore>
#include <QtNetwork>
#include <cmath>
#include <functional>

namespace U {
QVariantMap compactionView(QVariantMap entry);
QVariantMap compactionStatus(QVariantMap row, QString active = {});
QString uuid();
QString stamp();
QString cleanPath(QString path);
QString localPath(QString url);
QString processDirectory(QString path);
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
qint64 providerContext(QVariantMap model);
QStringList reasoningLevels(QVariantMap model);
QString reconcileEffort(QString value, const QStringList &levels);
QVariantMap normalizeModel(QVariantMap model, QVariantMap previous = {});
int parseContext(const QVariant &value);
QVariantMap toolView(QString id, QString name, QVariantMap arguments,
                     QString cwd, QVariantMap result = {});
QVariantList mediaView(QVariantList items);
QString documentKind(QString path, QString kind = {});
QPair<QVariantList, QVariantList> preview(const QString &file, qint64 limit = 0, const QString &leaf = {});
QString selectedChatText(QString script, QVariantList rows, QVariantMap range);
QVariantMap writeSelectionAttachment(QString workspace, QString text);
QString attachmentPrompt(QString text, QVariantList attachments);
QPair<QString, QVariantList> splitAttachments(QString text);
} // namespace U
