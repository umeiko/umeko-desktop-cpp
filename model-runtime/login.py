"""Per-user native login registration; isolated previews never invoke writes."""
import json
import sys
from engine.login_startup import LoginStartup
request=json.loads(sys.stdin.read(16385))
manager=LoginStartup(executable=request['executable'],frozen=True,value_name='UmekoCpp')
try:
    if request['operation']=='set':manager.set_enabled(request['umeko'],request['proxy'])
    print(json.dumps({'ok':True,'state':manager.state()}))
except Exception as error:print(json.dumps({'ok':False,'error':str(error)[:300]}))
