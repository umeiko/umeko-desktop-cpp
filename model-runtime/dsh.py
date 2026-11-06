"""Qt-free official DSH operations in an isolated cancellable helper process."""
import json,os,sys,threading
from pathlib import Path
import dsh_runtime as runtime
from dsh_profile import profile_configuration


def emit(value):
    print(json.dumps(value,ensure_ascii=False),flush=True)


def main():
    request=json.loads(sys.stdin.readline(1024*1024))
    runtime._NATIVE_NODE=request['node']
    runtime._NATIVE_NPM=request.get('npm','')
    root=Path(request['root'])
    stop=threading.Event()
    def cancel():
        for line in sys.stdin:
            try:
                if json.loads(line).get('type')=='cancel':stop.set()
            except ValueError:pass
    threading.Thread(target=cancel,daemon=True).start()
    action=request['operation']
    try:
        options=dict(bridge=root/'runtime/dsh-acp-bridge.mjs',cancelled=stop.is_set)
        if action=='download':
            result=runtime.install_dsh(request['runtimeRoot'],progress=lambda value:emit({'type':'progress',**value}),**options)
        elif action=='check':result=runtime.check_dsh(request.get('path',''),request['runtimeRoot'],**options)
        elif action=='prepare':
            found=runtime.discover_dsh(request.get('path',''),request['runtimeRoot'])
            own=request.get('own') is True
            profile=Path(request['profile'])
            home=Path(request['home'])
            sessions=home/'sessions'
            sessions.mkdir(parents=True,exist_ok=True)
            patch,_=profile_configuration(request['providers'],request['provider'],request['model'],request['maxTokens'],
                request['permissionMode'],root/'runtime/dsh-permissions.mjs',own_config=own,session_root=sessions,
                proxy_url=request['proxyUrl'],proxy_token='provided-only-by-native-child-environment' if not own else '')
            overlay=home/'umeko.patch.json' if own else home/'profiles/acp/cordis.patch.yml'
            overlay.parent.mkdir(parents=True,exist_ok=True)
            overlay.write_text(json.dumps(patch,ensure_ascii=False),encoding='utf-8')
            try:os.chmod(overlay,0o600)
            except OSError:pass
            workspace=Path(request['workspace']);workspace.mkdir(parents=True,exist_ok=True)
            result={'ok':True,'path':found['path'],'overlay':str(overlay),'home':str(home),'sessionRoot':str(sessions)}
        else:raise ValueError('Unsupported DSH operation')
        emit({'type':'result',**result})
    except InterruptedError:emit({'type':'result','ok':False,'code':'cancelled','message':'已取消','hint':'原有安装已保留。'})
    except runtime.RuntimeFailure as error:emit({'type':'result','ok':False,'code':error.code,'message':str(error),'hint':error.hint})
    except Exception:emit({'type':'result','ok':False,'message':'dsh 操作失败','hint':'检查 Node、安装路径和网络后重试；原有安装已保留。'})

if __name__=='__main__':main()
