#pragma once
#include "common.hpp"
#include <QNetworkProxyFactory>

// Called on a worker: Windows PAC and desktop settings can block.
inline QString updateProxy(const QUrl &url) {
  const auto ignored = qEnvironmentVariable("no_proxy", qEnvironmentVariable("NO_PROXY")).split(',');
  for (auto host : ignored) {
    host = host.trimmed().toLower();
    if (host == "*") return {};
    const auto colon = host.lastIndexOf(':');
    if (colon >= 0) {
      if (host.mid(colon + 1).toInt() != url.port(443)) continue;
      host = host.left(colon);
    }
    if (host.startsWith("*.")) host.remove(0, 2);
    else if (host.startsWith('.')) host.remove(0, 1);
    if (!host.isEmpty() && (url.host().toLower() == host || url.host().toLower().endsWith('.' + host))) return {};
  }
  for (const auto key : {"https_proxy", "HTTPS_PROXY", "all_proxy", "ALL_PROXY", "http_proxy", "HTTP_PROXY"}) {
    auto address = qEnvironmentVariable(key);
    if (!address.isEmpty()) {
      if (!address.contains("://")) address.prepend("http://");
      return address;
    }
  }
  for (const auto &proxy : QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(url))) {
    if ((proxy.type() == QNetworkProxy::HttpProxy || proxy.type() == QNetworkProxy::Socks5Proxy) && !proxy.hostName().isEmpty()) {
      QUrl address; address.setScheme(proxy.type() == QNetworkProxy::Socks5Proxy ? "socks5" : "http");
      address.setHost(proxy.hostName()); address.setPort(proxy.port());
      address.setUserName(proxy.user()); address.setPassword(proxy.password());
      return address.toString(QUrl::FullyEncoded);
    }
  }
#ifdef Q_OS_LINUX
  // Official Qt Linux binaries may lack libproxy: read GNOME/Cinnamon's manual
  // desktop proxy as well. Native Qt handles PAC on supported OS builds.
  auto setting = [](QString schema, QString key) {
    QProcess process; process.start("gsettings", {"get", "org.gnome.system.proxy" + schema, key});
    if (!process.waitForFinished(1500)) { process.kill(); process.waitForFinished(); return QString{}; }
    auto value = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    if (value.startsWith('\'') && value.endsWith('\'')) value = value.mid(1, value.size()-2);
    return value;
  };
  if (setting({}, "mode") == "manual") {
    for (const auto &schema : {QString(".https"), QString(".http"), QString(".socks")}) {
      const auto host = setting(schema, "host"); const auto port = setting(schema, "port").toInt();
      if (host.isEmpty() || port <= 0 || port > 65535) continue;
      QUrl address; address.setScheme(schema == ".socks" ? "socks5" : "http");
      address.setHost(host); address.setPort(port);
      if (schema != ".socks" && setting(".http", "use-authentication") == "true") {
        address.setUserName(setting(".http", "authentication-user"));
        address.setPassword(setting(".http", "authentication-password"));
      }
      return address.toString(QUrl::FullyEncoded);
    }
  }
#endif
  return {};
}
inline QNetworkProxy qtUpdateProxy(const QString &address) {
  if (address.isEmpty()) return QNetworkProxy(QNetworkProxy::NoProxy);
  const QUrl url(address);
  return QNetworkProxy(url.scheme().startsWith("socks5") ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy,
                       url.host(), url.port(url.scheme().startsWith("socks5") ? 1080 : 80), url.userName(), url.password());
}
