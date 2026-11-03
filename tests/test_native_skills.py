"""Exercise the native Skill helper protocol with isolated local fixtures."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]
class NativeSkills(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.base = Path(self.temporary.name)
        self.agent = self.base / 'agent'; self.project = self.base / 'project'
        self.agent.mkdir(); self.project.mkdir()
        self.context = dict(node=shutil.which('node'), release=str(ROOT/'runtime/releases/coding-1.3.0'),
            cwd=str(self.project), agentDir=str(self.agent), includeProject=True, agent={}, skillsEnabled=True)
        self.worker = subprocess.Popen([os.sys.executable,'-B',str(ROOT/'skill-runtime/worker.py')],
            stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,encoding='utf-8')
    def tearDown(self):
        self.worker.stdin.close(); self.worker.wait(timeout=5)
        self.worker.stdout.close(); self.worker.stderr.close(); self.temporary.cleanup()
    def call(self, operation, **arguments):
        identifier = uuid.uuid4().hex
        self.worker.stdin.write(json.dumps(dict(id=identifier,operation=operation,arguments=arguments,context=self.context))+'\n');self.worker.stdin.flush()
        value = json.loads(self.worker.stdout.readline())
        self.assertEqual(value['id'], identifier)
        return value['result']
    def skill(self, root, name='fixture', body='# Local fixture'):
        path = root/name;path.mkdir(parents=True)
        (path/'SKILL.md').write_text(f'---\nname: {name}\ndescription: Offline fixture.\n---\n{body}',encoding='utf-8')
        return path
    def test_browse_preview_attach_and_pinned_folder_actions(self):
        folder = self.skill(self.agent/'skills')
        self.skill(self.project/'.pi/skills','project-fixture')
        rows = self.call('skills.browse')['skills'];self.assertEqual(len(rows),2)
        row = next(row for row in rows if row['scope']=='global')
        self.assertTrue(row['enabled']);self.assertTrue(row['canToggle'])
        self.assertIn('Local fixture',self.call('skills.preview',id=row['id'])['details']['markdown'])
        self.assertFalse(self.call('skills.attach',id=row['id'],enabled=False)['enabled'])
        self.context['sessionSkills']={row['path']:False}
        self.assertFalse(next(r for r in self.call('skills.browse')['skills'] if r['id']==row['id'])['enabled'])
        self.assertTrue(self.call('skills.rename',id=row['id'],name='renamed')['ok'])
        self.assertFalse(folder.exists());self.assertTrue((folder.parent/'renamed/SKILL.md').exists())
        self.assertFalse(self.call('skills.delete',id=row['id'])['ok'])
        current = next(r for r in self.call('skills.browse')['skills'] if r['name']=='renamed')
        self.assertTrue(self.call('skills.delete',id=current['id'])['ok'])
    def test_import_is_explicit_non_destructive_and_respects_scope(self):
        source = self.skill(self.base/'source')
        self.call('skills.browse')
        result = self.call('skills.importLocal',source=str(source),project=True)
        self.assertTrue(result['ok'],result)
        target = self.project/'.pi/skills/fixture/SKILL.md';self.assertTrue(target.exists())
        self.assertFalse((self.agent/'skills/fixture').exists())
        original = target.read_bytes();(source/'SKILL.md').write_text('changed')
        self.call('skills.importLocal',source=str(source),project=True)
        self.assertEqual(target.read_bytes(),original)
    def test_rejects_zip_traversal_and_stale_folder_replacements(self):
        archive=self.base/'bad.zip'
        with zipfile.ZipFile(archive,'w') as z:z.writestr('../escaped/SKILL.md','malicious')
        result=self.call('skills.importLocal',source=str(archive),project=False)
        self.assertFalse(result['ok']);self.assertFalse((self.base/'escaped').exists())
        folder=self.skill(self.agent/'skills')
        row=self.call('skills.browse')['skills'][0]
        folder.rename(folder.with_name('retired'));self.skill(self.agent/'skills')
        self.assertFalse(self.call('skills.delete',id=row['id'])['ok'])
        self.assertTrue((folder/'SKILL.md').is_file())
    def test_widget_gate_disables_discovery_and_toggles(self):
        self.skill(self.agent/'skills');self.context['skillsEnabled']=False
        rows=self.call('skills.browse')['skills'];self.assertFalse(rows[0]['canToggle'])
        self.assertFalse(self.call('skills.attach',id=rows[0]['id'],enabled=True)['ok'])
        self.assertFalse(any(row['attached'] for row in self.call('skills.list')['skills']))
if __name__=='__main__':unittest.main()
