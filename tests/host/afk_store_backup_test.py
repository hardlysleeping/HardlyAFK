#!/usr/bin/env python3
"""Host-only offline backup contract; all fixtures live under TMPDIR."""
import importlib.util
import os
from pathlib import Path
import sqlite3
import struct
import subprocess
import sys
import tempfile

spec = importlib.util.spec_from_file_location('backup', 'tools/afk-store-backup.py')
assert spec is not None and spec.loader is not None
backup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backup)

def stable_inventory(root):
    # SQLite read locks may update/recreate ephemeral shared-memory sidecars.
    return {k: v for k, v in backup.inventory(root).items()
            if not k.endswith(('-shm', '-wal'))}


with tempfile.TemporaryDirectory(prefix='afk-backup-test-', dir=os.environ['TMPDIR']) as temp:
    base = Path(temp)
    source = base / 'source'
    source.mkdir()
    subprocess.run([sys.argv[1], '--root', str(source), 'create', '--name', 'Test',
                    '--distro', 'debian', '--version', '13'], check=True)
    with sqlite3.connect(source / 'catalog.db') as catalog:
        ident, rel = catalog.execute('select id, relative_path from environments').fetchone()
        catalog.execute('insert into env_desktops values (?,?)', (ident, 'openbox'))
    fs = source / rel / 'rootfs'
    data = fs / 'data'
    data.mkdir()
    (data / 'hello').write_bytes(b'guest content\x00\xff')
    (data / 'hello').chmod(0o751)
    os.link(data / 'hello', data / 'hardlink')
    # Upstream fakefs stores guest symlink targets as regular host files.
    (data / 'guest-link').write_text('/hello')
    (data / 'host-link').symlink_to('/nonexistent-outside')
    db = sqlite3.connect(fs / 'meta.db')
    db.executescript('''pragma journal_mode=wal; pragma user_version=3;
        create table meta(id integer unique default 0, db_inode integer);
        insert into meta values(0,0);
        create table stats(inode integer primary key, stat blob);
        create table paths(path blob primary key,inode integer references stats(inode));
        create index inode_to_path on paths(inode,path);''')
    for inode, path, mode in [(1,b'',0o40755),(2,b'/hello',0o100751),
                              (2,b'/hardlink',0o100751),(3,b'/guest-link',0o120777)]:
        db.execute('insert or ignore into stats values (?,?)',
                   (inode, struct.pack('<IIII',mode,123,456,0)))
        db.execute('insert into paths values (?,?)',(path,inode))
    db.commit()  # retain open WAL connection to exercise SQLite backup
    before = stable_inventory(source)
    bundle, restored = base / 'bundle', base / 'restored'
    backup.transfer('export', source, bundle, True)
    backup.transfer('import', bundle, restored, True)
    assert backup.inventory(bundle / 'store') == backup.inventory(restored)
    out = restored / rel / 'rootfs'
    assert (out / 'data/hello').stat().st_ino == (out / 'data/hardlink').stat().st_ino
    with sqlite3.connect(out / 'meta.db') as check:
        assert check.execute('select * from stats').fetchall() == db.execute('select * from stats').fetchall()
        assert check.execute('select * from paths').fetchall() == db.execute('select * from paths').fetchall()
    assert stable_inventory(source) == before
    for action, src, dst, quiet in [('import',bundle,source,True),
                                     ('export',source,base/'refused',False)]:
        try:
            backup.transfer(action,src,dst,quiet)
            raise AssertionError('accepted unsafe operation')
        except ValueError:
            pass
    (bundle / 'store' / rel / 'rootfs/data/hello').write_text('corrupt')
    try:
        backup.transfer('import',bundle,base/'corrupt-result',True)
        raise AssertionError('accepted corrupt bundle')
    except ValueError:
        pass
    assert not (base/'corrupt-result').exists()
    assert stable_inventory(source) == before
    # Make a second genuine catalog environment; synthetic fakefs only.
    subprocess.run([sys.argv[1], '--root', str(source), 'create', '--name', 'Second',
                    '--distro', 'alpine', '--version', '3'], check=True)
    with backup.contextlib.closing(sqlite3.connect(source / 'catalog.db')) as catalog:
        second, second_rel = catalog.execute('select id,relative_path from environments where name="Second"').fetchone()
    backup.copy_store(fs, source / second_rel / 'fixture', True)
    (source / second_rel / 'rootfs').rmdir()
    os.rename(source / second_rel / 'fixture', source / second_rel / 'rootfs')
    original_trees = {r: stable_inventory(source / r) for r in (rel, second_rel)}
    single = base / 'single'
    backup.environment_transfer('export-env', source, single, True, ident)
    clone = subprocess.check_output([sys.executable, 'tools/afk-store-backup.py',
        'import-env', str(single), str(source), '--quiescent', '--name', 'Clone'], text=True).strip()
    assert clone not in (ident, second)
    with backup.contextlib.closing(sqlite3.connect(source / 'catalog.db')) as catalog:
        assert catalog.execute('select count(*) from environments').fetchone() == (3,)
        assert catalog.execute('select name from environments where id=?', (clone,)).fetchone() == ('Clone',)
        assert catalog.execute('select desktop_id from env_desktops where env_id=?', (clone,)).fetchall() == [('openbox',)]
    clone_fs = source / 'environments' / clone / 'rootfs'
    assert backup.inventory(clone_fs) == backup.inventory(single / 'store' / rel / 'rootfs')
    assert (clone_fs/'data/hello').stat().st_ino == (clone_fs/'data/hardlink').stat().st_ino
    merged = stable_inventory(source)
    from unittest.mock import patch
    with patch.object(backup.os, 'rename', side_effect=OSError('injected publication failure')):
        try:
            backup.environment_transfer('import-env', single, source, True, name='Failed')
            raise AssertionError('publication unexpectedly succeeded')
        except OSError:
            pass
    assert stable_inventory(source) == merged
    # A store-level symlink must never redirect database/tree writes outside it.
    redirected = base / 'redirected'
    redirected.mkdir()
    (redirected / 'environments').symlink_to(source / 'environments')
    try:
        backup.environment_transfer('import-env', single, redirected, True, name='Escape')
        raise AssertionError('accepted redirected environments directory')
    except ValueError:
        pass
    assert stable_inventory(source) == merged
    for quiet, name in [(False, 'Other'), (True, None), (True, 'Clone')]:
        try:
            backup.environment_transfer('import-env', single, source, quiet, name=name)
            raise AssertionError('accepted unsafe merge')
        except (ValueError, sqlite3.IntegrityError):
            pass
        assert stable_inventory(source) == merged
    with backup.contextlib.closing(sqlite3.connect(source / 'catalog.db')) as catalog:
        catalog.execute('insert into catalog_meta values (?,?)', ('active_environment', ident))
        catalog.commit()
    try:
        backup.environment_transfer('export-env', source, base/'active', True, ident)
        raise AssertionError('accepted active source')
    except ValueError:
        pass
    merged = stable_inventory(source)
    (single / 'store' / rel / 'rootfs/data/hello').write_bytes(b'corrupt')
    try:
        backup.environment_transfer('import-env', single, source, True, name='Corrupt')
        raise AssertionError('accepted corrupt merge')
    except ValueError:
        pass
    assert stable_inventory(source) == merged
    for r, entries in original_trees.items():
        assert stable_inventory(source / r) == entries
    print('individual environment: two existing envs, new UUID/name, desktops, fakefs WAL, permissions, hardlinks, active/quiescence/name/corrupt refusal, existing trees unchanged: PASS')
    db.close()
    (fs/'meta.db').write_bytes(b'not sqlite')
    try:
        backup.transfer('export',source,base/'bad-db',True)
        raise AssertionError('accepted corrupt SQLite')
    except sqlite3.Error:
        pass
    assert not (base/'bad-db').exists()
print('offline store backup: round-trip, WAL, guest metadata, symlinks, modes, hardlinks, corrupt refusal, existing destination untouched: PASS')
