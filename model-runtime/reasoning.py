"""Bounded discovery through the authenticated native gateway, off the GUI."""
import json
import sys
import time
import urllib.error
import urllib.request
from engine.reasoning_detection import detect_model


def main():
    task=json.loads(sys.stdin.read(16385))
    deadline=time.monotonic()+180
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    def request(effort,format,cap):
        if time.monotonic()>deadline:raise TimeoutError('Reasoning detection timed out')
        payload={'model':task['alias'],'_umeko_provider':task['provider'],
            '_umeko_reasoning_effort':effort,'_umeko_reasoning_format':format,
            '_umeko_reasoning_probe':True,'stream':False,'max_tokens':cap,
            'messages':[{'role':'user','content':'Reply with OK.'}]}
        query=urllib.request.Request(task['gateway']+'v1/chat/completions',data=json.dumps(payload).encode(),
            headers={'Authorization':'Bearer '+task['token'],'Content-Type':'application/json'})
        try:
            with opener.open(query,timeout=min(20,max(1,deadline-time.monotonic()))) as response:
                body=json.loads(response.read(1024*1024))
        except urllib.error.HTTPError as error:
            body=json.loads(error.read(8192));failure=ValueError(body.get('error',{}).get('message','Reasoning probe failed'))
            failure.status_code=error.code
            raise failure from None
        message=body.get('choices',[{}])[0].get('message',{})
        return {'thinkingChars':len(message.get('reasoning_content') or ''),'contentChars':len(message.get('content') or '')}
    row=task['model']
    result=detect_model(request,manual_levels=row.get('reasoningLevels') if row.get('reasoningLevelsAuto') is False else None,
        manual_format=row.get('reasoningFormat') if row.get('reasoningFormatAuto') is False else None,
        protocol=task.get('protocol','openai'))
    print(json.dumps({'ok':True,'result':result}))


if __name__=='__main__':
    try:main()
    except Exception as error:print(json.dumps({'ok':False,'error':str(error)[:500]}))
