#pragma once
#include "common.hpp"

namespace RequestHeaders {
inline QVariantList defaults() {
  return {QVariantMap{{"id","content-type"},{"name","Content-Type"},{"value","application/json"},{"enabled",true}},
    QVariantMap{{"id","user-agent"},{"name","User-Agent"},{"value","Umeko-Desktop"},{"enabled",true}},
    QVariantMap{{"id","authorization"},{"name","Authorization"},{"value","Bearer {{apiKey}}"},{"enabled",true}},
    QVariantMap{{"id","x-api-key"},{"name","x-api-key"},{"value","{{apiKey}}"},{"enabled",true}},
    QVariantMap{{"id","anthropic-version"},{"name","anthropic-version"},{"value","2023-06-01"},{"enabled",true}}};
}
inline QVariantList validate(QVariant value) {
  auto fail=[](const char *message) { throw std::runtime_error(message); };
  if (value.typeId()!=QMetaType::QVariantList || value.toList().size()>32) fail("请求头最多可设置 32 项");
  const QSet<QString> managed{"host","content-length","transfer-encoding","connection","proxy-authorization","proxy-connection","upgrade","trailer","te"};
  const QRegularExpression idPattern("\\A[a-z0-9-]{1,64}\\z"), namePattern("\\A[!#$%&'*+.^_`|~0-9A-Za-z-]+\\z");
  QVariantList rows; QSet<QString> ids;
  for (const auto &item:value.toList()) {
    const auto row=item.toMap(); const auto id=row.value("id").toString();
    if (item.typeId()!=QMetaType::QVariantMap || row.keys()!=QStringList{"enabled","id","name","value"}
        || row["id"].typeId()!=QMetaType::QString || !idPattern.match(id).hasMatch() || ids.contains(id)
        || row["enabled"].typeId()!=QMetaType::Bool) fail("请求头配置格式无效");
    const auto name=row["name"].toString(), text=row["value"].toString();
    if (row["name"].typeId()!=QMetaType::QString || name.size()>128 || !namePattern.match(name).hasMatch()) fail("请求头名称只能包含英文字母、数字和 HTTP 字段符号");
    if (managed.contains(name.toLower())) fail("此请求头由网络传输管理，不能自定义");
    if (row["value"].typeId()!=QMetaType::QString || text.size()>4096) fail("请求头值不能包含换行、控制字符或非 Latin-1 字符，且最多 4096 字符");
    for (const auto c:text) if (c.unicode()<32 || c.unicode()==127 || c.unicode()>255) fail("请求头值不能包含换行、控制字符或非 Latin-1 字符，且最多 4096 字符");
    ids.insert(id); rows<<row;
  }
  QVariantList full;
  for (const auto &row:defaults()) if (!ids.contains(row.toMap()["id"].toString())) full<<row;
  full<<rows;
  if (full.size()>32) fail("请求头最多可设置 32 项");
  for (const auto &protocol:QStringList{"openai","anthropic"}) {
    QSet<QString> names;
    for (const auto &item:full) {
      const auto row=item.toMap(); const auto id=row["id"].toString();
      const auto scope=id=="authorization"?"openai":(id=="x-api-key"||id=="anthropic-version")?"anthropic":protocol;
      if (!row["enabled"].toBool() || scope!=protocol) continue;
      const auto name=row["name"].toString().toLower();
      if (names.contains(name)) fail("启用的请求头名称不能重复（不区分大小写）");
      names.insert(name);
    }
  }
  return full;
}
}
