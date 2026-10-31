"""Portable, offline SDK and worker tests; no PySide or desktop profile required."""
import importlib.util,json,os,subprocess,sys,tempfile,unittest,zipfile,shutil,threading
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'plugin-runtime'))
from umeko_plugin_sdk import Context,PluginError
from umeko_plugin_sdk.packages import validate,install

class DesktopPluginSdkTests(unittest.TestCase):
    def test_documents_summary_and_public_schema_are_required(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name)/'widget';shutil.copytree(ROOT/'umeko-plugins/file-tree',folder)
            file=folder/'manifest.json';original=json.loads(file.read_text(encoding='utf-8'))
            for patch in ({'summary':''},{'summary':'x'*21},{'agent':{}},{'agent':{'document':'../outside.md'}},
                    {'agent':{'document':'AGENT.md','methods':{'bad':{'description':'bad','parameters':{'type':'object','pattern':'unsupported'}}}}}):
                with self.subTest(patch=patch):
                    file.write_text(json.dumps({**original,**patch}),encoding='utf-8')
                    with self.assertRaises((PluginError,ValueError)):validate(folder)
            file.write_text(json.dumps(original),encoding='utf-8');(folder/'AGENT.md').write_text('',encoding='utf-8')
            with self.assertRaisesRegex(PluginError,'Markdown'):validate(folder)
    def test_application_notifications_require_captured_target_and_thread_local_request(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name);first={'token':'first','id':'first-id','workspace':name};second={'token':'second','id':'second-id','workspace':name}
            events=[];context=Context(folder,folder,{},first,['workspace.read','session.write'],emit=events.append,lifecycle='application')
            captured=context.capture_session();context.session=second
            with self.assertRaises(PluginError):context.notify_agent('implicit target')
            with self.assertRaises(PluginError):context.resolve_workspace('.')
            context.request_id='parent-request';context.request_session=first;seen=[]
            def send():
                seen.append(context.request_id);context.notify_agent('background result',session=captured)
                seen.append(str(context.resolve_workspace('.',session=captured)))
            thread=threading.Thread(target=send);thread.start();thread.join()
            self.assertEqual(seen,['',str(Path(name).resolve())]);self.assertEqual(events[0]['arguments']['session'],captured);self.assertEqual(events[0]['arguments']['requestId'],'')
    def test_builtin_icon_catalog_and_custom_asset_validation(self):
        icons=json.loads((ROOT/'plugin-runtime/umeko_plugin_sdk/icons.json').read_text(encoding='utf-8'));self.assertEqual(len(icons),22)
        for icon in icons:self.assertTrue((ROOT/'app/icons'/(icon['glyph']+'.svg')).is_file())
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name)/'widget';shutil.copytree(ROOT/'umeko-plugins/file-tree',folder);file=folder/'manifest.json';manifest=json.loads(file.read_text(encoding='utf-8'))
            manifest['icon']='does-not-exist';file.write_text(json.dumps(manifest),encoding='utf-8')
            with self.assertRaises(PluginError):validate(folder)
            (folder/'assets/custom.svg').write_text('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 20 20"><path d="M2 2h16v16H2z"/></svg>',encoding='utf-8')
            manifest['icon']={'path':'assets/custom.svg'};file.write_text(json.dumps(manifest),encoding='utf-8');self.assertEqual(validate(folder)[0]['icon'],manifest['icon'])
            manifest['icon']={'path':'../outside.svg'};file.write_text(json.dumps(manifest),encoding='utf-8')
            with self.assertRaises(PluginError):validate(folder)
    def test_builtins_and_worker_are_portable_and_bounded(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name);(folder/'hello.py').write_text('print(1)')
            manifest,config,_=validate(ROOT/'umeko-plugins/file-tree')
            requests=[{'id':'init','operation':'initialize','arguments':{'path':str(ROOT/'umeko-plugins/file-tree'),'stateDir':str(folder/'state'),'config':config,'session':{'token':'fixture','id':'fixture','workspace':str(folder)}}},
                {'id':'list','operation':'call','arguments':{'method':'list','arguments':{'path':'.'}}},
                {'id':'outside','operation':'call','arguments':{'method':'list','arguments':{'path':'..'}}}]
            result=subprocess.run([sys.executable,'-B',str(ROOT/'plugin-runtime/worker.py')],input=''.join(json.dumps(row)+'\n' for row in requests),text=True,capture_output=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
            rows=[json.loads(line) for line in result.stdout.splitlines()]
            self.assertTrue(rows[0]['ok']);self.assertTrue(rows[1]['ok']);self.assertFalse(rows[2]['ok'])
            self.assertIn('hello.py',[row['name'] for row in rows[1]['result']['entries']])
    def test_zip_paths_cannot_escape_or_overwrite_builtins(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name)
            for bad in ('../outside.txt','/absolute','C:/outside','bad\\file'):
                archive=folder/'unsafe.zip'
                with zipfile.ZipFile(archive,'w') as stream:stream.writestr(bad,'fixture')
                with self.subTest(bad=bad),self.assertRaises(PluginError):install(archive,folder/'packages')
            archive=folder/'builtin.zip'
            with zipfile.ZipFile(archive,'w') as stream:
                for path in (ROOT/'umeko-plugins/file-tree').rglob('*'):
                    if path.is_file() and '__pycache__' not in path.parts:stream.write(path,path.relative_to(ROOT/'umeko-plugins/file-tree'))
            with self.assertRaises(PluginError):install(archive,folder/'packages',['file-tree'])
    def test_process_output_and_timeout_are_bounded(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name);context=Context(folder,folder,{}, {'workspace':name},['workspace.read','process.execute'],{'fixture':{'system':sys.executable}})
            value=context.run_executable('fixture',['-c','import sys;sys.stdout.write("x"*1000000)'],limit=4096)
            self.assertTrue(value['truncated']);self.assertLessEqual(len(value['text']),4096)
            with self.assertRaises(PluginError):context.run_executable('fixture',['-c','import time;time.sleep(30)'],timeout=.1)
            self.assertFalse(context._processes);context.close()
    def test_git_demo_uses_disposable_repository(self):
        with tempfile.TemporaryDirectory() as name:
            folder=Path(name);subprocess.run(['git','init','-q',name],check=True)
            spec=importlib.util.spec_from_file_location('desktop_git_demo',ROOT/'umeko-plugins/git-viewer/scripts/plugin.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
            manifest,config,_=validate(ROOT/'umeko-plugins/git-viewer');context=Context(ROOT/'umeko-plugins/git-viewer',folder,config,{'workspace':name},manifest['permissions'],manifest['executables']);plugin=module.create_plugin(context)
            (folder/'hello.py').write_text('print(1)\n');self.assertEqual(plugin.handle('status',{})['entries'][0]['group'],'untracked')
            plugin.handle('stage',{'path':'hello.py'});self.assertEqual(plugin.handle('status',{})['entries'][0]['group'],'staged')
            plugin.handle('unstage',{'path':'hello.py'});self.assertTrue((folder/'hello.py').exists())
            context.close()
if __name__=='__main__':unittest.main()
