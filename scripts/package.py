"""Build a native portable distribution; no Python interpreter is shipped."""
from __future__ import annotations
import argparse, hashlib, json, os, platform, shutil, subprocess, sys, tarfile, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def run(*args, **kw):
    subprocess.run([str(a) for a in args], check=True, cwd=ROOT, **kw)

def copy(source, target):
    if source.is_dir():
        shutil.copytree(source, target, dirs_exist_ok=True, symlinks=True,
                        ignore=shutil.ignore_patterns('__pycache__', '*.pyc', '*.map', '.DS_Store'))
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)

def main():
    p=argparse.ArgumentParser();p.add_argument('--cmake',default='cmake');p.add_argument('--build',default='build');p.add_argument('--platform',default='');args=p.parse_args()
    arch={'AMD64':'x64','x86_64':'x64','arm64':'arm64','aarch64':'arm64'}[platform.machine()]
    target=args.platform or {'win32':'windows','darwin':'macos','linux':'linux'}[sys.platform]+'-'+arch
    prefix=ROOT/'dist'/target
    prefix.mkdir(parents=True,exist_ok=True)
    run(args.cmake,'--install',args.build,'--config','Release','--prefix',prefix)
    resources=prefix/'UmekoCpp.app/Contents/Resources' if sys.platform=='darwin' else prefix
    resources.mkdir(parents=True,exist_ok=True)
    for relative in ['runtime/gateway.mjs','runtime/releases/coding-1.3.0','scripts/prepare-runtime.mjs','scripts/search-tools.json','provider-runtime']:
        copy(ROOT/relative,resources/relative)
    copy(ROOT/'build/provider-resources',resources/'provider-resources')
    for name in ['THIRD_PARTY_NOTICES.md','README.md','licenses']:
        copy(ROOT/name,resources/name)
    qt_root = os.environ.get('QT_ROOT')
    if qt_root and (Path(qt_root)/'sbom').is_dir():
        copy(Path(qt_root)/'sbom',resources/'licenses/Qt-SBOM')
    # Every bundled module keeps its upstream license. Only source maps are omitted.
    info={'version':'0.2.0','platform':target,'commit':os.environ.get('GITHUB_SHA','local'),
          'system':platform.system(),'architecture':platform.machine(),
          'builtAt':__import__('datetime').datetime.now(__import__('datetime').timezone.utc).isoformat(),
          'pi':'1.3.0','qt':'6.10.2','providerApi':1,'python':'external, >=3.10',
          'node':'external, >=22.19','parityBaseline':'f648e18b0c8ef86c65363dc78d99223ef0276fcf'}
    (resources/'build-info.json').write_text(json.dumps(info,indent=2)+'\n',encoding='utf-8')
    executable=prefix/'UmekoCpp.app/Contents/MacOS/UmekoCpp' if sys.platform=='darwin' else prefix/('UmekoCpp.exe' if os.name=='nt' else 'UmekoCpp')
    env=dict(os.environ,QT_QPA_PLATFORM='offscreen',QT_QUICK_BACKEND='software',UMEKO_DIAGNOSTICS=str(ROOT/'build/packaged-smoke.log'))
    log=ROOT/'build/packaged-smoke.log';log.unlink(missing_ok=True)
    result=subprocess.run([str(executable),'--data-dir',str(ROOT/'build/packaged-profile'),'--smoke','--demo','--snapshot',str(ROOT/'build/packaged-chat.png')],env=env,timeout=45,capture_output=True)
    if result.returncode or not log.exists() or 'QML_READY' not in log.read_text(encoding='utf-8') or 'EXIT:0' not in log.read_text(encoding='utf-8'):
        raise RuntimeError('Packaged application failed: '+result.stderr.decode('utf-8','replace')+'\n'+(log.read_text(encoding='utf-8') if log.exists() else 'no diagnostics'))
    diagnostics=log.read_text(encoding='utf-8')
    if any(text in diagnostics for text in ['qrc:/','SNAPSHOT_FAILED','QML_FAILED']):
        raise RuntimeError('Packaged QML warnings: '+diagnostics)
    # Probe bundled Pi and offline search binaries from their installed paths.
    run('node',resources/'runtime/releases/coding-1.3.0/bridge.mjs','--self-test')
    for binary in ['fd','rg']:
        run(resources/'runtime/releases/coding-1.3.0/bin'/(binary+('.exe' if os.name=='nt' else '')),'--version')
    output=ROOT/'release-assets';output.mkdir(exist_ok=True)
    stem='UmekoCpp-v0.2.0-'+target
    if sys.platform=='linux':
        archive=output/(stem+'.tar.gz')
        with tarfile.open(archive,'w:gz',compresslevel=9) as stream:stream.add(prefix,arcname='UmekoCpp')
    else:
        archive=output/(stem+'.zip')
        with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as stream:
            for file in sorted(prefix.rglob('*')):
                if file.is_symlink():
                    entry=zipfile.ZipInfo(str(Path('UmekoCpp')/file.relative_to(prefix)).replace('\\','/'));entry.create_system=3;entry.external_attr=0o120777<<16
                    stream.writestr(entry,os.readlink(file))
                elif file.is_file():stream.write(file,Path('UmekoCpp')/file.relative_to(prefix))
    digest=hashlib.sha256(archive.read_bytes()).hexdigest()
    archive.with_name(archive.name+'.sha256').write_text(digest+'  '+archive.name+'\n',encoding='ascii')
    (output/(stem+'.json')).write_text(json.dumps({**info,'bytes':archive.stat().st_size,'sha256':digest},indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'archive':str(archive),'bytes':archive.stat().st_size,'sha256':digest}))

if __name__=='__main__':main()
