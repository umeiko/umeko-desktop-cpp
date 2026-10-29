"""Provider process contracts, independent of Qt and real enterprise services."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))

from umeko_provider_sdk import ProviderError
from umeko_provider_sdk.packages import validate_folder

class WorkerTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory()
        self.folder=Path(self.temp.name)/'provider'
        shutil.copytree(ROOT/'provider_dev/examples/echo-provider',self.folder)
        self.env=dict(os.environ,PYTHONUTF8='1',PYTHONDONTWRITEBYTECODE='1')
    def tearDown(self):self.temp.cleanup()
    def worker(self,method,params=None,secrets=None):
        request={'package':str(self.folder),'state_dir':str(Path(self.temp.name)/'state'),
                 'method':method,'params':params or {},'secrets':secrets or {}}
        result=subprocess.run([sys.executable,str(ROOT/'provider-runtime/runner.py')],
                              input=json.dumps(request),text=True,capture_output=True,env=self.env,timeout=15)
        events=[json.loads(line) for line in result.stdout.splitlines()]
        return result,events
    def implementation(self,body):
        (self.folder/'scripts/provider.py').write_text(body,encoding='utf-8')
    def test_optional_description_metadata(self):
        path=self.folder/'manifest.json'
        manifest=json.loads(path.read_text(encoding='utf-8'))
        manifest.pop('description',None)
        path.write_text(json.dumps(manifest),encoding='utf-8')
        self.assertEqual(validate_folder(self.folder)[0]['description'],'')
        for value in ('', ' \n ', '  插件说明\nUse <b>plain text</b>.  '):
            manifest['description']=value
            path.write_text(json.dumps(manifest),encoding='utf-8')
            self.assertEqual(validate_folder(self.folder)[0]['description'],value.strip())
        for value in (None,42,True,[],{}):
            with self.subTest(value=value):
                manifest['description']=value
                path.write_text(json.dumps(manifest),encoding='utf-8')
                with self.assertRaisesRegex(ProviderError,'description must be a string'):
                    validate_folder(self.folder)
    def test_login_and_fresh_process_inference(self):
        result,events=self.worker('login',{'credentials':{'username':'synthetic-user'}})
        self.assertEqual(result.returncode,0,result.stderr)
        secrets=[e['data'] for e in events if e['type']=='secrets'][-1]
        self.assertEqual(events[-1]['data']['models'][0]['id'],'local-echo')
        result,events=self.worker('events',{'model':'local-echo','messages':[{'role':'user','content':'fixture'}]},secrets)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn('Echo: fixture',json.dumps(events))
    def test_vendor_precedes_external_environment(self):
        external=Path(self.temp.name)/'external';external.mkdir()
        (external/'umeko_fixture_dep.py').write_text("VALUE='external'",encoding='utf-8')
        vendor=self.folder/'vendor';vendor.mkdir()
        (vendor/'umeko_fixture_dep.py').write_text("VALUE='bundled'",encoding='utf-8')
        self.env['PYTHONPATH']=str(external)
        self.implementation("from umeko_provider_sdk import Provider\nimport umeko_fixture_dep\nclass Custom(Provider):\n def list_models(self,refresh=False): return [{'id':umeko_fixture_dep.VALUE}]\ndef create_provider(context): return Custom(context)\n")
        result,events=self.worker('models')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(events[-1]['data']['models'][0]['id'],'bundled')

    def test_login_progress_redacts_credentials_before_truncation(self):
        self.implementation("from umeko_provider_sdk import Provider\nclass Custom(Provider):\n def authenticate(self,credentials):\n  self.context.report_status('Confirm '+credentials['password']+' '+self.context.secrets.get('token'))\n  return {'label':'Signed in'}\n def list_models(self,refresh=False): return []\ndef create_provider(context): return Custom(context)\n")
        password='synthetic-long-password-'+'x'*400
        result,events=self.worker('login',{'credentials':{'password':password}}, {'token':'synthetic-secret-token'})
        self.assertEqual(result.returncode,0,result.stderr)
        statuses=[event['message'] for event in events if event['type']=='auth_status']
        self.assertEqual(len(statuses),1)
        self.assertIn('Confirm',statuses[0])
        self.assertNotIn('synthetic-long-password',statuses[0])
        self.assertNotIn('synthetic-secret-token',statuses[0])
        self.assertEqual(events[-1]['type'],'result')

    def test_auth_progress_does_not_escape_from_model_discovery(self):
        self.implementation("from umeko_provider_sdk import Provider\nclass Custom(Provider):\n def list_models(self,refresh=False):\n  self.context.report_status('Not a login operation')\n  return []\ndef create_provider(context): return Custom(context)\n")
        result,events=self.worker('models')
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertFalse(any(event['type']=='auth_status' for event in events))
    def test_safe_errors_and_debug_stdout(self):
        self.implementation("from umeko_provider_sdk import Provider,ProviderError\nclass Custom(Provider):\n def list_models(self,refresh=False):\n  print('provider diagnostic')\n  raise ProviderError('failed with '+self.context.secrets.get('token'))\ndef create_provider(context): return Custom(context)\n")
        result,events=self.worker('models',secrets={'token':'synthetic-secret-token'})
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(events[-1]['type'],'error')
        self.assertNotIn('synthetic-secret-token',result.stdout)
        self.assertIn('provider diagnostic',result.stderr)
    def test_missing_dependency_is_actionable_without_traceback(self):
        self.implementation('import umeko_nonexistent_fixture_dependency\n')
        result,events=self.worker('models')
        self.assertNotEqual(result.returncode,0)
        self.assertIn('Python',events[-1]['message'])
        self.assertNotIn('Traceback',result.stdout)

    def test_inference_http_error_retains_body_and_hides_credentials(self):
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*_):pass
            def do_POST(self):
                self.rfile.read(int(self.headers['Content-Length']))
                body=b'upstream-details synthetic-llm-token'
                self.send_response(429);self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
        server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
        threading.Thread(target=server.serve_forever,daemon=True).start()
        try:
            url=f'http://127.0.0.1:{server.server_port}/completion'
            self.implementation('from umeko_provider_sdk import Provider,HttpSession\nclass Custom(Provider):\n def events(self,model,messages,tools,max_tokens,parameters):\n  http=HttpSession(['+repr(url)+'],environment_proxy=False)\n  with http.request('+repr(url)+',method="POST",headers={"Authorization":"Bearer "+self.context.secrets.get("token")},data={"model":model,"messages":messages}) as response:\n   yield {"text":response.read().decode()}\ndef create_provider(context): return Custom(context)\n')
            result,events=self.worker('events',{'model':'fixture','messages':[{'role':'user','content':'full prompt'}]}, {'token':'synthetic-llm-token'})
            self.assertNotEqual(result.returncode,0)
            report=events[-1]['diagnostics'][0]
            self.assertEqual(report['response']['status'],429)
            self.assertEqual(report['response']['body'],'upstream-details [REDACTED]')
            self.assertNotIn('synthetic-llm-token',result.stdout)
            self.assertEqual(json.loads(report['request']['body'])['messages'][0]['content'],'full prompt')
        finally:
            server.shutdown();server.server_close()

if __name__=='__main__':unittest.main()
