#!/usr/bin/env python3
"""Offline host store backup. No archive extraction; destination must not exist."""
import contextlib
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import stat
import tempfile

FORMAT = 'hardlyafk-offline-store-v1'


def inventory(root):
    result = {}
    def walk(path):
        for item in sorted(path.iterdir()):
            rel = str(item.relative_to(root))
            st = item.lstat()
            mode = stat.S_IMODE(st.st_mode)
            if stat.S_ISLNK(st.st_mode):
                result[rel] = ['link', os.readlink(item)]
            elif stat.S_ISDIR(st.st_mode):
                result[rel] = ['dir', mode]
                walk(item)
            elif stat.S_ISREG(st.st_mode):
                with item.open('rb') as f:
                    digest = hashlib.file_digest(f, 'sha256').hexdigest()
                result[rel] = ['file', mode, st.st_size, digest]
            else:
                raise ValueError('unsupported host node: ' + rel)
    walk(root)
    return result


def check_database(path, fake=False):
    if path.is_symlink() or not path.is_file():
        raise ValueError('missing regular database: ' + str(path))
    with contextlib.closing(sqlite3.connect(path.as_uri() + '?mode=ro', uri=True)) as db:
        if db.execute('pragma integrity_check').fetchall() != [('ok',)]:
            raise ValueError('database integrity failure')
        if fake:
            if db.execute('pragma user_version').fetchone()[0] != 3:
                raise ValueError('unsupported fakefs schema')
            db.execute('select path, inode, stat from paths join stats using(inode)').fetchall()
            if db.execute("select count(*) from paths where path=x''").fetchone()[0] != 1:
                raise ValueError('missing fakefs root')


def validate_store(root):
    check_database(root / 'catalog.db')
    with contextlib.closing(sqlite3.connect((root / 'catalog.db').as_uri() + '?mode=ro', uri=True)) as db:
        rows = db.execute('select id, relative_path from environments').fetchall()
    expected = set()
    import uuid
    for ident, rel in rows:
        if str(uuid.UUID(ident)) != ident or rel != 'environments/' + ident:
            raise ValueError('unsafe environment path')
        expected.add(ident)
        env = root / rel
        if env.is_symlink() or not env.is_dir():
            raise ValueError('missing environment')
        # The current store materialises rootfs; real fakefs must occupy it,
        # not a plain unpacked distribution directory.
        fs = env / 'rootfs'
        if fs.is_symlink() or (fs / 'data').is_symlink() or not (fs / 'data').is_dir():
            raise ValueError('rootfs is not a fakefs')
        check_database(fs / 'meta.db', fake=True)
    directory = root / 'environments'
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError('missing environments directory')
    if {p.name for p in directory.iterdir()} != expected:
        raise ValueError('orphan or missing environment directory')
    return rows


def copy_store(source, target, snapshot):
    inodes = {}
    def copy_file(src, dst):
        src, dst = Path(src), Path(dst)
        st = src.stat()
        key = (st.st_dev, st.st_ino)
        if key in inodes:
            os.link(inodes[key], dst)
        elif snapshot and src.name in ('catalog.db', 'meta.db'):
            with contextlib.closing(sqlite3.connect(src.as_uri() + '?mode=ro', uri=True)) as db:
                with contextlib.closing(sqlite3.connect(dst)) as out:
                    db.backup(out)
            shutil.copystat(src, dst)
            inodes[key] = dst
        else:
            shutil.copy2(src, dst)
            inodes[key] = dst
        return str(dst)
    def ignore(path, names):
        return [n for n in names if snapshot and
                n in ('catalog.db-wal', 'catalog.db-shm', 'meta.db-wal', 'meta.db-shm') and
                (Path(path) / n.removesuffix('-wal').removesuffix('-shm')).is_file()]
    shutil.copytree(source, target, symlinks=True, copy_function=copy_file, ignore=ignore)


def transfer(action, source, destination, quiescent):
    source = Path(source).resolve()
    destination = Path(destination).absolute()
    if not quiescent:
        raise ValueError('stop guest and all file-provider/writer access; pass --quiescent')
    if destination.exists() or destination.is_symlink():
        raise ValueError('destination already exists; replacement is forbidden')
    if source == destination or source in destination.parents:
        raise ValueError('destination is inside source')
    manifest = {}
    if action == 'export':
        inventory(source)  # refuse unsupported nodes before copying
        validate_store(source)
        payload = source
    else:
        manifest = json.loads((source / 'manifest.json').read_text())
        payload = source / 'store'
        if payload.is_symlink() or manifest.get('format') != FORMAT:
            raise ValueError('unsupported bundle')
        if inventory(payload) != manifest.get('entries'):
            raise ValueError('bundle contents do not match manifest')
        validate_store(payload)
    # Same-parent staging: failures never publish a partial destination.
    with tempfile.TemporaryDirectory(prefix='.afk-backup-', dir=destination.parent) as temp:
        stage = Path(temp) / 'result'
        if action == 'export':
            stage.mkdir(mode=0o700)
            copy_store(payload, stage / 'store', True)
            validate_store(stage / 'store')
            (stage / 'manifest.json').write_text(json.dumps({
                'format': FORMAT, 'entries': inventory(stage / 'store')}, sort_keys=True))
        else:
            copy_store(payload, stage, False)
            if inventory(stage) != manifest['entries']:
                raise ValueError('restore verification failed')
            validate_store(stage)
        # Exclusive publication; callers must serialize directory access.
        if destination.exists() or destination.is_symlink():
            raise ValueError('destination appeared during operation')
        os.rename(stage, destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['export', 'import'])
    parser.add_argument('source')
    parser.add_argument('destination')
    parser.add_argument('--quiescent', action='store_true')
    args = parser.parse_args()
    try:
        transfer(args.action, args.source, args.destination, args.quiescent)
    except (OSError, ValueError, sqlite3.Error) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
