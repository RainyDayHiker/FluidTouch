"""Writes lrt_version.h: LowRider Touch's version from git (specs/09 section 6, D104).

The firmware build runs it as a PlatformIO `pre:` script: the header goes into the build
folder, which is put on the include path. The simulator's CMake runs it on every build:
    python tools/lrt_version.py <output folder>
The header is rewritten only when its text changes, so only src/app/version.cpp is rebuilt.

LRT_VERSION   "lrt-m5" on a tagged commit, "lrt-m5+3" three commits after it, "untagged"
LRT_DETAIL    "af78053", with ", <branch>" off lrt/main and ", modified" with local changes
LRT_BUILT     the build's date, "2026-11-27"
"""
import datetime
import os
import subprocess
import sys


def git(root, *args):
    try:
        r = subprocess.run(['git', *args], cwd=root, capture_output=True, text=True, timeout=20)
    except (OSError, subprocess.SubprocessError):
        return None
    return r.stdout.strip() if r.returncode == 0 else None


def describe(root):
    commit = git(root, 'rev-parse', '--short=7', 'HEAD')
    if not commit:
        return 'unknown', '', datetime.date.today().isoformat()
    version = 'untagged'
    d = git(root, 'describe', '--tags', '--match', 'lrt-*', '--long', '--abbrev=7')
    if d:
        tag, count, _ = d.rsplit('-', 2)
        version = tag if count == '0' else '%s+%s' % (tag, count)
    detail = commit
    branch = git(root, 'rev-parse', '--abbrev-ref', 'HEAD') or ''
    if branch == 'HEAD':  # CI checks out a detached commit
        branch = os.environ.get('GITHUB_HEAD_REF') or os.environ.get('GITHUB_REF_NAME') or ''
    if branch and branch != 'lrt/main':
        detail += ', ' + branch
    if git(root, 'status', '--porcelain', '--untracked-files=no'):
        detail += ', modified'
    return version, detail, datetime.date.today().isoformat()


def write(root, out_dir):
    version, detail, built = describe(root)
    text = ('// Written by tools/lrt_version.py at build time; not in git\n'
            '#pragma once\n'
            '#define LRT_VERSION "%s"\n'
            '#define LRT_DETAIL "%s"\n'
            '#define LRT_BUILT "%s"\n') % (version, detail.replace('"', "'"), built)
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, 'lrt_version.h')
    try:
        with open(path, encoding='utf-8') as f:
            if f.read() == text:
                return version, detail
    except OSError:
        pass
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    return version, detail


if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    v, d = write(os.path.dirname(here), sys.argv[1])
    print('lrt_version: %s (%s)' % (v, d))
else:
    # PlatformIO (SCons) runs this file with Import() among its globals
    Import('env')  # noqa: F821
    out = os.path.join(env.subst('$BUILD_DIR'), 'lrt_version')  # noqa: F821
    v, d = write(env.subst('$PROJECT_DIR'), out)  # noqa: F821
    print('lrt_version: %s (%s)' % (v, d))
    env.Append(CPPPATH=[out])  # noqa: F821
