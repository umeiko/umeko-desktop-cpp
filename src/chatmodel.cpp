#include "chatmodel.hpp"
#include "common.hpp"
#include <QtConcurrent>
#include <QTextDocument>

QVariantList ChatModel::project(const QVariantList &messages) {
  QVariantList out;
  for (auto v : messages) {
    auto m = v.toMap();
    m["_entries"] = QStringList{m["entryId"].toString(),m["_searchId"].toString()};
    if (m["role"] == "user") {
      auto parsed = U::splitAttachments(m["text"].toString());
      m["text"] = parsed.first;
      if (!parsed.second.isEmpty())
        m["attachments"] = parsed.second;
    }
    if (m["role"] == "assistant" && m["customType"]!="umeko-compaction") {
      const QRegularExpression marker("\\s*\\[Umeko:([a-f0-9-]{36})\\]");
      const auto match = marker.match(m["text"].toString());
      if (match.hasMatch()) {
        m["diagnosticId"] = match.captured(1);
        const auto summary=QRegularExpression("供应商返回 HTTP \\d+：.*?(?=\\s*\\[Umeko:|$)").match(m["text"].toString());
        m["text"] = summary.hasMatch()?summary.captured().trimmed():QString("供应商请求失败，请查看请求详情");
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
        if (!tool["subagent"].toMap().isEmpty()) {
          if (!batch.isEmpty()) { activity << QVariantMap{{"kind","tools"},{"tools",batch}}; batch.clear(); }
          if (!activity.isEmpty() && activity.last().toMap()["kind"] == "subagents") {
            auto step=activity.last().toMap();auto agents=step["agents"].toList();agents<<tool["subagent"];step["agents"]=agents;activity.last()=step;
          } else activity << QVariantMap{{"kind","subagents"},{"agents",QVariantList{tool["subagent"]}}};
          continue;
        }
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
    const bool continuing = !out.isEmpty() &&
                            out.last().toMap()["stopReason"] == "length" &&
                            !m["error"].toBool();
    if (!out.isEmpty() && m["customType"]!="umeko-compaction" && out.last().toMap()["customType"]!="umeko-compaction" && m["role"] == "assistant" &&
        out.last().toMap()["role"] == "assistant" &&
        (continuing || !out.last().toMap()["tools"].toList().isEmpty() ||
         !m["tools"].toList().isEmpty())) {
      auto group = out.last().toMap();
      auto entries = group["_entries"].toStringList();
      entries.append(m["entryId"].toString());
      entries.append(m["_searchId"].toString());
      group["_entries"] = entries;
      if (m.contains("diagnosticId"))
        group["diagnosticId"] = m["diagnosticId"];
      auto activity = group["activity"].toList();
      auto steps = m["activity"].toList();
      if (continuing) {
        QVariantList tail, head;
        const auto prose = [](const QVariant &value) {
          const auto kind = value.toMap()["kind"].toString();
          return kind == "thinking" || kind == "text";
        };
        while (!activity.isEmpty() && prose(activity.last()))
          tail.prepend(activity.takeLast());
        while (!steps.isEmpty() && prose(steps.first()))
          head.append(steps.takeFirst());
        tail.append(head);
        for (const auto kind : {"thinking", "text"}) {
          QString text;
          for (const auto &value : tail) {
            const auto step = value.toMap();
            if (step["kind"] == kind)
              text += step["text"].toString();
          }
          if (!text.isEmpty()) {
            QVariantMap step{{"kind", kind}, {"text", text}};
            if (QString(kind) == "text")
              step["error"] = false;
            activity.append(step);
          }
        }
      }
      if (!activity.isEmpty() && !steps.isEmpty() &&
          activity.last().toMap()["kind"] == steps.first().toMap()["kind"] &&
          QStringList{"tools","subagents"}.contains(steps.first().toMap()["kind"].toString())) {
        auto step = activity.last().toMap();
        const auto field=step["kind"]=="tools"?"tools":"agents";
        auto tools = step[field].toList();
        tools.append(steps.takeFirst().toMap()[field].toList());
        step[field] = tools;
        activity.last() = step;
      }
      activity.append(steps);
      group["activity"] = activity;
      auto tools = group["tools"].toList();
      tools.append(m["tools"].toList());
      group["tools"] = tools;
      group["text"] = (continuing ? group["text"].toString() : QString()) +
                      m["text"].toString();
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
int ChatModel::rowForEntry(const QString &entry) const {
  for(int i=0;i<rows.size();++i)if(rows[i].toMap()["_entries"].toStringList().contains(entry))return i;
  return -1;
}
ChatModel::ChatModel(QObject *parent):QAbstractListModel(parent) {}
QVariant ChatModel::data(const QModelIndex &idx,int role) const {
  if(!idx.isValid() || idx.row()<0 || idx.row()>=rows.size() || role!=MessageRole)return {};
  if(!paged())return rows[idx.row()];
  if(prepared.contains(idx.row()))return prepared[idx.row()];
  auto row=rows[idx.row()].toMap();
  return QVariantMap{{"role",row["role"]},{"text",""},{"_entries",row["_entries"]},{"_loading",true},{"_estimatedHeight",heights.value(idx.row(),100)}};
}
void ChatModel::recordHeight(int row,double height){if(row>=0 && row<rows.size() && std::isfinite(height) && height>0)heights[row]=height;}
bool ChatModel::rangeReady(int first,int last) const {
  if(!paged())return true;
  for(int i=qMax(0,first);i<=qMin(last,int(rows.size())-1);++i)if(!prepared.contains(i))return false;
  return true;
}
void ChatModel::switchContext(QString token) {
  if(context==token)return;
  context=token;++revision;input.clear();pendingInput.clear();prepared.clear();cached.clear();heights.clear();
  beginResetModel();rows.clear();questionRows.clear();endResetModel();emit questionsChanged();
}
void ChatModel::seed(QVariantList messages,QVariantList projected,int anchor) {
  input=std::move(messages);pendingInput=input;prepared.clear();cached.clear();heights.clear();
  wantedFirst=qMax(0,anchor-4);wantedLast=qMax(3,anchor+6);
  publish(std::move(projected));
}
void ChatModel::update(const QVariantList &messages) {
  if(messages==input && !projecting)return;
  pendingInput=messages;projectPending();
}
void ChatModel::projectPending() {
  if(projecting)return;
  if(pendingInput.size()<=256) {input=pendingInput;publish(project(input));return;}
  projecting=true;const auto snapshot=pendingInput;
  auto watcher=new QFutureWatcher<QVariantList>(this);
  connect(watcher,&QFutureWatcher<QVariantList>::finished,this,[this,watcher,snapshot] {
    auto projected=watcher->result();watcher->deleteLater();projecting=false;
    if(snapshot==pendingInput){input=snapshot;publish(std::move(projected));}
    else projectPending();
  });watcher->setFuture(QtConcurrent::run([snapshot]{return project(snapshot);}));
}
void ChatModel::publish(QVariantList projected) {
  ++revision;
  if(projected.size()<rows.size())for(auto i:prepared.keys())if(i>=projected.size()){prepared.remove(i);cached.removeAll(i);}
  auto common = qMin(rows.size(), projected.size());
  bool questionsDirty = rows.size() != projected.size();
  for (int i = 0; i < common; ++i)
    if (rows[i] != projected[i]) {
      questionsDirty |= rows[i].toMap()["role"] == "user" ||
                        projected[i].toMap()["role"] == "user";
      prepared.remove(i);cached.removeAll(i);
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
  requestRange(wantedFirst,wantedLast);
  emit windowReady();

}

namespace {
struct SafeMarkdown:QTextDocument {QVariant loadResource(int,const QUrl &) override{return {};}};
QVariantList renderedText(QString text) {
  QVariantList blocks;const QRegularExpression fence("(?:^|\\n)```([^\\n]*)\\n([\\s\\S]*?)(?:\\n```|$)");
  auto add=[&](QString text,bool code,QString language={}) {
    for(qsizetype at=0;at<text.size();) {
      auto end=qMin(at+8192,text.size());const auto boundary=text.lastIndexOf('\n',end-1);if(boundary>at+4096)end=boundary+1;
      if(end<text.size() && text[end-1].isHighSurrogate())--end;
      auto part=text.mid(at,end-at);QVariantMap block{{"code",code},{"text",part},{"language",language}};
      if(!code){SafeMarkdown doc;doc.setMarkdown(part);auto html=doc.toHtml();html.remove(QRegularExpression("<img\\b[^>]*>",QRegularExpression::CaseInsensitiveOption));block["html"]=html;}
      blocks<<block;at=end;
    }
  };
  qsizetype position=0;auto matches=fence.globalMatch(text);
  while(matches.hasNext()){const auto m=matches.next();add(text.mid(position,m.capturedStart()-position),false);add(m.captured(2),true,m.captured(1).trimmed());position=m.capturedEnd();}
  add(text.mid(position),false);return blocks;
}
QVariantMap renderMessage(QVariantMap row) {
  auto activity=row["activity"].toList();
  for(auto &v:activity){auto step=v.toMap();if(step["kind"]=="text" && !step["error"].toBool())step["_rendered"]=renderedText(step["text"].toString());v=step;}
  row["activity"]=activity;
  if(activity.isEmpty() && row["role"]=="assistant" && !row["error"].toBool())row["_rendered"]=renderedText(row["text"].toString());
  return row;
}
}
void ChatModel::requestRange(int first,int last) {
  wantedFirst=qMax(0,first-4);wantedLast=qMin(int(rows.size())-1,qMax(first,last)+6);
  renderRange();
}
void ChatModel::renderRange() {
  if(!paged() || rendering || rows.isEmpty())return;
  QList<int> indices;QVariantList values;
  for(int i=wantedFirst;i<=wantedLast && indices.size()<32;++i)if(!prepared.contains(i)){indices<<i;values<<rows[i];}
  if(indices.isEmpty())return;
  rendering=true;
  auto watcher=new QFutureWatcher<QVariantList>(this);
  connect(watcher,&QFutureWatcher<QVariantList>::finished,this,[this,watcher,indices,values] {
    const auto result=watcher->result();watcher->deleteLater();rendering=false;
    {
      // Streaming may update the tail while historical rows are rendering.
      // Accept unchanged rows individually so a long reply cannot starve reads.
      for(int n=0;n<indices.size();++n){int i=indices[n];if(i>=rows.size() || rows[i]!=values[n])continue;prepared[i]=result[n].toMap();cached.removeAll(i);cached<<i;emit dataChanged(index(i),index(i),{MessageRole});}
      while(cached.size()>256){const int i=cached.takeFirst();prepared.remove(i);if(i<wantedFirst || i>wantedLast)emit dataChanged(index(i),index(i),{MessageRole});}
      emit windowReady();
    }
    renderRange();
  });
  watcher->setFuture(QtConcurrent::run([values]{QVariantList result;for(const auto &v:values)result<<renderMessage(v.toMap());return result;}));
}
