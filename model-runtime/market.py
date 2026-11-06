"""Inert catalog/download work; installation and confirmation stay in Qt."""
import json
import os
from pathlib import Path
import subprocess
import sys
from engine.widget_market import MarketCatalog


def main():
    request=json.loads(sys.stdin.read(16385))
    root=Path(request['root']).resolve()
    def fetch(url, headers=None, destination=None):
        job={'url':url,'headers':headers or {}}
        if destination is not None: job['destination']=str(destination)
        environment=dict(os.environ)
        if request.get('proxy'):environment['UMEKO_UPDATE_PROXY']=request['proxy']
        value=subprocess.run([request['node'],str(root/'runtime/widget-market.mjs')],
            input=json.dumps(job),capture_output=True,text=True,timeout=180,env=environment,
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        result=json.loads(value.stdout)
        if value.returncode or result.get('error'):raise ValueError(result.get('error','Market request failed'))
        return result
    catalog=MarketCatalog(request['profile'],root=root,fetcher=fetch)
    operation=request['operation']
    if operation=='market.list':result=catalog.refresh(force=request['arguments'].get('force',False))
    elif operation=='market.prepare':result=catalog.prepare(request['arguments']['id'])
    else:raise ValueError('Unsupported market operation')
    print(json.dumps({'ok':True,'result':result},ensure_ascii=False))


if __name__=='__main__':
    try:main()
    except Exception as error:print(json.dumps({'ok':False,'error':str(error)[:2000]}))
