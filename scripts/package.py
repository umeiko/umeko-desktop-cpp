"""Build a portable distribution with an optional standalone Python runtime."""
from __future__ import annotations
import argparse, hashlib, json, os, platform, re, shutil, subprocess, sys, tarfile, tempfile, zipfile
from pathlib import Path
from bundle_python import install_python
from bundle_node import install_node, node_binary
from macos_dmg import create_dmg

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

def finalize_macos(bundle):
    """Official Qt frameworks are universal; each release needs only its own CPU."""
    architecture='arm64' if platform.machine()=='arm64' else 'x86_64'
    magic={b'\xca\xfe\xba\xbe',b'\xbe\xba\xfe\xca',b'\xca\xfe\xba\xbf',b'\xbf\xba\xfe\xca'}
    saved=0
    for file in bundle.rglob('*'):
        if file.is_symlink() or not file.is_file():continue
        if file.suffix!='.dylib' and 'Frameworks' not in file.parts:continue
        with file.open('rb') as stream:header=stream.read(4)
        if header not in magic:continue
        architectures=subprocess.check_output(['lipo','-archs',str(file)],text=True).split()
        if architecture not in architectures:raise RuntimeError('Wrong Qt architecture: '+str(file))
        if len(architectures)>1:
            before=file.stat();temporary=file.with_name(file.name+'.native-thin')
            run('lipo',file,'-thin',architecture,'-output',temporary)
            temporary.chmod(before.st_mode);os.replace(temporary,file)
            saved+=before.st_size-file.stat().st_size
    # Resources are complete before the final ad-hoc seal; no developer identity required.
    run('codesign','--force','--deep','--sign','-',bundle)
    run('codesign','--verify','--deep','--strict',bundle)
    print('Qt architecture thinning saved',saved,'uncompressed bytes')

def main():
    p=argparse.ArgumentParser();p.add_argument('--cmake',default='cmake');p.add_argument('--build',default='build');p.add_argument('--platform',default='')
    p.add_argument('--bundle-python',action=argparse.BooleanOptionalAction,default=None,help='Override UMEKO_BUNDLE_PYTHON from the configured CMake build')
    args=p.parse_args()
    arch={'AMD64':'x64','x86_64':'x64','arm64':'arm64','aarch64':'arm64'}[platform.machine()]
    target=args.platform or {'win32':'windows','darwin':'macos','linux':'linux'}[sys.platform]+'-'+arch
    build_config=json.loads((ROOT/args.build/'build-config.json').read_text(encoding='utf-8'))
    bundle_python=build_config['bundlePython'] if args.bundle_python is None else args.bundle_python
    build_config['bundlePython']=bundle_python
    qt_root=os.environ.get('QT_ROOT')
    if not qt_root:
        cache=(ROOT/args.build/'CMakeCache.txt').read_text(encoding='utf-8')
        found=re.search(r'^Qt6_DIR:PATH=(.+)$',cache,re.M)
        if found:qt_root=str(Path(found.group(1)).parents[2])
    # A fresh staging folder prevents stale DLLs or previous versions entering a release.
    # Qt deployment creates library symlinks; workspaces may live on exFAT.
    prefix=Path(tempfile.mkdtemp(prefix='umeko-package-'+target+'-'))
    run(args.cmake,'--install',args.build,'--config','Release','--component','Runtime','--prefix',prefix)
    if sys.platform=='linux':
        from webview_package import strip_linux_engine
        strip_linux_engine(prefix)
    resources=prefix/'UmekoCpp.app/Contents/Resources' if sys.platform=='darwin' else prefix
    resources.mkdir(parents=True,exist_ok=True)
    if os.name=='nt':
        vswhere=Path(os.environ.get('ProgramFiles(x86)','C:/Program Files (x86)'))/'Microsoft Visual Studio/Installer/vswhere.exe'
        installation=subprocess.check_output([str(vswhere),'-latest','-products','*','-property','installationPath'],text=True).strip()
        candidates=sorted((Path(installation)/'VC/Redist/MSVC').glob('*/x64/Microsoft.VC143.CRT'))
        if not candidates: raise RuntimeError('App-local MSVC redistributable was not found')
        for dll in candidates[-1].glob('*.dll'):copy(dll,prefix/dll.name)
    for relative in ['runtime/gateway.mjs','runtime/transport.mjs','runtime/openai-responses.mjs','runtime/bridge.mjs','runtime/subagents.mjs','runtime/subagent-sdk.mjs','runtime/subagent-settings.mjs','runtime/input-queue.mjs','runtime/rpc-ui.mjs','runtime/resources.mjs','runtime/provider-policy.mjs','runtime/chatbox.mjs','runtime/update-engine.mjs','runtime/network-retry.mjs','runtime/pi-commands.mjs','runtime/umeko-control.mjs','runtime/umeko-operations.json','runtime/umeko-docs','runtime/webview-component.mjs','runtime/webview-component.json','runtime/releases/coding-1.3.0','scripts/prepare-runtime.mjs','scripts/search-tools.json','provider-runtime']:
        copy(ROOT/relative,resources/relative)
    copy(ROOT/'build/provider-resources',resources/'provider-resources')
    for document in (ROOT/'runtime/umeko-docs').glob('*.md'):
        if (resources/'runtime/releases/coding-1.3.0/umeko-docs'/document.name).read_bytes() != document.read_bytes():
            raise RuntimeError('Packaged Umeko document differs: ' + document.name)
    python_distribution=install_python(target,resources/'runtime/python') if bundle_python else None
    node_distribution=install_node(target,resources/'runtime/node')
    for name in ['THIRD_PARTY_NOTICES.md','README.md','licenses']:
        copy(ROOT/name,resources/name)
    if qt_root and (Path(qt_root)/'sbom').is_dir():
        copy(Path(qt_root)/'sbom',resources/'licenses/Qt-SBOM')
    if sys.platform!='darwin':
        plugin='qoffscreen.dll' if os.name=='nt' else 'libqoffscreen.so'
        copy(Path(qt_root)/'plugins/platforms'/plugin,prefix/'plugins/platforms'/plugin)
    # Every bundled module keeps its upstream license. Only source maps are omitted.
    info={**build_config,'platform':target,'commit':os.environ.get('GITHUB_SHA','local'),
          'system':platform.system(),'architecture':platform.machine(),
          'builtAt':__import__('datetime').datetime.now(__import__('datetime').timezone.utc).isoformat(),
          'pi':'1.3.0','qt':'6.10.2','providerApi':1,
          'edition':'standard' if bundle_python else 'minimal',
          'python':'bundled, CPython '+python_distribution['version'] if bundle_python else 'external, >=3.10',
          'pythonDistribution':python_distribution,
          'node':'bundled, '+node_distribution['version'],'nodeDistribution':node_distribution,
          'parityBaseline':'f648e18b0c8ef86c65363dc78d99223ef0276fcf'}
    (resources/'build-info.json').write_text(json.dumps(info,indent=2)+'\n',encoding='utf-8')
    if sys.platform=='darwin':finalize_macos(prefix/'UmekoCpp.app')
    executable=prefix/'UmekoCpp.app/Contents/MacOS/UmekoCpp' if sys.platform=='darwin' else prefix/('UmekoCpp.exe' if os.name=='nt' else 'UmekoCpp')
    env=dict(os.environ,QT_QPA_PLATFORM='cocoa' if sys.platform=='darwin' else 'offscreen',QT_QUICK_BACKEND='software',UMEKO_DIAGNOSTICS=str(ROOT/'build/packaged-smoke.log'))
    for key in ['QT_PLUGIN_PATH','QML_IMPORT_PATH','QML2_IMPORT_PATH','LD_LIBRARY_PATH','DYLD_LIBRARY_PATH','DYLD_FRAMEWORK_PATH']:
        env.pop(key,None)
    if qt_root:
        sdk=str(Path(qt_root).resolve()).replace('\\','/').lower()
        env['PATH']=os.pathsep.join(x for x in env['PATH'].split(os.pathsep) if not x.replace('\\','/').lower().startswith(sdk))
    # A colleague's clean machine must not need Node/npm installed in PATH.
    env['PATH']=str(ROOT/'build/no-system-node')
    env.pop('NODE_OPTIONS',None);env.pop('NODE_PATH',None)
    log=ROOT/'build/packaged-smoke.log';log.unlink(missing_ok=True)
    result=subprocess.run([str(executable),'--data-dir',str(ROOT/'build/packaged-profile'),'--smoke','--demo','--snapshot',str(ROOT/'build/packaged-chat.png')],env=env,timeout=45,capture_output=True)
    if result.returncode or not log.exists() or 'QML_READY' not in log.read_text(encoding='utf-8') or 'EXIT:0' not in log.read_text(encoding='utf-8'):
        raise RuntimeError('Packaged application failed ('+str(result.returncode)+'): '+result.stderr.decode('utf-8','replace')+'\n'+(log.read_text(encoding='utf-8') if log.exists() else 'no diagnostics'))
    diagnostics=log.read_text(encoding='utf-8')
    if 'TRAY_ICON_READY:1' not in diagnostics:
        raise RuntimeError('Installed Yanami tray avatar could not be decoded')
    required = 'IME_PLUGIN_READY:1' if sys.platform == 'linux' else 'NATIVE_NOTIFICATIONS_READY:1'
    if required not in diagnostics:
        raise RuntimeError('Installed input method or native notification adapter could not be loaded: '+required)
    if 'WINDOW_TITLE:Umeko-Desktop' not in diagnostics or 'COMPILER:'+build_config['compiler'] not in diagnostics:
        raise RuntimeError('Installed title or compiler metadata does not match the build')
    node=node_binary(resources/'runtime/node',target)
    selected=next((line.removeprefix('NODE_EXECUTABLE:') for line in diagnostics.splitlines() if line.startswith('NODE_EXECUTABLE:')), '')
    if not selected or Path(selected).resolve()!=node.resolve():
        raise RuntimeError('Installed app did not select bundled Node without a system PATH')
    if any(text in diagnostics for text in ['qrc:/','SNAPSHOT_FAILED','QML_FAILED']):
        raise RuntimeError('Packaged QML warnings: '+diagnostics)
    # Probe bundled Pi and offline search binaries from their installed paths.
    run(node,resources/'runtime/releases/coding-1.3.0/bridge.mjs','--self-test',env=env)
    npm=resources/'runtime/node'/('node_modules/npm/bin/npm-cli.js' if os.name=='nt' else 'lib/node_modules/npm/bin/npm-cli.js')
    run(node,npm,'--version',env=env)
    for binary in ['fd','rg']:
        run(resources/'runtime/releases/coding-1.3.0/bin'/(binary+('.exe' if os.name=='nt' else '')),'--version')
    if bundle_python:
        python=resources/'runtime/python'/('python.exe' if os.name=='nt' else 'bin/python3')
        selected=next((line.removeprefix('PYTHON_EXECUTABLE:') for line in diagnostics.splitlines() if line.startswith('PYTHON_EXECUTABLE:')), '')
        if not selected or Path(selected).resolve()!=python.resolve():
            raise RuntimeError('Installed app did not automatically select bundled Python')
        # Isolated mode ignores PYTHONDONTWRITEBYTECODE; keep the sealed
        # macOS bundle unchanged while importing the bundled standard library.
        run(python,'-I','-B','-c','import ssl,sqlite3,ctypes,venv,ensurepip; print(ssl.OPENSSL_VERSION)')
        run(python,'-I','-B','-m','pip','--version')
        # Exercise real SDK login, vendor dependencies and inference in fresh processes.
        run(python,'-I','-B',ROOT/'tests/worker_test.py')
    if sys.platform=='darwin':
        run('codesign','--verify','--deep','--strict',prefix/'UmekoCpp.app')
    output=ROOT/'release-assets';output.mkdir(exist_ok=True)
    stem='UmekoCpp-v'+build_config['version']+('' if bundle_python else '-minimal')+'-'+target
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
    disk_image = {'diskImage': create_dmg(prefix/'UmekoCpp.app', output/(stem+'.dmg'), 'Umeko-Desktop')} if sys.platform == 'darwin' else {}
    (output/(stem+'.json')).write_text(json.dumps({**info,'bytes':archive.stat().st_size,'sha256':digest,**disk_image},indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'archive':str(archive),'bytes':archive.stat().st_size,'sha256':digest}))

if __name__=='__main__':main()
