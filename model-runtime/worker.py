"""One cancellable generation request. Credentials travel only over stdin."""
from __future__ import annotations
import json
from pathlib import Path
import sys
import threading
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from umeko_provider_sdk import request_scope
from umeko_provider_sdk.errors import safe_text, format_http_error
from engine.generation import generate, GenerationCancelled


def emit(value):
    print(json.dumps(value, ensure_ascii=False), flush=True)


class Diagnostics:
    def failure(self, provider, request, response, error, records):
        # Successful base64/media bodies never enter diagnostics or stderr.
        status = response.get('status')
        message = (format_http_error(status, response.get('body', ''), response.get('headers', {}))
                   if str(status).isdigit() and int(status) >= 400 else str(error))
        secrets = [provider.get('key', '')] + [value for call in records for value in call.get('request', {}).get('headers', {}).values()]
        return ValueError(safe_text(message, secrets)[:2000])


def main():
    line = sys.stdin.readline(1024 * 1024)
    request = json.loads(line)
    stopped = threading.Event()
    def watch():
        for line in sys.stdin:
            if line.strip() == 'cancel':
                stopped.set()
                return
        stopped.set()
    threading.Thread(target=watch, daemon=True).start()
    gateway = SimpleNamespace(store=SimpleNamespace(data=request['settings']),
                              plugins=None, diagnostics=Diagnostics())
    try:
        with request_scope(stopped):
            result = generate(gateway, request['body'],
                              on_progress=lambda progress: emit({'type': 'progress', **progress}))
        emit({'type': 'result', 'result': result})
    except GenerationCancelled:
        emit({'type': 'cancelled'})
    except Exception as error:
        provider = request['settings'].get('providers', [{}])[0]
        emit({'type': 'error', 'error': {'message': safe_text(str(error), [provider.get('key', '')])[:2000]}})


if __name__ == '__main__':
    main()
