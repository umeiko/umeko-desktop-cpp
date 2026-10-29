"""Capture the native app in English and Chinese with an isolated demo profile."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--executable', type=Path, required=True)
parser.add_argument('--theme-file', type=Path, required=True)
args = parser.parse_args()
theme = json.loads(args.theme_file.read_text(encoding='utf-8-sig'))
assert theme['format'] == 'umeko-theme' and theme['version'] == 1
output = ROOT / 'docs/screenshots'
output.mkdir(exist_ok=True)
for language in ('en', 'zh'):
    with tempfile.TemporaryDirectory(prefix='umeko-native-readme-') as folder:
        profile = Path(folder)
        settings = {'themes': [theme], 'theme': 'custom:' + theme['id'],
                    'ui': {'language': language, 'fontScale': 1.1},
                    'agent': {'agentDir': str(profile / 'agent'), 'safeMode': True}}
        (profile / 'settings.json').write_text(json.dumps(settings, ensure_ascii=False), encoding='utf-8')
        image = output / ('chat-white-' + language + '.png')
        env = {**os.environ, 'UMEKO_LANGUAGE': language, 'QT_QPA_PLATFORM': 'offscreen',
               'QT_QUICK_BACKEND': 'software', 'UMEKO_DIAGNOSTICS': str(profile / 'render.log')}
        subprocess.run([str(args.executable.resolve()), '--resource-root', str(ROOT),
                        '--data-dir', folder, '--demo', '--smoke', '--snapshot', str(image)],
                       cwd=ROOT, env=env, check=True, timeout=60)
        assert image.read_bytes().startswith(b'\x89PNG\r\n\x1a\n')
        print('Captured native', language, 'screenshot:', image.name)
