#!/usr/bin/env python3
"""Executable host regressions for the iOS resolver; synthetic fakefs, no guest boot."""
import ctypes
import hashlib
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class BootRootTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="afk-ios-build-")
        lib = Path(cls.build.name) / "boot.so"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-shared", "-fPIC", "-I", str(REPO),
                        str(REPO / "core/environment/afk_env_catalog.c"),
                        str(REPO / "core/environment/afk_ios_boot.c"), "-lsqlite3", "-o", str(lib)], check=True)
        cls.lib = ctypes.CDLL(str(lib))
        cls.lib.afk_catalog_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.afk_catalog_close.argtypes = [ctypes.c_void_p]
        cls.lib.afk_ios_boot_root.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="afk-ios-store-")
        self.root = Path(self.tmp.name)
        self.catalog = ctypes.c_void_p()
        self.err = ctypes.create_string_buffer(512)
        self.out = ctypes.create_string_buffer(1024)
        self.assertEqual(self.lib.afk_catalog_open(os.fsencode(self.root), ctypes.byref(self.catalog), self.err, 512), 0)
        self.identifier = "12345678-1234-4234-8234-123456789abc"
        self.relative = f"environments/{self.identifier}"
        # Use the real catalog schema, not a replacement implementation.
        with sqlite3.connect(self.root / "catalog.db") as db:
            db.execute("INSERT INTO environments (id,name,distro_id,distro_version,architecture,relative_path,created_at,default_user,default_session) VALUES (?,?,?,?,?,?,?,?,?)",
                       (self.identifier, "Test", "alpine", "test", "aarch64", self.relative, 1, "root", "terminal"))
        self.fs = self.root / self.relative / "rootfs"
        (self.fs / "data").mkdir(parents=True)
        (self.fs / "data/sentinel").write_bytes(b"preserve guest data")
        with sqlite3.connect(self.fs / "meta.db") as db:
            db.executescript("""
                CREATE TABLE meta (id INTEGER UNIQUE DEFAULT 0, db_inode INTEGER);
                INSERT INTO meta (db_inode) VALUES (0);
                CREATE TABLE stats (inode INTEGER PRIMARY KEY, stat BLOB);
                CREATE TABLE paths (path BLOB PRIMARY KEY, inode INTEGER REFERENCES stats(inode));
                PRAGMA user_version=3;
            """)

    def tearDown(self):
        self.lib.afk_catalog_close(self.catalog)
        self.tmp.cleanup()

    def resolve(self, identifier=None):
        return self.lib.afk_ios_boot_root(self.catalog, (identifier or self.identifier).encode(), self.out, self.err, 512)

    def test_resolves_complete_fakefs_without_modification(self):
        before = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in [self.fs / "meta.db", self.fs / "data/sentinel"]}
        self.assertEqual(self.resolve(), 0, self.err.value)
        self.assertEqual(Path(os.fsdecode(self.out.value)), self.fs)
        after = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in [self.fs / "meta.db", self.fs / "data/sentinel"]}
        self.assertEqual(before, after)
        self.assertLess(self.resolve("../../roots/default"), 0)
        self.assertEqual(self.out.value, b"")

    def test_invalid_arguments_and_unknown_identity_fail_closed(self):
        self.assertLess(self.lib.afk_ios_boot_root(None, self.identifier.encode(), self.out, self.err, 512), 0)
        self.assertEqual(self.out.value, b"")
        self.assertLess(self.lib.afk_ios_boot_root(self.catalog, None, self.out, self.err, 512), 0)
        self.assertLess(self.lib.afk_ios_boot_root(self.catalog, self.identifier.encode(), None, None, 0), 0)
        self.assertLess(self.resolve("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"), 0)

    def test_reopen_and_other_environment_are_isolated(self):
        import shutil
        other = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
        other_fs = self.root / "environments" / other / "rootfs"
        shutil.copytree(self.fs, other_fs)
        (other_fs / "data/sentinel").write_bytes(b"independent guest")
        with sqlite3.connect(self.root / "catalog.db") as db:
            db.execute("INSERT INTO environments SELECT ?,?,distro_id,distro_version,architecture,?,created_at,default_user,default_session FROM environments WHERE id=?",
                       (other, "Other", f"environments/{other}", self.identifier))
            db.execute("INSERT INTO catalog_meta VALUES ('active_environment', ?)", (self.identifier,))
        self.lib.afk_catalog_close(self.catalog)
        self.assertEqual(self.lib.afk_catalog_open(os.fsencode(self.root), ctypes.byref(self.catalog), self.err, 512), 0)
        self.assertEqual(self.resolve(other), 0, self.err.value)
        self.assertEqual(Path(os.fsdecode(self.out.value)), other_fs)
        self.assertEqual((self.fs / "data/sentinel").read_bytes(), b"preserve guest data")
        with sqlite3.connect(self.root / "catalog.db") as db:
            self.assertEqual(db.execute("SELECT value FROM catalog_meta WHERE key='active_environment'").fetchone()[0], self.identifier)
        # A valid relative path is insufficient: it must belong to this exact UUID.
        with sqlite3.connect(self.root / "catalog.db") as db:
            db.execute("UPDATE environments SET relative_path='elsewhere' WHERE id=?", (other,))
        self.assertLess(self.resolve(other), 0)
        self.assertEqual(self.out.value, b"")

    def test_plain_rootfs_missing_and_wrong_schema_refused(self):
        (self.fs / "meta.db").unlink()
        self.assertLess(self.resolve(), 0)
        self.assertFalse((self.fs / "meta.db").exists())
        with sqlite3.connect(self.fs / "meta.db") as db:
            db.execute("PRAGMA user_version=2")
        self.assertLess(self.resolve(), 0)

    def test_v3_number_without_fakefs_schema_is_refused(self):
        with sqlite3.connect(self.fs / "meta.db") as db:
            db.execute("DROP TABLE paths")
        self.assertLess(self.resolve(), 0)
        self.assertEqual(self.out.value, b"")

    def test_symlinks_refused_at_each_store_component(self):
        for path in [self.fs / "meta.db", self.fs / "data", self.fs, self.fs.parent, self.fs.parent.parent]:
            moved = path.with_name(path.name + ".saved")
            path.rename(moved)
            path.symlink_to(moved, target_is_directory=moved.is_dir())
            self.assertLess(self.resolve(), 0, str(path))
            path.unlink()
            moved.rename(path)

    def test_unsupported_configuration_refused(self):
        for field, value in [("architecture", "x86"), ("default_user", "alice"), ("default_session", "desktop")]:
            with sqlite3.connect(self.root / "catalog.db") as db:
                old = db.execute(f"SELECT {field} FROM environments").fetchone()[0]
                db.execute(f"UPDATE environments SET {field}=?", (value,))
            self.assertLess(self.resolve(), 0)
            with sqlite3.connect(self.root / "catalog.db") as db:
                db.execute(f"UPDATE environments SET {field}=?", (old,))


class IOSWiringTests(unittest.TestCase):
    def test_boot_uses_validated_process_pinned_root(self):
        delegate = (REPO / "app/AppDelegate.m").read_text()
        self.assertIn("[Roots.instance bootRootURL:&rootError]", delegate)
        self.assertIn("if (!root)", delegate)
        roots = (REPO / "app/Roots.m").read_text()
        self.assertIn("dispatch_once(&once", roots)
        self.assertIn("afk_ios_boot_root(catalog, self.managedSelection", roots)
        selection = roots.split("- (BOOL)selectManagedEnvironment:")[1].split("- (NSURL *)bootRootURL:")[0]
        self.assertNotIn("afk_catalog_set_active(", selection)
        self.assertNotIn("mount_root(", selection)
        self.assertIn('URLByAppendingPathComponent:@"managed-environments"', roots)
        self.assertIn('URLByAppendingPathComponent:@"roots"', roots)

    def test_ui_is_reachable_and_does_not_offer_live_switch(self):
        ui = (REPO / "app/RootsTableViewController.m").read_text()
        self.assertIn("@selector(showManagedEnvironments)", ui)
        self.assertIn("pushViewController:[[ManagedEnvironmentsViewController alloc]", ui)
        self.assertIn("selectManagedEnvironment:identifier", ui)
        self.assertIn("Select for next launch", ui)
        self.assertIn("No automatic shutdown or safe-stop is claimed", ui)
        self.assertIn("@selector(createEnvironment)", ui)
        self.assertIn("createManagedAlpineNamed:name", ui)
        self.assertIn("dispatch_get_global_queue", ui)
        roots = (REPO / "app/Roots.m").read_text()
        creation = roots.split("- (BOOL)createManagedAlpineNamed:")[1].split("- (BOOL)selectManagedEnvironment:")[0]
        self.assertIn("afk_managed_install(", creation)
        self.assertIn('@"root" withExtension:@"tar.gz"', creation)
        for forbidden in ["afk_catalog_set_active(", "setObject:", "mount_root(", "importRootFromArchive:"]:
            self.assertNotIn(forbidden, creation)

    def test_every_roots_source_phase_links_core_and_sqlite(self):
        import re
        project = (REPO / "iSH.xcodeproj/project.pbxproj").read_text()
        phases = re.findall(r"isa = PBXSourcesBuildPhase;.*?files = \((.*?)\);", project, re.S)
        phases = [p for p in phases if "Roots.m in Sources" in p]
        self.assertEqual(len(phases), 2)
        for phase in phases:
            for filename in ["afk_env_catalog.c", "afk_ios_boot.c", "afk_env_store.c", "afk_distro_catalog.c", "afk_sha256.c", "afk_managed_install.c"]:
                match = re.search(r"([A-F0-9]{24}) /\* " + re.escape(filename) + " in Sources", phase)
                assert match is not None, filename
                self.assertRegex(project, match[1] + r" /\* .*? = \{isa = PBXBuildFile;")
        self.assertIn("-lsqlite3", (REPO / "app/App.xcconfig").read_text())


if __name__ == "__main__":
    unittest.main(verbosity=2)
