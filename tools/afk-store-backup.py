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
    directory = root / 'environments'
    if root.is_symlink() or directory.is_symlink() or not directory.is_dir():
        raise ValueError('unsafe store directory')
    # SQLite may follow sidecars; never permit host links for these files.
    for parent in [root] + [p / 'rootfs' for p in directory.iterdir() if not p.is_symlink()]:
        for filename in ('catalog.db-wal', 'catalog.db-shm', 'meta.db-wal', 'meta.db-shm'):
            if (parent / filename).is_symlink():
                raise ValueError('unsafe SQLite sidecar')
    check_database(root / 'catalog.db')
    with contextlib.closing(sqlite3.connect((root / 'catalog.db').as_uri() + '?mode=ro', uri=True)) as db:
        if db.execute('pragma user_version').fetchone() != (1,):
            raise ValueError('unsupported catalog schema')
        if db.execute('pragma foreign_key_check').fetchall():
            raise ValueError('invalid catalog references')
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
                    out.execute('pragma journal_mode=delete')
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


ENV_FORMAT = 'hardlyafk-offline-environment-v1'


def environment_transfer(action, source, destination, quiescent, ident=None, name=None):
    """Trusted, serialized offline access only; never overwrite an environment."""
    import uuid
    if not quiescent:
        raise ValueError('stop runtime and all writers; pass --quiescent')
    source = Path(source).resolve()
    destination = Path(destination).absolute()
    if action == 'export-env':
        rows = validate_store(source)
        if ident not in dict(rows):
            raise ValueError('unknown environment UUID')
        with contextlib.closing(sqlite3.connect(source / 'catalog.db')) as db:
            active = db.execute("select value from catalog_meta where key='active_environment'").fetchone()
            if active and active[0] == ident:
                raise ValueError('deselect the active environment before export')
        if destination.exists() or destination.is_symlink() or source in destination.parents:
            raise ValueError('destination must be absent and outside source')
        inventory(source / dict(rows)[ident])
        with tempfile.TemporaryDirectory(prefix='.afk-env-', dir=destination.parent) as temp:
            stage = Path(temp) / 'bundle'
            payload = stage / 'store'
            (payload / 'environments').mkdir(parents=True, mode=0o700)
            with contextlib.closing(sqlite3.connect(source / 'catalog.db')) as db:
                with contextlib.closing(sqlite3.connect(payload / 'catalog.db')) as out:
                    db.backup(out)
                    out.execute('delete from env_desktops where env_id != ?', (ident,))
                    out.execute('delete from environments where id != ?', (ident,))
                    out.execute('delete from catalog_meta')
                    out.commit()
                    # Do not leak deleted environments through SQLite free pages.
                    out.execute('vacuum')
                    out.execute('pragma journal_mode=delete')
            shutil.copystat(source / 'catalog.db', payload / 'catalog.db')
            copy_store(source / dict(rows)[ident], payload / dict(rows)[ident], True)
            validate_store(payload)
            (stage / 'manifest.json').write_text(json.dumps({
                'format': ENV_FORMAT, 'entries': inventory(payload)}, sort_keys=True))
            if destination.exists() or destination.is_symlink():
                raise ValueError('destination appeared')
            os.rename(stage, destination)
        return ident
    if not name or not name.strip() or len(name.encode()) > 127 or '\x00' in name:
        raise ValueError('explicit nonempty --name (at most 127 UTF-8 bytes) required')
    manifest = json.loads((source / 'manifest.json').read_text())
    payload = source / 'store'
    if payload.is_symlink() or manifest.get('format') != ENV_FORMAT:
        raise ValueError('unsupported environment bundle')
    if inventory(payload) != manifest.get('entries'):
        raise ValueError('bundle contents do not match manifest')
    rows = validate_store(payload)
    if len(rows) != 1:
        raise ValueError('expected exactly one environment')
    inventory(destination)
    validate_store(destination)
    old_id, rel = rows[0]
    with contextlib.closing(sqlite3.connect(payload / 'catalog.db')) as db:
        record = list(db.execute('select id,name,distro_id,distro_version,architecture,relative_path,created_at,default_user,default_session from environments').fetchone())
        desktops = db.execute('select desktop_id from env_desktops where env_id=?', (old_id,)).fetchall()
    new_id = str(uuid.uuid4())
    new_rel = 'environments/' + new_id
    final = destination / new_rel
    with tempfile.TemporaryDirectory(prefix='.afk-env-', dir=destination) as temp:
        stage = Path(temp) / 'environment'
        copy_store(payload / rel, stage, False)
        # Compare the standalone snapshots, including modes and all fakefs bytes.
        if inventory(stage) != inventory(payload / rel):
            raise ValueError('environment verification failed')
        with contextlib.closing(sqlite3.connect(destination / 'catalog.db')) as db:
            db.execute('pragma foreign_keys=on')
            db.execute('begin immediate')
            published = False
            try:
                record[0], record[1], record[5] = new_id, name, new_rel
                db.execute('insert into environments values (?,?,?,?,?,?,?,?,?)', record)
                db.executemany('insert into env_desktops values (?,?)', [(new_id, d[0]) for d in desktops])
                if final.exists() or final.is_symlink():
                    raise ValueError('UUID destination already exists')
                os.rename(stage, final)
                published = True
                db.commit()
            except BaseException:
                db.rollback()
                if published:
                    shutil.rmtree(final)
                raise
    return new_id


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['export', 'import', 'export-env', 'import-env'])
    parser.add_argument('--id', help='source environment UUID for export-env')
    parser.add_argument('--name', help='explicit new environment name for import-env')
    parser.add_argument('source')
    parser.add_argument('destination')
    parser.add_argument('--quiescent', action='store_true')
    args = parser.parse_args()
    try:
        if args.action.endswith('-env'):
            print(environment_transfer(args.action, args.source, args.destination,
                                       args.quiescent, args.id, args.name))
        else:
            transfer(args.action, args.source, args.destination, args.quiescent)
    except (OSError, ValueError, sqlite3.Error) as error:
        parser.exit(1, str(error) + '\n')


if __name__ == '__main__':
    main()
