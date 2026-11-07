#!/usr/bin/env python3
"""No-screen root-fence regression. Usage: test_root_fence.py APP_BINARY WORK_DIR OUTPUT_DIR.
WORK_DIR must be a new absolute directory under the disposable fence. Fixtures are retained.
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time
import unittest
import zlib

from qa import (App, FENCE, build_id, canonical_absolute, confirm_native_directory,
                require_fenced, scope_check)

BINARY = None
WORK = None
OUTPUT = None


def snapshot(root):
    """All names, including hidden directories, with content and identity; never follow links."""
    result = {}
    for current, dirs, files in os.walk(root, followlinks=False):
        for path in [Path(current), *(Path(current) / n for n in dirs + files)]:
            st = path.lstat()
            result[str(path.relative_to(root))] = {
                'size': st.st_size, 'mtime_ns': st.st_mtime_ns, 'inode': st.st_ino,
                'sha256': hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() and not path.is_symlink() else None,
            }
    return result


def png(path):
    def chunk(kind, data):
        return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', 8, 8, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress((b'\0' + b'\xff\0\x80' * 8) * 8)) + chunk(b'IEND', b''))


class FenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if WORK is None or BINARY is None or OUTPUT is None:
            raise RuntimeError('explicit APP_BINARY, new WORK_DIR and OUTPUT_DIR required')
        require_fenced(WORK)
        WORK.mkdir(parents=True, exist_ok=False)
        cls.work = WORK.resolve()
        require_fenced(OUTPUT)
        OUTPUT.mkdir(parents=True, exist_ok=True)
        cls.output = scope_check(OUTPUT / cls.work.name, [OUTPUT])
        cls.output.mkdir(exist_ok=False)
        (cls.output / 'dry-run-manifest.json').write_text(json.dumps({
            'write_roots': [str(cls.work)], 'outside_exception': 'explicit task-owned synthetic sibling only',
            'operations': ['create synthetic HOME/profile/roots', 'seed fake repository', 'create scratch symlinks',
                           'exercise QA protocol', 'record snapshots'], 'deletes': ['task-owned temporary symlinks only']}, indent=2))
        cls.home = cls.work / 'home'
        cls.home.mkdir()
        cls.empty_legacy = cls.work / 'empty-legacy'
        cls.empty_legacy.mkdir()
        cls.env = dict(HOME=str(cls.home), CFFIXED_USER_HOME=str(cls.home),
                       XDG_CONFIG_HOME=str(cls.home / '.config'), QT_QPA_PLATFORM='offscreen')
        cls.allowed = cls.work / 'allowed-root'
        cls.second = cls.work / 'second-root'
        for root in (cls.allowed, cls.second):
            (root / 'album').mkdir(parents=True)
            png(root / 'album' / 'sample.png')
        # Explicit task-owned sibling, never a real repository/library or system temp directory.
        cls.outside = Path(os.environ['ROOM237_QA_NEGATIVE_ROOT']).resolve()
        assert cls.outside.is_absolute() and Path(FENCE) not in cls.outside.parents
        cls.outside.mkdir(parents=True, exist_ok=False)
        cls.repo = cls.outside / 'fake-repository'
        (cls.repo / '.git' / 'refs' / 'heads').mkdir(parents=True)
        for path, content in {'.git/refs/heads/main': b'0123456789\n', '.git/index': b'DIRC\0fake',
                              '.git/HEAD': b'ref: refs/heads/main\n', 'source.py': b'print("fixture")\n'}.items():
            (cls.repo / path).write_bytes(content)
        png(cls.repo / 'sample.png')
        cls.substring = cls.outside / ('prefix-' + Path(FENCE).name + '-suffix')
        cls.substring.mkdir()
        cls.same_basename = cls.outside / Path(FENCE).name
        cls.same_basename.mkdir()
        cls.link_in = cls.outside / 'link-in'
        cls.link_in.symlink_to(cls.second, target_is_directory=True)
        (cls.repo / 'source').mkdir()
        (cls.repo / 'media').mkdir()
        png(cls.repo / 'media' / 'sample.png')
        cls.link = cls.work / 'link-out'
        cls.link.symlink_to(cls.repo, target_is_directory=True)
        cls.dangling = cls.work / 'dangling'
        cls.dangling.symlink_to(cls.outside / 'absent', target_is_directory=True)
        (cls.output / 'fixture-manifest.json').write_text(json.dumps({
            'outside': str(cls.outside), 'allowed': str(cls.allowed), 'second': str(cls.second),
            'binary': str(BINARY), 'build': build_id(BINARY), 'repo_before': snapshot(cls.repo)}, indent=2))
        cls.case_number = 0
        cls.attempts = []

    @classmethod
    def tearDownClass(cls):
        (cls.output / 'attempts.json').write_text(json.dumps(cls.attempts, indent=2))

    def start(self, **kwargs):
        type(self).case_number += 1
        profile = self.work / f'profile-{self.case_number}'
        app = App(BINARY, self.allowed, profile, env=self.env,
                  log=self.output / f'app-{self.case_number}.log', **kwargs)
        self.addCleanup(app.close)
        app.wait_albums()
        app.op('save_ui')
        time.sleep(.4)
        self.assertEqual(app.op('state')['root'], str(self.allowed))
        return app, profile

    def unchanged(self, app, profile, operation, **params):
        before = snapshot(self.outside)
        active = app.op('state')['root']
        saved = snapshot(profile)
        # Core housekeeping in profile may continue; the UI state must retain its identity and bytes.
        ui_before = saved.get('ui-state.json')
        result = app.op(operation, check=False, **params)
        self.assertFalse(result['ok'], result)
        self.assertIn('fence', result['error'].lower())
        time.sleep(.45)
        self.assertEqual(app.op('state')['root'], active)
        self.assertEqual(snapshot(profile).get('ui-state.json'), ui_before)
        after = snapshot(self.outside)
        if after != before:
            (self.output / ('outside-drift-' + str(time.time_ns()) + '.json')).write_text(json.dumps({'operation':operation,'params':params,'activeRoot':active,'outside':str(self.outside),'before':before,'after':after},indent=2))
        self.assertEqual(after, before)
        self.attempts.append({'op': operation, 'params': params, 'refusal': result,
                              'active_root': active, 'outside_before': before, 'outside_after': after})
        return result

    def test_01_dialog_entries_and_last_gate(self):
        app, profile = self.start()
        self.assertEqual(app.op('dialog_start'), str(self.allowed))
        self.assertEqual(app.op('dialog_start', decoy=True), str(self.allowed))
        for kind in ('root', 'startup', 'decoy'):
            with self.subTest(kind=kind):
                self.unchanged(app, profile, 'choose_root', kind=kind, path=str(self.repo))
        self.unchanged(app, profile, 'open_root', path=str(self.repo))
        for path in (self.link, self.substring, self.same_basename, self.outside, self.dangling, self.work / 'missing-root',
                     str(self.allowed) + '/' + os.path.relpath(self.repo, self.allowed), '', 'relative-root'):
            with self.subTest(path=str(path)):
                self.unchanged(app, profile, 'choose_root', kind='root', path=str(path))

    def test_02_gateway_nested_roots_and_directories(self):
        app, profile = self.start()
        for method, params in (
            ('get_albums_detached', {'rootDir': str(self.repo)}),
            ('canonicalize_directory', {'directory': str(self.repo)}),
            ('get_media_page', {'query': {'rootDir': str(self.repo)}}),
            ('get_albums_detached', {'rootDir': str(self.link)}),
            ('get_albums_detached', {'rootDir': str(self.substring)}),
            ('get_albums_detached', {'rootDir': str(self.same_basename)}),
            ('get_albums_detached', {'rootDir': str(self.outside)}),
            ('get_media_page', {'query': {'rootDir': str(self.same_basename)}}),
            ('unknown', {'root': 'sample.png'}),
            ('duplicate_group_members', {'root': 'sample.png'}),
            ('duplicate_group_members', {'dir': str(self.repo), 'root': 'sample.png'}),
            ('unknown', {'paths': 'not-an-array'}),
            ('unknown', {'decoyRoot': str(self.repo)}),
            ('unknown', {'plan': {'install': str(self.repo)}}),
            ('add_media_files', {'dir': str(self.repo), 'files': []}),
        ):
            with self.subTest(method=method, params=params):
                self.unchanged(app, profile, 'core_call', method=method, params=params)
        self.unchanged(app, profile, 'import_prepare', paths=[str(self.repo / 'sample.png')])

    def test_03_decoy_switch_and_clear(self):
        app, profile = self.start()
        decoy = self.work / 'retargeted-decoy'
        decoy.mkdir()
        app.op('choose_root', kind='decoy', path=str(decoy))
        app.op('save_ui')
        time.sleep(.4)
        decoy.rename(self.work / 'original-decoy')
        decoy.symlink_to(self.repo, target_is_directory=True)
        self.unchanged(app, profile, 'switch_decoy')
        app.op('clear_decoy')
        self.assertEqual(app.op('state')['root'], str(self.allowed))
        app.op('choose_root', kind='root', path=str(self.allowed))
        app.op('choose_root', kind='decoy', path=str(self.second))
        app.op('switch_decoy')
        self.assertEqual(app.op('state')['root'], str(self.second))
        moved = self.work / 'temporarily-moved-root'
        self.allowed.rename(moved)
        self.allowed.symlink_to(self.repo, target_is_directory=True)
        try:
            self.unchanged(app, profile, 'clear_decoy')
        finally:
            self.allowed.unlink()
            moved.rename(self.allowed)
        app.op('clear_decoy')
        app.wait_albums()
        self.assertEqual(app.op('state')['root'], str(self.allowed))

    def test_04_saved_profile_runtime_reload(self):
        for field in ('rootDir', 'decoyRoot'):
            with self.subTest(field=field):
                app, profile = self.start()
                file = profile / 'ui-state.json'
                state = json.loads(file.read_text())
                state[field] = str(self.repo)
                file.write_text(json.dumps(state))
                self.unchanged(app, profile, 'reload_ui')
                before = file.read_bytes()
                app.op('save_ui')
                self.assertEqual(file.read_bytes(), before)
                app.close()

    def test_05_saved_and_legacy_profiles_preflight(self):
        for field in ('rootDir', 'decoyRoot'):
            for legacy in (False, True):
                with self.subTest(field=field, legacy=legacy):
                    profile = self.work / f'preflight-{field}-{legacy}'
                    profile.mkdir()
                    argv = ["--legacy-profile", str(self.empty_legacy)]
                    state = {field: str(self.repo)}
                    target = profile
                    if legacy:
                        target = self.work / f'legacy-{field}'
                        target.mkdir()
                        (target / 'state.json').write_text(json.dumps({'room237-storage': json.dumps(state)}))
                        (target / 'settings.json').write_text('{}')
                        argv = ['--legacy-profile', str(target)]
                    else:
                        (profile / 'ui-state.json').write_text(json.dumps(state))
                    before, saved, old = snapshot(self.outside), snapshot(profile), snapshot(target)
                    run = subprocess.run([str(BINARY), '--root', str(self.allowed), '--profile', str(profile), *argv],
                                         env=dict(os.environ, **self.env, ROOM237_QA_PORT='1', ROOM237_QA_FENCE=FENCE),
                                         capture_output=True, text=True, timeout=20)
                    self.assertEqual(run.returncode, 2, run.stderr)
                    self.assertIn('refused saved profile roots before core startup', run.stderr)
                    self.assertEqual(snapshot(profile), saved)
                    self.assertEqual(snapshot(target), old)
                    self.assertEqual(snapshot(self.outside), before)
                    self.attempts.append({'preflight': field, 'legacy': legacy, 'returncode': run.returncode,
                                          'stderr': run.stderr, 'outside_before': before, 'outside_after': snapshot(self.outside)})

        # Presence of the QA variable is sufficient: an empty port must not disable the fence.
        profile = self.work / 'empty-port-profile'
        profile.mkdir()
        before = snapshot(self.outside)
        run = subprocess.run([str(BINARY), '--root', str(self.repo), '--profile', str(profile), '--legacy-profile', str(self.empty_legacy)],
                             env=dict(os.environ, **self.env, ROOM237_QA_PORT='', ROOM237_QA_FENCE=FENCE),
                             capture_output=True, text=True, timeout=20)
        self.assertEqual(run.returncode, 2, run.stderr)
        self.assertEqual(snapshot(self.outside), before)
        self.assertEqual(list(profile.iterdir()), [])

    def test_06_second_root_positive_control(self):
        app, profile = self.start()
        before = snapshot(self.outside)
        app.op('choose_root', kind='root', path=str(self.second))
        app.wait_albums()
        self.assertEqual(app.op('state')['root'], str(self.second))
        albums = app.op('state')['albums']
        self.assertTrue(albums)
        app.op('activate', album=albums[0]['id'])
        app.wait_idle()
        app.op('save_ui')
        self.assertEqual(json.loads((profile / 'ui-state.json').read_text())['rootDir'], str(self.second))
        files = [str(p.relative_to(self.second)) for p in self.second.rglob('*')]
        self.assertTrue(any('lock' in p for p in files), files)
        self.assertTrue(any('thumb' in str(p) and p.suffix == '.webp' and p.stat().st_size > 0 for p in self.second.rglob('*') if p.is_file()), files)
        self.assertEqual(snapshot(self.outside), before)
        self.attempts.append({'positive_root': str(self.second), 'created': files})

    def test_07_startup_positive_and_reset(self):
        app, profile = self.start()
        app.op('choose_root', kind='startup', path=str(self.allowed))
        app.wait_albums()
        app.op('reset_root')
        self.assertEqual(app.op('state')['root'], str(self.allowed))  # CLI root is revalidated after reset.
        self.assertEqual(app.op('dialog_start'), str(self.allowed))
        moved = self.work / 'no-valid-dialog-root'
        self.allowed.rename(moved)
        try:
            self.assertEqual(app.op('dialog_start'), '')
            self.assertEqual(app.op('dialog_start', decoy=True), '')
        finally:
            moved.rename(self.allowed)

    def test_08_canonical_reply_is_checked(self):
        import shutil
        folder = self.work / 'canonical-reply-stub'
        folder.mkdir()
        binary = folder / BINARY.name
        if BINARY.parent.name == "MacOS" and BINARY.parent.parent.name == "Contents":
            bundle = folder / "Probe.app"
            subprocess.run(["/bin/cp", "-cR", str(BINARY.parents[2]), str(bundle)], check=True)
            binary = bundle / "Contents/MacOS" / BINARY.name
        elif (BINARY.parent / 'lib').is_dir():
            closure = folder / 'Probe-root'
            subprocess.run(['cp', '-a', '--reflink=auto', str(BINARY.parent), str(closure)], check=True)
            binary = closure / BINARY.name
        else:
            shutil.copy2(BINARY, binary)
        core = binary.parent / 'room237-core'
        core.write_text("#!/usr/bin/env python3\n" +
            "import sys,json,os\n" +
            "for line in sys.stdin:\n" +
            " r=json.loads(line); m=r['method']\n" +
            " if m=='retry_startup': print(json.dumps({'event':'startup-status','payload':{'phase':'ready'}}),flush=True)\n" +
            " v=os.environ['FENCE_TEST_CANONICAL_REPLY'] if m=='canonicalize_directory' else [] if m=='get_albums_detached' else False if m=='is_preloading' else {}\n" +
            " print(json.dumps({'id':r['id'],'result':v}),flush=True)\n")
        core.chmod(0o755)
        profile = self.work / 'canonical-reply-profile'
        app = App(binary, self.allowed, profile, env=dict(self.env, FENCE_TEST_CANONICAL_REPLY=str(self.repo)),
                  log=self.output / 'canonical-reply.log')
        self.addCleanup(app.close)
        app.wait_albums()
        app.op('save_ui')
        time.sleep(.4)
        self.unchanged(app, profile, 'choose_root', kind='startup', path=str(self.allowed))

    def test_09_confirmation_contract(self):
        for scenario in ('match', 'mismatch', 'read_failure', 'type_select_drift', 'readback_mismatch'):
            with self.subTest(scenario=scenario):
                calls = {'read': 0, 'confirm': 0, 'cancel': 0, 'readback': 0}
                def read():
                    calls['read'] += 1
                    if scenario == 'read_failure':
                        raise OSError('reader failed')
                    return self.repo if scenario == 'type_select_drift' else self.second if scenario == 'mismatch' else self.allowed
                def confirm():
                    calls['confirm'] += 1
                def cancel():
                    calls['cancel'] += 1
                def active():
                    calls['readback'] += 1
                    return self.second if scenario == 'readback_mismatch' else self.allowed
                if scenario == 'match':
                    self.assertEqual(confirm_native_directory(read, self.allowed, confirm, cancel, active), self.allowed)
                else:
                    with self.assertRaises((ValueError, OSError, RuntimeError)):
                        confirm_native_directory(read, self.allowed, confirm, cancel, active)
                accepted = scenario in ('match', 'readback_mismatch')
                self.assertEqual(calls, {'read': 1, 'confirm': int(accepted), 'cancel': int(not accepted), 'readback': int(accepted)})

    def test_10_prewrite_scope(self):
        destination = self.allowed / 'new-file'
        self.assertEqual(scope_check(destination, [self.allowed]), destination)
        for path in (self.repo, self.link / 'new-file', self.substring, self.dangling, 'relative', ''):
            with self.subTest(path=str(path)), self.assertRaises((ValueError, OSError)):
                scope_check(path, [self.allowed])
        with self.assertRaises(ValueError):
            scope_check(self.second / 'new-file', [self.allowed])

    def test_11_gateway_path_values_still_refuse_escapes(self):
        app, profile = self.start()
        duplicate_params = {'dir': str(self.allowed / 'album'), 'signature': '', 'offset': 0}
        for root in (str(self.repo), '../x', '..', '', '.', 'nested\\sample.png'):
            with self.subTest(root=root):
                result = self.unchanged(app, profile, 'core_call', method='duplicate_group_members',
                                        params=dict(duplicate_params, root=root))
                self.assertEqual(result['error'], 'QA fence refused core parameters')
        for path in (self.repo / 'missing.png', self.link / 'missing.png'):
            with self.subTest(path=str(path)):
                self.assertFalse(path.exists())
                result = self.unchanged(app, profile, 'core_call', method='set_media_favorite',
                                        params={'path': str(path), 'favorite': True})
                self.assertEqual(result['error'], 'QA fence refused core parameters')

    def test_12_gateway_bare_root_and_missing_fenced_leaf(self):
        app, _ = self.start()
        missing = self.allowed / 'album' / 'missing.png'
        self.assertFalse(missing.exists())
        for method, params in (
            ('duplicate_group_members', {'dir': str(self.allowed / 'album'), 'root': 'sample.png',
                                         'signature': '', 'offset': 0}),
            ('mark_duplicate_group_different', {'dir': str(self.allowed / 'album'), 'root': 'sample.png',
                                               'signature': ''}),
            ('set_media_favorite', {'path': str(missing), 'favorite': True}),
        ):
            with self.subTest(method=method):
                before = snapshot(self.outside)
                result = app.op('core_call', check=False, method=method, params=params)
                # A core error is allowed; the gateway must let these requests reach it.
                self.assertNotIn('QA fence refused core parameters', result.get('error', ''))
                self.assertEqual(app.op('state')['root'], str(self.allowed))
                after = snapshot(self.outside)
                self.assertEqual(after, before)
                self.attempts.append({'op': 'core_call', 'params': {'method': method, 'params': params},
                                      'fence_allowed_reply': result, 'outside_before': before, 'outside_after': after})

    def test_13_root_selection_still_requires_existing_directory(self):
        app, profile = self.start()
        missing = self.allowed / 'missing-root'
        self.assertFalse(missing.exists())
        for kind in ('root', 'startup', 'decoy'):
            with self.subTest(kind=kind):
                self.unchanged(app, profile, 'choose_root', kind=kind, path=str(missing))
        self.unchanged(app, profile, 'open_root', path=str(missing))

    def test_14_absolute_boundary_and_link_in_positive(self):
        app, _ = self.start()
        self.assertEqual(require_fenced(self.link_in), self.second)
        app.op('choose_root', kind='root', path=str(self.link_in))
        self.assertEqual(app.op('state')['root'], str(self.second))
        for bad_fence in (Path(FENCE).name, '', '/', str(Path(FENCE) / 'missing-boundary')):
            with self.subTest(boundary=bad_fence):
                run = subprocess.run([str(BINARY), '--root', str(self.allowed), '--profile', str(self.work / 'bad-boundary'), '--legacy-profile', str(self.empty_legacy)],
                    env=dict(os.environ, **self.env, ROOM237_QA_PORT='1', ROOM237_QA_FENCE=bad_fence),
                    capture_output=True, text=True, timeout=15)
                self.assertEqual(run.returncode, 2, run.stderr)

    def test_15_updater_and_native_unowned_refusal(self):
        app, profile = self.start()
        for method in ('update_stage', 'update_launch_helper', 'update_cleanup', 'update_check'):
            self.unchanged(app, profile, 'core_call', method=method, params={})
        response = app.op('native_directory_panel', check=False, action='read')
        self.assertFalse(response['ok'])
        self.assertIn('no QA-owned' if sys.platform=='darwin' else 'macOS only', response['error'])
        response = app.op('native_directory_open', check=False, kind='root')
        self.assertFalse(response['ok'])
        self.assertIn('already-active owned cocoa' if sys.platform=='darwin' else 'macOS only', response['error'])


if __name__ == '__main__':
    BINARY = Path(sys.argv.pop(1)).resolve(strict=True) if len(sys.argv) > 1 else None
    WORK = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else None
    OUTPUT = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else None
    unittest.main(verbosity=2)
