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
from threading import Event, Thread

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
    from umeko_provider_sdk import Provider, ProviderContext, ProviderError, request_scope
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
    request_cancelled = Event()
    if request.get("method") == "login":
        def watch_cancel():
            try:
                while True:
                    line = sys.stdin.readline()
                    if not line:
                        break
                    try:
                        command = json.loads(line)
                    except ValueError:
                        continue
                    if command.get("cancel"):
                        request_cancelled.set()
                        break
            except (OSError, ValueError):
                pass
        Thread(target=watch_cancel, daemon=True).start()
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
        with request_scope(request_cancelled), contextlib.redirect_stdout(sys.stderr), network_scope(request.get("proxy"), records if request.get("method") == "events" else None):
            implementation = importlib.import_module(namespace + "." + entry[:-3].replace("/", "."))
            def report_status(message):
                if request.get("method") == "login" and not cancelled.is_set() and not request_cancelled.is_set():
                    # Status is public UI text. Providers must never submit
                    # credentials, authentication URLs, or raw server bodies.
                    known = []
                    def collect(value):
                        if isinstance(value, dict):
                            for child in value.values(): collect(child)
                        elif isinstance(value, (list, tuple)):
                            for child in value: collect(child)
                        elif isinstance(value, str): known.append(value)
                    for value in sensitive: collect(value)
                    send("auth_status", message=redact_data(str(message), known)[:300])
            provider = getattr(implementation, factory)(ProviderContext(config, Path(request["state_dir"]), MemorySecrets(), cancelled,
                request_cancelled=request_cancelled, report_status=report_status))
            if not isinstance(provider, Provider):
                raise ProviderError("供应商实现必须继承 umeko_provider_sdk.Provider")
            method, params = request["method"], request.get("params") or {}
            if method == "login":
                result = None
                if not request_cancelled.is_set() and not cancelled.is_set():
                    credentials = params["credentials"]
                    details = provider.authenticate(credentials) or {}
                    if not request_cancelled.is_set() and not cancelled.is_set():
                        models = provider.list_models(refresh=True)
                        if not request_cancelled.is_set() and not cancelled.is_set():
                            provider.context.secrets.set("credentials", credentials)
                            result = {"models": models, "label": str(details.get("label", "已登录"))[:100]}
            elif method == "models":
                result = {"models": provider.list_models(refresh=True)}
            elif method == "generate":
                kind=params.get("kind")
                if kind not in manifest.get("generationCapabilities",[]):
                    raise ProviderError("The provider does not support this generation kind")
                function=getattr(provider,"generate_"+kind)
                arguments=(params["model"],params["text"],params.get("parameters",{}))
                values=function(*arguments,on_progress=lambda value:send("event",data={"type":"progress",**value})) if kind=="video" else function(*arguments)
                if not isinstance(values,list) or not 1<=len(values)<=10:
                    raise ProviderError("Invalid generated media list")
                output=Path(params["_transferDir"]).resolve();output.mkdir(parents=True,exist_ok=True,mode=0o700)
                total=0;items=[]
                for value in values:
                    mime=value.get("mimeType","")
                    if not isinstance(mime,str) or not mime.startswith("audio/" if kind=="speech" else kind+"/"):
                        raise ProviderError("Invalid generated media type")
                    data=value.get("data")
                    if not isinstance(data,bytes) or not 0<len(data)<=32*1024*1024:
                        raise ProviderError("Invalid or oversized generated media")
                    total+=len(data)
                    if total>48*1024*1024:raise ProviderError("Generated media exceeds limit")
                    file=output/("media-"+str(len(items))+".bin")
                    with file.open("xb") as stream:stream.write(data)
                    file.chmod(0o600)
                    items.append({"file":str(file),"mimeType":value.get("mimeType","")})
                result={"items":items}
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
        if result is None:
            send("cancelled", message="登录已取消")
        else:
            send("result", data=result)
        return 0
    except Exception as exc:
        message = str(exc)[:1000] if isinstance(exc, ProviderError) else "供应商运行失败，请检查 Python 依赖、插件配置与网络"
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
        if request_cancelled.is_set() or cancelled.is_set():
            send("cancelled", message=message)
        else:
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
