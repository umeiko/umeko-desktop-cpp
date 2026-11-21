#include "backend.hpp"
#include <limits>

namespace {
constexpr int maxGroupMembers = 16;
using ModelKey = QPair<QString, QString>;

ModelKey memberKey(const QVariantMap &member) {
  return {member["provider"].toString(), member["model"].toString()};
}
}

QVariantMap Backend::modelGroup() const {
  const auto saved = store.data["modelGroup"].toMap();
  const auto providers = store.data["providers"].toList();
  struct Projection {
    QVariantMap saved;
    QVariantList providers;
    QVariantMap result;
  };
  // Both inputs are implicitly shared. Reuse the projection on session changes.
  static thread_local Projection cache;
  if (!cache.result.isEmpty() && cache.saved == saved && cache.providers == providers)
    return cache.result;

  QHash<ModelKey, QVariantMap> models;
  QVariantList choices, members;
  for (const auto &value : config()["providers"].toList()) {
    const auto provider = value.toMap();
    for (const auto &entry : provider["models"].toList()) {
      const auto model = entry.toMap();
      QVariantMap row{{"provider", provider["id"]}, {"providerName", provider["name"]},
                      {"model", model["alias"]}, {"levels", U::reasoningLevels(model)},
                      {"contextWindow", model["contextWindow"]}, {"vision", model["vision"]}};
      models.insert(memberKey(row), row);
      choices << row;
    }
  }
  int available = 0;
  for (const auto &value : saved["members"].toList().mid(0, maxGroupMembers)) {
    const auto member = value.toMap();
    auto row = models.value(memberKey(member));
    const bool found = !row.isEmpty();
    if (!found) row = member;
    row["available"] = found;
    if (found)
      row["effort"] = U::reconcileEffort(member["effort"].toString(), row["levels"].toStringList());
    members << row;
    if (found) ++available;
  }
  const bool enabled = saved["enabled"].toBool();
  QVariantMap result{{"enabled", enabled}, {"ready", enabled && available > 0},
                     {"members", members}, {"choices", choices}, {"limit", maxGroupMembers}};
  cache = {saved, providers, result};
  return result;
}

QVariantMap Backend::modelGroupDescriptor() const {
  if (!store.data["modelGroup"].toMap()["enabled"].toBool())
    return {};
  const auto group = modelGroup();
  if (!group["ready"].toBool()) return {};
  int context = std::numeric_limits<int>::max();
  bool vision = false;
  for (const auto &value : group["members"].toList()) {
    const auto row = value.toMap();
    if (!row["available"].toBool()) continue;
    context = qMin(context, row["contextWindow"].toInt());
    vision |= row["vision"].toBool();
  }
  return {{"id", U::modelGroupId}, {"contextWindow", context},
          {"input", vision ? QStringList{"text", "image"} : QStringList{"text"}}};
}

QVariantMap Backend::gatewayModelGroup() const {
  const auto group = modelGroup();
  if (!group["ready"].toBool()) return {};
  QVariantList members;
  for (const auto &value : group["members"].toList()) {
    const auto row = value.toMap();
    if (row["available"].toBool())
      members << QVariantMap{{"provider", row["provider"]}, {"model", row["model"]}, {"effort", row["effort"]}};
  }
  return {{"id", U::modelGroupId}, {"members", members}};
}

bool Backend::modelGroupSelected() const {
  return view->provider == U::modelGroupProvider;
}

void Backend::rememberModelSelection(const std::shared_ptr<Session> &session) {
  const bool group = session->provider == U::modelGroupProvider;
  store.data["selectedModelGroup"] = group;
  // Features that require a specific provider keep using the last physical model.
  if (!group) {
    store.data["selectedProvider"] = session->provider;
    store.data["selectedModel"] = session->model;
  }
}

bool Backend::repairModelGroupSelection(const std::shared_ptr<Session> &session) {
  if (session->provider != U::modelGroupProvider || session->busy() ||
      (session->backend == "pi" && !modelGroupDescriptor().isEmpty()))
    return false;
  session->provider = store.data["selectedProvider"].toString();
  session->model = store.data["selectedModel"].toString();
  session->usage.clear();
  if (session->process) session->stale = true;
  if (session == view) store.data["selectedModelGroup"] = false;
  return true;
}

void Backend::selectModelGroup() {
  if (view->backend == "pi" && !modelGroupDescriptor().isEmpty())
    selectModel(U::modelGroupProvider, U::modelGroupId);
}

bool Backend::saveModelGroup(bool enabled, QString raw) {
  for (const auto &session : records) {
    if (session->provider != U::modelGroupProvider ||
        (!session->busy() && !session->subagentsActive)) continue;
    emit toast("请等待模型组任务结束后再修改");
    return false;
  }
  const auto document = QJsonDocument::fromJson(raw.toUtf8());
  const auto input = document.array();
  if (!document.isArray() || input.size() > maxGroupMembers) return false;

  const auto group = modelGroup();
  QHash<ModelKey, QStringList> levels;
  QHash<ModelKey, QString> retained;
  for (const auto &value : group["choices"].toList())
    levels.insert(memberKey(value.toMap()), value.toMap()["levels"].toStringList());
  for (const auto &value : group["members"].toList()) {
    const auto member = value.toMap();
    if (!member["available"].toBool()) retained.insert(memberKey(member), member["effort"].toString());
  }

  QVariantList members;
  QSet<ModelKey> seen;
  for (const auto &value : input) {
    const auto row = value.toObject().toVariantMap();
    const auto key = memberKey(row);
    const auto effort = row["effort"].toString();
    const bool valid = levels.contains(key)
        ? effort == "off" || levels.value(key).contains(effort)
        : retained.contains(key) && retained.value(key) == effort;
    if (seen.contains(key) || !valid)
      return false;
    seen.insert(key);
    members << QVariantMap{{"provider", key.first}, {"model", key.second}, {"effort", effort}};
  }
  const auto previous = store.data["modelGroup"];
  const QVariantMap next{{"enabled", enabled}, {"members", members}};
  if (previous == next) return true;
  const auto previousModel = modelGroupDescriptor();
  store.data["modelGroup"] = next;
  if (!store.save()) {
    store.data["modelGroup"] = previous;
    emit toast("模型组保存失败");
    return false;
  }
  // Only context and vision changes need to refresh Pi's model descriptor.
  if (modelGroupDescriptor() != previousModel) {
    for (const auto &session : records) {
      if (session->provider != U::modelGroupProvider) continue;
      if (session->process) session->stale = true;
      session->usage.clear();
    }
  }
  if (repairModelGroupSelection(view)) {
    indexSession(view);
    saveSessions();
    appearanceSave.start();
  }
  gatewayWrite({{"type", "model-group"}, {"modelGroup", gatewayModelGroup()}});
  emit modelGroupChanged();
  emit changed();
  emit reasoningChanged();
  emit contextUsageChanged();
  return true;
}
