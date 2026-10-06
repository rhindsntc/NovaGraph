#!/usr/bin/env python3
"""Export a clean public source snapshot without any Git history.

This checks file selection, not secret contents. Review the resulting archive
before importing it into a new repository; never push the private repository.
"""
import argparse
from pathlib import Path, PurePosixPath
import subprocess
import sys


def git(*args):
    return subprocess.check_output(['git', *args])


def export(output):
    root = Path(git('rev-parse', '--show-toplevel').decode().strip())
    # Include untracked public source so newly written files cannot be silently lost.
    if git('-C', str(root), 'status', '--porcelain', '--untracked-files=all'):
        raise ValueError('A clean checkout is required. Review and commit the public changes first.')
    revision = git('rev-parse', 'HEAD').decode().strip()
    entries = git('ls-tree', '-rz', '--full-tree', revision).split(b'\0')
    for entry in filter(None, entries):
        metadata, raw_path = entry.split(b'\t', 1)
        mode, kind, _ = metadata.split()
        name = raw_path.decode('utf-8')
        parts = PurePosixPath(name).parts
        if (parts[0] == 'docs' or name == 'benchmarks/performance/PLAN.md'
                or any(part in {'.git', '.idea', '.build', 'build', 'dist', 'node_modules'} for part in parts)
                or any(part == '.env' or (part.startswith('.env.') and part != '.env.example') for part in parts)
                or name.endswith(('.p12', '.mobileprovision'))):
            raise ValueError(f'Private or generated path is still tracked: {name}')
        if mode == b'120000':
            raise ValueError(f'Review symlink before public export: {name}')
        if kind != b'blob':
            raise ValueError(f'Unsupported source entry: {name}')
    output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive creation avoids silently replacing an already reviewed snapshot.
    with output.open('xb') as archive:
        try:
            subprocess.run(['git', '-C', str(root), 'archive', '--format=zip', '--prefix=Nova/', revision], stdout=archive, check=True)
        except BaseException:
            output.unlink(missing_ok=True)
            raise
    print(f'Exported {revision} to {output}. No Git history included; nothing published.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('dist/Nova-source.zip'))
    args = parser.parse_args()
    try:
        export(args.output)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
