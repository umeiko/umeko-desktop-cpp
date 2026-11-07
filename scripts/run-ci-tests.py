"""Shared regressions run once; native jobs also cover OS-specific paths and PTYs."""
import argparse
from pathlib import Path
import subprocess
import sys

SHARED = [['node', '--test', '--test-concurrency=2', 'tests/background-jobs.test.mjs',
  'tests/tool-permissions.test.mjs', 'tests/tool-permissions-integration.test.mjs',
  'tests/compaction-bridge.test.mjs', 'tests/recent-images-bridge.test.mjs',
  'tests/provider-errors.test.mjs'],
 ['node', '--test', '--test-concurrency=2', 'tests/session-capabilities.test.mjs',
  'tests/session-capabilities-integration.test.mjs', 'tests/skills.test.mjs',
  'tests/recent-images.test.mjs', 'tests/markdown-preview.test.mjs',
  'tests/native-skill-policy.test.mjs', 'tests/prepare-runtime.test.mjs'],
 ['node', '--test', '--test-concurrency=2', 'tests/tool-visibility.test.mjs',
  'tests/package-install.test.mjs', 'tests/pi-widget-command.test.mjs',
  'tests/extension-metadata.test.mjs', 'tests/update-engine.test.mjs',
  'tests/update-download.test.mjs', 'tests/update-network.test.mjs', 'tests/webview.test.mjs',
  'tests/pi-commands.test.mjs', 'tests/pi-commands-integration.test.mjs',
  'tests/umeko-control.test.mjs', 'tests/global-agents.test.mjs', 'tests/umeko-documents.test.mjs',
  'tests/native-history.test.mjs', 'tests/widget-messages.test.mjs'],
 ['node', '--test', '--test-concurrency=2', 'tests/conversation-search.test.mjs',
  'tests/gateway.test.mjs', 'tests/model-tls.test.mjs', 'tests/openai-responses.test.mjs',
  'tests/network_retry_test.mjs'],
 ['node', '--test', '--test-concurrency=2', 'tests/dsh-acp.test.mjs',
  'tests/dsh-permissions.test.mjs', 'tests/approval-view.test.mjs',
  'tests/system-prompt.test.mjs'],
 ['node', '--test', '--test-concurrency=2', 'tests/auxiliary-vision.test.mjs',
  'tests/auxiliary-vision-bridge.test.mjs', 'tests/generation.test.mjs',
  'tests/native-generation.test.mjs', 'tests/widget-market.test.mjs'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_native_*.py'],
 ['node', '--test', '--test-concurrency=2', 'tests/subagents.test.mjs',
  'tests/subagent-ui.test.mjs', 'tests/subagent-queue.test.mjs', 'tests/rpc-ui.test.mjs'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_release_retention.py'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_release_replacement.py'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_release_downloads.py'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_release_policy.py'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_webview_package.py'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_native_skills.py'],
 ['python', 'tests/test_skill_import.py'], ['python', 'tests/test_bundled_skills.py'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_checkpoints.py'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_provider_errors.py'],
 ['python', 'tests/worker_test.py'], ['python', 'tests/desktop_plugins_test.py'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_change_review.py'],
 ['python', 'tests/test_terminal_widget.py']]
PLATFORM = [['node', '--test', '--test-concurrency=2', 'tests/background-jobs.test.mjs',
  'tests/tool-permissions.test.mjs', 'tests/tool-permissions-integration.test.mjs',
  'tests/conversation-search.test.mjs', 'tests/dsh-acp.test.mjs', 'tests/global-agents.test.mjs',
  'tests/subagent-queue.test.mjs', 'tests/system-prompt.test.mjs', 'tests/update-download.test.mjs',
  'tests/update-engine.test.mjs', 'tests/webview.test.mjs', 'tests/package-install.test.mjs',
  'tests/extension-metadata.test.mjs'],
 ['python', 'tests/test_terminal_widget.py'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'worker_test.py'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_change_review.py'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_skill_import.py'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_native_skills.py'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_native_dsh.py'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_native_login_startup.py'],
 ['python', '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_webview_package.py']]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--scope',choices=['shared','platform'],required=True)
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[1]
    commands=list(SHARED if args.scope=='shared' else PLATFORM)
    if args.scope=='shared':commands.append(['python','-m','unittest','discover','-s','tests','-p','test_ci_plan.py'])
    for command in commands:
        command=list(command)
        if command[0]=='python':command[0]=sys.executable
        print('Running:',*command,flush=True)
        subprocess.run(command,cwd=root,check=True)

if __name__=='__main__':main()
