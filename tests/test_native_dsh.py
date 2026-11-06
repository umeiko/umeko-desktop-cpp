"""Official ACP helper fixtures. Never reads user DSH home or contacts a model."""
import json,os,sys,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'model-runtime'))
import dsh_runtime as runtime
from dsh_profile import profile_configuration

class DshNativeTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup);self.base=Path(self.tmp.name)
        runtime._NATIVE_NODE=str(ROOT/'runtime/node/bin/node') if os.name!='nt' else str(ROOT/'runtime/node/node.exe')
        if not Path(runtime._NATIVE_NODE).exists():
            import shutil
            runtime._NATIVE_NODE=shutil.which('node')
        self.lib=self.base/'official/lib';self.lib.mkdir(parents=True)
        self.entry=self.lib/'bin.js';self.entry.write_text((ROOT/'tests/fixtures/fake-acp.mjs').read_text())
        (self.lib.parent/'package.json').write_text(json.dumps({'type':'module','name':'@deepseek-ai/dsh','version':'0.2.2-rc.1'}))
    def test_explicit_bad_path_never_falls_back(self):
        with self.assertRaises(runtime.RuntimeFailure):runtime.discover_dsh(self.base/'missing',self.lib.parent)
    def test_official_fixture_health_uses_temporary_home(self):
        value=runtime.check_dsh(str(self.lib.parent),bridge=ROOT/'runtime/dsh-acp-bridge.mjs')
        self.assertTrue(value['ok']);self.assertEqual(value['version'],'0.2.2-rc.1')
        self.assertFalse((self.lib.parent/'profiles').exists())
    def test_cancel_before_spawn(self):
        with self.assertRaises(InterruptedError):runtime.check_dsh(str(self.entry),bridge=ROOT/'runtime/dsh-acp-bridge.mjs',cancelled=lambda:True)
    def test_reject_nonofficial_package(self):
        (self.lib.parent/'package.json').write_text('{"name":"imposter","version":"9.0.0"}')
        with self.assertRaises(runtime.RuntimeFailure) as e:runtime.check_dsh(str(self.entry),bridge=ROOT/'runtime/dsh-acp-bridge.mjs')
        self.assertEqual(e.exception.code,'package')
    def test_version_semver(self):
        self.assertLess(runtime._version('0.2.2-rc.1'),runtime._version('0.2.2'))
        self.assertLess(runtime._version('0.2.2-rc.2'),runtime._version('0.2.2-rc.10'))
    def test_environment_only_loopback_ephemeral_credentials(self):
        env={'HOME':'fixture','UPSTREAM_API_KEY':'secret','UPSTREAM_AUTH_TOKEN':'secret2','NAMED_PROVIDER':'credential','UMEKO_DSH_KEY_OLD':'old','UMEKO_DSH_PROXY_TOKEN':'stale'}
        value=runtime.child_environment(env,[{'key':'credential'}]);self.assertFalse(any(x in value.values() for x in ('secret','secret2','credential','stale','old')))
        self.assertIn('127.0.0.1',value['NO_PROXY'])
        own=runtime.child_environment(env,own_config=True);self.assertEqual(own['UPSTREAM_API_KEY'],'secret');self.assertNotIn('UMEKO_DSH_PROXY_TOKEN',own)
    def test_proxy_profile_has_no_upstream_endpoint_or_key(self):
        providers=[{'id':'configured','baseUrl':'https://private-upstream.example','key':'do-not-copy','models':[{'alias':'configured-model','upstream':'private-model','reasoningLevels':['low'],'reasoningLevelsAuto':False}]}]
        spec,env=profile_configuration(providers,'configured','configured-model',8192,'ask',ROOT/'runtime/dsh-permissions.mjs',proxy_url='http://127.0.0.1:123/v1',proxy_token='ephemeral',session_root=self.base/'sessions')
        value=json.dumps(spec);self.assertNotIn('do-not-copy',value);self.assertNotIn('private-upstream',value);self.assertNotIn('ephemeral',value)
        self.assertEqual(env,{'UMEKO_DSH_PROXY_TOKEN':'ephemeral'});self.assertIn('workspace-write',value);self.assertIn('umeko-tool-permissions',value)
    def test_own_config_does_not_inject_provider_or_model(self):
        spec,env=profile_configuration([],None,None,8192,'auto',ROOT/'runtime/dsh-permissions.mjs',own_config=True,session_root=self.base/'sessions')
        self.assertEqual(env,{});self.assertFalse(any(x.get('id') in ('acp','agent-default-model','llm-pi-ai','deepseek-account') for x in spec))

if __name__=='__main__':unittest.main()
