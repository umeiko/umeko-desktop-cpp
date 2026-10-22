#include "chatmodel.hpp"
#include "common.hpp"

QVariantList ChatModel::project(const QVariantList &messages) {
  QVariantList out;
  for (auto v : messages) {
    auto m = v.toMap();
    if (m["role"] == "user") {
      auto parsed = U::splitAttachments(m["text"].toString());
      m["text"] = parsed.first;
      if (!parsed.second.isEmpty())
        m["attachments"] = parsed.second;
    }
    if (m["role"] == "assistant") {
      const QRegularExpression marker("\\s*\\[Umeko:([a-f0-9-]{36})\\]");
      const auto match = marker.match(m["text"].toString());
      if (match.hasMatch()) {
        m["diagnosticId"] = match.captured(1);
        m["text"] = "供应商请求失败，请查看请求详情";
      }
      QVariantList activity;
      if (!m["thinking"].toString().isEmpty())
        activity << QVariantMap{{"kind", "thinking"}, {"text", m["thinking"]}};
      if (!m["text"].toString().isEmpty())
        activity << QVariantMap{{"kind", "text"},
                                {"text", m["text"]},
                                {"error", m.value("error", false)}};
      QVariantList batch;
      for (auto value : m["tools"].toList()) {
        auto tool = value.toMap();
        batch << tool;
        if (tool["name"] == "submit_to_chatbox" &&
            !tool["media"].toList().isEmpty()) {
          activity << QVariantMap{{"kind", "tools"}, {"tools", batch}};
          activity << QVariantMap{
              {"kind", "media"},
              {"items", U::mediaView(tool["media"].toList())},
              {"toolId", tool["id"]}};
          batch.clear();
        }
      }
      if (!batch.isEmpty())
        activity << QVariantMap{{"kind", "tools"}, {"tools", batch}};
      m["activity"] = activity;
    }
    if (!out.isEmpty() && m["role"] == "assistant" &&
        out.last().toMap()["role"] == "assistant" &&
        (!out.last().toMap()["tools"].toList().isEmpty() ||
         !m["tools"].toList().isEmpty())) {
      auto group = out.last().toMap();
      if (m.contains("diagnosticId"))
        group["diagnosticId"] = m["diagnosticId"];
      auto activity = group["activity"].toList();
      auto steps = m["activity"].toList();
      if (!activity.isEmpty() && !steps.isEmpty() &&
          activity.last().toMap()["kind"] == "tools" &&
          steps.first().toMap()["kind"] == "tools") {
        auto step = activity.last().toMap();
        auto tools = step["tools"].toList();
        tools.append(steps.takeFirst().toMap()["tools"].toList());
        step["tools"] = tools;
        activity.last() = step;
      }
      activity.append(steps);
      group["activity"] = activity;
      auto tools = group["tools"].toList();
      tools.append(m["tools"].toList());
      group["tools"] = tools;
      group["text"] = m["text"];
      if (m.contains("timestamp"))
        group["timestamp"] = m["timestamp"];
      group["error"] = group["error"].toBool() || m["error"].toBool();
      group["stopReason"] = m.value("stopReason", "");
      out.last() = group;
    } else
      out << m;
  }
  for (int i = 0; i < out.size(); ++i) {
    auto m = out[i].toMap();
    auto steps = m["activity"].toList();
    for (int j = 0; j < steps.size(); ++j) {
      auto s = steps[j].toMap();
      s["blockOffset"] = j * 10000;
      steps[j] = s;
    }
    m["activity"] = steps;
    out[i] = m;
  }
  return out;
}
void ChatModel::update(const QVariantList &messages) {
  auto projected = project(messages);
  auto common = qMin(rows.size(), projected.size());
  bool questionsDirty = rows.size() != projected.size();
  for (int i = 0; i < common; ++i)
    if (rows[i] != projected[i]) {
      questionsDirty |= rows[i].toMap()["role"] == "user" ||
                        projected[i].toMap()["role"] == "user";
      rows[i] = projected[i];
      emit dataChanged(index(i), index(i), {MessageRole});
    }
  if (projected.size() > rows.size()) {
    auto first = rows.size();
    beginInsertRows({}, first, projected.size() - 1);
    for (int i = first; i < projected.size(); ++i)
      rows << projected[i];
    endInsertRows();
  } else if (projected.size() < rows.size()) {
    beginRemoveRows({}, projected.size(), rows.size() - 1);
    while (rows.size() > projected.size())
      rows.removeLast();
    endRemoveRows();
  }
  if (questionsDirty) {
    QVariantList questions;
    for (int i = 0; i < rows.size(); ++i) {
      const auto message = rows[i].toMap();
      if (message["role"] != "user")
        continue;
      auto title = message["text"].toString().trimmed();
      if (title.isEmpty()) {
        QStringList names;
        for (auto item : message["attachments"].toList())
          names << item.toMap()["name"].toString();
        title = names.join("、");
      }
      questions << QVariantMap{{"row", i},
                               {"title", title.isEmpty()
                                             ? QString("…")
                                             : title.left(500).simplified()}};
    }
    if (questions != questionRows) {
      questionRows = questions;
      emit questionsChanged();
    }
  }
}
