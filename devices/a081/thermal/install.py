#!/usr/bin/env python3
"""Install profile controls in an existing osstore rootfs, with file rollback."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

FILES = ['usr/local/lib/a081-thermal/a081-thermal-profile',
         'usr/local/lib/a081-thermal/profiles.json',
         'usr/local/sbin/a081-thermal-profile',
         'etc/systemd/system/a081-thermal-profile.service',
         'etc/systemd/system/multi-user.target.wants/a081-thermal-profile.service',
         'etc/a081-thermal-profile']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--backup', type=Path, required=True)
    parser.add_argument('--restore', action='store_true')
    args = parser.parse_args()
    root = args.root.resolve()
    backup = args.backup.resolve()
    if os.geteuid() != 0 or not root.is_relative_to('/osstore/systems') or not backup.is_relative_to('/osstore/updates'):
        parser.error('Expected root and existing osstore installation/backup paths')
    marker = (root / 'etc/a081-rootfs-id').read_text().strip()
    if marker not in ('a081-steamos-arm64', 'a081-debian-trixie-arm64'):
        parser.error('Unexpected rootfs identity')
    if args.restore:
        manifest = json.loads((backup / 'files.json').read_text())
        if manifest['root'] != str(root):
            parser.error('Backup belongs to another rootfs')
        for name in FILES:
            (root / name).unlink(missing_ok=True)
        subprocess.run(['tar', '-xpf', str(backup / 'files.tar'), '-C', str(root)], check=True)
        return
    backup.mkdir(parents=True, exist_ok=False)
    existing = [name for name in FILES if (root / name).exists() or (root / name).is_symlink()]
    subprocess.run(['tar', '-cpf', str(backup / 'files.tar'), '-C', str(root),
                    '--files-from=/dev/null', *existing], check=True)
    (backup / 'files.json').write_text(json.dumps(dict(root=str(root), existing=existing,
                                                     installed=FILES), indent=2) + '\n')
    source = Path(__file__).resolve().parent
    for src, name, mode in [('a081-thermal-profile', FILES[0], 0o755),
                            ('profiles.json', FILES[1], 0o644),
                            ('a081-thermal-profile.service', FILES[3], 0o644)]:
        dest = root / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.unlink(missing_ok=True)
        shutil.copyfile(source / src, dest)
        dest.chmod(mode)
    for name, target in [(FILES[2], '../lib/a081-thermal/a081-thermal-profile'),
                          (FILES[4], '../a081-thermal-profile.service')]:
        dest = root / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.unlink(missing_ok=True)
        dest.symlink_to(target)
    (root / FILES[5]).write_text('factory\n')
    (root / FILES[5]).chmod(0o644)
    print(json.dumps(dict(installed=True, root=str(root), default='factory', backup=str(backup))))


if __name__ == '__main__':
    main()
