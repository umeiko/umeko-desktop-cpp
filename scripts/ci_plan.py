"""One explicit platform plan is shared by the CI matrix and release verifier."""
import argparse
import json
from pathlib import Path
import re

NATIVE = (
    dict(name='windows-x64', os='windows-2022', host='windows', arch='win64_msvc2022_64', qt_dir='msvc2022_64'),
    dict(name='linux-x64', os='ubuntu-24.04', host='linux', arch='linux_gcc_64', qt_dir='gcc_64'),
    dict(name='macos-arm64', os='macos-15', host='mac', arch='clang_64', qt_dir='macos'),
    dict(name='macos-x64', os='macos-15-intel', host='mac', arch='clang_64', qt_dir='macos'),
)
ALL_PLATFORMS = {row['name'] for row in NATIVE}


def make_plan(commit, all_platforms=False):
    if not re.fullmatch('[0-9a-f]{40}', commit):
        raise ValueError('An exact source commit is required')
    return {'schema': 1, 'commit': commit,
            'platforms': [row['name'] for row in (NATIVE if all_platforms else NATIVE[:1])]}


def release_platforms(plan, commit):
    if plan.get('schema') != 1 or plan.get('commit') != commit:
        raise ValueError('Build plan does not match the verified source')
    names = plan.get('platforms')
    if not isinstance(names, list) or not all(isinstance(name, str) for name in names):
        raise ValueError('Invalid platform list')
    selected = set(names)
    if len(names) != len(selected) or selected not in ({'windows-x64'}, ALL_PLATFORMS):
        raise ValueError('Expected Windows or the complete four-platform build')
    return selected


def require_successful_jobs(jobs, platforms, shared=False):
    latest = {}
    for job in jobs:
        if job['id'] > latest.get(job['name'], {}).get('id', -1):
            latest[job['name']] = job
    required = set(platforms)
    if shared:
        required.update({'plan', 'Shared runtime and release checks'})
    for name in required:
        if latest.get(name, {}).get('conclusion') != 'success':
            raise ValueError('Required CI job did not succeed: ' + name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--commit', required=True)
    parser.add_argument('--all-platforms', choices=['true', 'false'], default='false')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    plan = make_plan(args.commit, args.all_platforms == 'true')
    selected = release_platforms(plan, args.commit)
    args.output.write_text(json.dumps(plan, indent=2) + '\n', encoding='utf-8')
    print('matrix=' + json.dumps({'include': [row for row in NATIVE if row['name'] in selected]}, separators=(',', ':')))


if __name__ == '__main__':
    main()
