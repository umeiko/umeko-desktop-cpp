"""Provider process contracts, independent of Qt and real enterprise services."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]

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

if __name__=='__main__':unittest.main()
