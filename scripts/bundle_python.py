"""Fetch a pinned, stripped standalone CPython, retaining its stdlib and licenses."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def install_python(target: str, destination: Path) -> dict:
    manifest = json.loads((ROOT / 'scripts/python-distributions.json').read_text(encoding='utf-8'))
    record = manifest['platforms'][target]
    cache = ROOT / '.tools/python-downloads'
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / record['name']

    def valid():
        if not archive.is_file() or archive.stat().st_size != record['bytes']:
            return False
        with archive.open('rb') as stream:
            return hashlib.file_digest(stream, 'sha256').hexdigest() == record['sha256']

    if not valid():
        url = manifest['project'] + '/releases/download/' + manifest['release'] + '/' + urllib.parse.quote(record['name'])
        print('Downloading pinned CPython', manifest['version'], target, flush=True)
        with tempfile.NamedTemporaryFile(dir=cache, delete=False) as stream:
            temporary = Path(stream.name)
            request = urllib.request.Request(url, headers={'User-Agent': 'Umeko-Desktop-build'})
            with urllib.request.urlopen(request, timeout=60) as response:
                shutil.copyfileobj(response, stream)
        os.replace(temporary, archive)
        if not valid():
            raise RuntimeError('Standalone Python size or SHA-256 mismatch')

    # Each packaging stage is fresh; never overwrite an earlier runtime.
    if destination.exists():
        raise RuntimeError('Python staging destination already exists')
    with tempfile.TemporaryDirectory(prefix='python-extract-', dir=cache) as staging:
        with tarfile.open(archive, 'r:gz') as package:
            # data_filter rejects escaping paths/links, devices and other special files.
            package.extractall(staging, filter='data')
        source = Path(staging) / 'python'
        executable = source / ('python.exe' if target.startswith('windows') else 'bin/python3')
        if not executable.is_file():
            raise RuntimeError('Standalone Python executable is missing')
        shutil.copytree(source, destination, symlinks=True)

    return {'version': manifest['version'], 'project': manifest['project'],
            'release': manifest['release'], 'archive': record['name'], 'sha256': record['sha256']}
