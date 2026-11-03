"""Inert filesystem helper for the native Skill Widget (no Qt or provider calls).
One process belongs to one Widget/chat context; its import plans and pinned
filesystem identities are discarded when that context closes.
"""
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'provider-runtime'))
from umeko_skills.skill_folders import folder_snapshots, checked_folder, preview_folder, rename_folder, delete_folder
from umeko_skills.skill_import import preview_import, commit_import, import_local, discard_import
from umeko_skills.bundled_skills import describe_bundled, install_bundled

if sys.argv[1:2] == ['--install-bundled']:
    install_bundled(ROOT / 'bundled-skills', Path(sys.argv[2]))
    sys.exit(0)

catalog, folders, previews = {}, {}, {}
def node(context, **action):
    env = dict(os.environ, PI_OFFLINE='1')
    env.pop('NODE_OPTIONS', None); env.pop('NODE_PATH', None)
    value = subprocess.run([context['node'], str(Path(context['release']) / 'skills.mjs')],
        input=json.dumps({**context, **action}), capture_output=True, text=True, encoding='utf-8',
        timeout=45, env=env, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    result = json.loads(value.stdout)
    if value.returncode or not result.get('ok'): raise ValueError(result.get('error', 'Skill discovery failed'))
    describe_bundled(result.get('skills', []), context['agentDir'], ROOT / 'bundled-skills')
    return result

def execute(request):
    global catalog, folders
    op, args, context = request['operation'], request.get('arguments', {}), request['context']
    agent_root, cwd = Path(context['agentDir']).resolve(), Path(context['cwd']).resolve()
    if op in ('skills.browse', 'skills.list'):
        result = node(context, action='browse' if op.endswith('browse') else 'list')
        roots = {'global': agent_root / 'skills'}
        if context.get('includeProject'): roots['project'] = cwd / '.pi/skills'
        result['roots'] = {key:str(path) for key,path in roots.items()}
        if op.endswith('browse'): folders = folder_snapshots(result, roots)
        catalog = {row['id']:row for row in result['skills']}
        return result
    if op in ('skills.importPreview', 'skills.importLocal', 'skills.paste'):
        project = args.get('project', False)
        if type(project) is not bool or project and not context.get('includeProject'): raise ValueError('Select a workspace first')
        destination = cwd / '.pi/skills' if project else agent_root / 'skills'
        rows = [row for row in catalog.values() if Path(row['path']).parent.is_relative_to(destination)]
        def validate(documents): return node(context, action='validateText', documents=documents)['documents']
        if op != 'skills.importPreview':
            result = import_local(args['source'], destination, rows, validate)
            return {'ok':not bool(result['errors']), **result, 'error':'\n'.join(row['name']+': '+row['error'] for row in result['errors'][:8])}
        value = preview_import(args['source'], destination, rows, validate, preserve_folder_names=True)
        while len(previews)>=4:
            key=next(iter(previews));discard_import(previews.pop(key)[0])
        previews[value['planId']] = (value, destination)
        return {'ok':True, 'preview':{'id':value['planId'], 'skills':value['skills'], 'errors':value['errors'], 'project':project, 'sourceName':Path(value['source']).name}}
    if op == 'skills.import':
        value, destination = previews[args['id']]
        if value.get('errors'): raise ValueError('Resolve validation errors first')
        result = commit_import(value, args.get('decisions', {}), destination)
        previews.pop(args['id'],None)
        return {'ok':True, **result}
    row = catalog.get(args.get('id'))
    if not row: raise ValueError('Refresh the current Skill directory first')
    folder = folders.get(row['id'])
    if op in ('skills.preview','skills.open','skills.copy','skills.insert','skills.rename','skills.delete','skills.attach'):
        if not folder: raise ValueError('Refresh the current Skill folder first')
        root = cwd / '.pi/skills' if folder['scope']=='project' else agent_root / 'skills'
        if folder['root'] != root: raise ValueError('The Skill directory changed')
        directory = checked_folder(folder)
        if op == 'skills.preview': return {'ok':True,'details':{'id':row['id'],**preview_folder(folder)}}
        if op == 'skills.rename': directory = rename_folder(folder,args['name'])
        if op == 'skills.delete': delete_folder(folder)
        if op == 'skills.attach':
            if type(args.get('enabled')) is not bool or not row.get('canToggle'): raise ValueError('This Skill cannot be enabled; refresh its state')
            return {'ok':True,'path':row['path'],'enabled':args['enabled']}
        return {'ok':True,'directory':str(directory)}
    if op in ('skills.details','skills.default'):
        return node(context,action=op.split('.')[1],**args)
    raise ValueError('Unsupported Skill operation')

for line in sys.stdin:
    identifier = ''
    try:
        request = json.loads(line); identifier = request['id']
        result = execute(request)
    except Exception as error:
        result = {'ok':False,'error':str(error)[:800]}
    print(json.dumps({'id':identifier,'result':result},ensure_ascii=False),flush=True)
