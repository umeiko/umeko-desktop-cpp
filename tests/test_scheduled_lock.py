"""The bundled stdlib owns the scheduler lease, including after a crash."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPTS=Path(__file__).resolve().parents[1]/'umeko-plugins/scheduled-tasks/scripts'
spec=importlib.util.spec_from_file_location('scheduler_lock',SCRIPTS/'scheduler_lock.py')
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
SchedulerLock=module.SchedulerLock


class SchedulerLeaseTests(unittest.TestCase):
    def test_timer_worker_starts_without_gui_python_dependencies(self):
        script = '''import sys,types,importlib,tempfile
from pathlib import Path
sys.path.insert(0,sys.argv[2])
package=types.ModuleType("timer_without_qt");package.__path__=[sys.argv[1]];sys.modules[package.__name__]=package
from umeko_plugin_sdk import Context
Plugin=importlib.import_module(package.__name__+".plugin").ScheduledTasksPlugin
assert "PySide6" not in sys.modules
with tempfile.TemporaryDirectory() as folder:
    def context():return Context(Path(sys.argv[1]).parent,Path(folder),{},{},[],lifecycle="application")
    current=context()
    first=Plugin(current)
    try:
        try:Plugin(current)
        except Exception as error:assert "another instance" in str(error)
        else:raise AssertionError("A second worker acquired the same timer state")
    finally:first.close()
    restored=Plugin(context());restored.close()
'''
        result=subprocess.run([sys.executable,'-I','-S','-c',script,str(SCRIPTS),
            str(SCRIPTS.parents[2]/'plugin-runtime')],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_exclusive_lease_releases_before_profile_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'lease.sqlite3'
            first,second=SchedulerLock(path),SchedulerLock(path)
            self.assertTrue(first.acquire())
            try:self.assertFalse(second.acquire())
            finally:first.release()
            self.assertTrue(second.acquire())
            second.release();path.unlink()

    def test_actual_process_crash_releases_operating_system_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'lease.sqlite3'
            script="import sys,os;sys.path.insert(0,sys.argv[1]);from scheduler_lock import SchedulerLock;lock=SchedulerLock(sys.argv[2]);assert lock.acquire();print('ready',flush=True);sys.stdin.readline();os._exit(7)"
            with subprocess.Popen([sys.executable,'-B','-c',script,str(SCRIPTS),str(path)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True) as child:
                self.assertEqual(child.stdout.readline().strip(),'ready')
                lease=SchedulerLock(path)
                self.assertFalse(lease.acquire())
                child.stdin.write('crash\n');child.stdin.flush()
                self.assertEqual(child.wait(timeout=5),7)
                self.assertTrue(lease.acquire());lease.release()
