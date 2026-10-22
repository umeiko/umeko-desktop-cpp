"""Private, one-request Provider API v1 worker. Requires Python 3.10+."""
from __future__ import annotations

import contextlib
import importlib
import json
import re
import signal
import sys
import traceback
import types
from pathlib import Path
from threading import Event

wire = sys.stdout


def send(kind, **values):
    wire.write(json.dumps({"type": kind, **values}, ensure_ascii=True, separators=(",", ":")) + "\n")
    wire.flush()


def main():
    if sys.version_info < (3, 10):
        send("error", message="Provider 插件需要 Python 3.10 或更新版本")
        return 1
    request = json.loads(sys.stdin.readline(32 * 1024 * 1024))
    sdk = str(Path(__file__).resolve().parent)
    sys.path.insert(0, sdk)
    from umeko_provider_sdk import Provider, ProviderContext, ProviderError
    from umeko_provider_sdk.network import network_scope, redact as redact_data
    from umeko_provider_sdk.packages import validate_config, validate_folder

    secrets = dict(request.get("secrets") or {})
    sensitive = [secrets, request.get("params", {}).get("credentials", {})]

    class MemorySecrets:
        def get(self, key, default=None):
            return secrets.get(key, default)

        def set(self, key, value):
            secrets[key] = value
            send("secrets", data=secrets)

        def delete(self, key):
            secrets.pop(key, None)
            send("secrets", data=secrets)

    cancelled = Event()
    if hasattr(signal, "SIGTERM"):
        def stop(*_):
            cancelled.set()
            raise SystemExit(0)
        signal.signal(signal.SIGTERM, stop)
    provider = None
    records = []
    try:
        folder = Path(request["package"]).resolve()
        manifest, defaults, schema = validate_folder(folder)
        config = request.get("config", defaults)
        validate_config(config, schema)
        namespace = "_umeko_provider_impl"
        module = types.ModuleType(namespace)
        module.__path__ = [str(folder)]
        sys.modules[namespace] = module
        vendor = folder / "vendor"
        if vendor.is_dir():
            sys.path.insert(1, str(vendor))
        entry, factory = manifest["entrypoint"].split(":")
        # Plugin debug output is not part of the wire protocol and is never shown in UI.
        with contextlib.redirect_stdout(sys.stderr), network_scope(request.get("proxy"), records if request.get("method") == "events" else None):
            implementation = importlib.import_module(namespace + "." + entry[:-3].replace("/", "."))
            provider = getattr(implementation, factory)(ProviderContext(config, Path(request["state_dir"]), MemorySecrets(), cancelled))
            if not isinstance(provider, Provider):
                raise ProviderError("供应商实现必须继承 umeko_provider_sdk.Provider")
            method, params = request["method"], request.get("params") or {}
            if method == "login":
                credentials = params["credentials"]
                details = provider.authenticate(credentials) or {}
                models = provider.list_models(refresh=True)
                provider.context.secrets.set("credentials", credentials)
                result = {"models": models, "label": str(details.get("label", "已登录"))[:100]}
            elif method == "models":
                result = {"models": provider.list_models(refresh=True)}
            elif method == "events":
                for event in provider.events(params["model"], params["messages"], params.get("tools", []),
                                             params.get("max_tokens", 4096), params.get("parameters", {})):
                    if cancelled.is_set():
                        break
                    if not isinstance(event, dict):
                        raise ProviderError("供应商事件必须为 JSON 对象")
                    if event.get('error'):
                        records.append({'providerEvent': event})
                        raise ProviderError('上游返回错误，请查看请求详情')
                    send("event", data=event)
                result = {}
            else:
                raise ProviderError("不支持的 Provider 操作")
        send("result", data=result)
        return 0
    except Exception as exc:
        message = str(exc)[:400] if isinstance(exc, ProviderError) else "供应商运行失败，请检查 Python 依赖、插件配置与网络"
        def redact(value):
            nonlocal message
            if isinstance(value, dict):
                for item in value.values():
                    redact(item)
            elif isinstance(value, (list, tuple)):
                for item in value:
                    redact(item)
            elif isinstance(value, str) and len(value) >= 3:
                message = message.replace(value, "***")
        for value in sensitive:
            redact(value)
        message = re.sub(r"(https?://)[^\s/@]+:[^\s/@]+@", r"\1***@", message)
        known = []
        def collect(value):
            if isinstance(value, dict):
                for item in value.values(): collect(item)
            elif isinstance(value, (list, tuple)):
                for item in value: collect(item)
            elif isinstance(value, str): known.append(value)
        for value in sensitive: collect(value)
        diagnostics = redact_data(records + [{"exception": traceback.format_exc()}], known) if request.get("method") == "events" else []
        send("error", message=message, diagnostics=diagnostics)
        return 1
    finally:
        if provider is not None:
            with contextlib.redirect_stdout(sys.stderr):
                provider.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, KeyError, OSError):
        send("error", message="Provider 请求无效或运行环境不可用")
        raise SystemExit(1)
