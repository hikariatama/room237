#!/usr/bin/env python3
"""Runs the offscreen suites against the packages of one release_driver.py work directory. No window is shown.

  run_offscreen.py --work WORK --platform macos --arch arm64 --qt QT_ROOT --fixtures DIR --out NEW_DIR [--suites ...]
  run_offscreen.py --work WORK --platform linux --kind tar|deb|appimage --qt QT_LINUX --fixtures DIR --out NEW_DIR

Every suite gets its own fence (ROOM237_QA_FENCE) with a synthetic HOME and TMPDIR under OUT. FIXTURES is the output of
gen_format_fixtures.py with its FIXTURES.expected.json beside it. OUT/matrix.json lists every suite with its command,
exit code and time; the exit status is 1 when a suite failed.
The suites run the QA variants of the packages (release_driver.py --qa: n/qa, n1/qa), which add the QA driver to the
otherwise identical build. On macOS the app is tested from a copy that only adds Qt's offscreen platform plugin (the
package does not ship it); the N+1 update payload gets the same plugin. Linux packages ship the plugin.
"""
import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import time
from pathlib import Path

TESTS = Path(__file__).resolve().parent
SUITES = ['core-package', 'qa-helpers', 'root-fence', 'isolation', 'external-album', 'listing-order', 'formats', 'functional',
          'near-duplicates', 'duplicate-positive', 'album-create-scope', 'fonts', 'migration', 'hardening', 'formats-encrypted', 'processing', 'update']


def clone(source, target):
    if platform.system() == 'Darwin':
        subprocess.run(['/bin/cp', '-cR', str(source), str(target)], check=True)
    else:
        shutil.copytree(source, target, symlinks=True)


def fence_env(case):
    fence = case / 'fence'
    (fence / 'home').mkdir(parents=True)
    (fence / 'tmp').mkdir()
    env = dict(os.environ, ROOM237_QA_FENCE=str(fence), HOME=str(fence / 'home'), CFFIXED_USER_HOME=str(fence / 'home'),
               XDG_CONFIG_HOME=str(fence / 'home/.config'), TMPDIR=str(fence / 'tmp') + '/', QT_QPA_PLATFORM='offscreen',
               PYTHONDONTWRITEBYTECODE='1', ROOM237_QA_NEGATIVE_ROOT=str(case / 'negative'), ROOM237_QA_HELPER_OUTSIDE=str(case / 'helper-outside'))
    for key in ('ROOM237_SLOT_AUTHORIZATION', 'ROOM237_QA_UPDATE_FIXTURE', 'ROOM237_QA_PORT', 'ROOM237_TRACE'):
        env.pop(key, None)
    return fence, env


class Matrix:
    def __init__(self, out):
        self.out, self.rows = out, []

    def save(self):
        counts = {s: sum(r['status'] == s for r in self.rows) for s in ('passed', 'failed', 'blocked')}
        (self.out / 'matrix.json').write_text(json.dumps({'counts': counts, 'rows': self.rows}, indent=2) + '\n')
        return counts

    def run(self, name, command, env, timeout, cwd=None):
        start = time.monotonic()
        log = self.out / f'{name}.log'
        with log.open('w') as handle:
            try:
                rc = subprocess.run([str(x) for x in command], env=env, cwd=cwd or self.out, stdout=handle, stderr=subprocess.STDOUT, timeout=timeout).returncode
                reason = ''
            except subprocess.TimeoutExpired:
                rc, reason = None, f'timeout after {timeout} s'
        if rc and not reason:
            lines = [l for l in log.read_text(errors='replace').splitlines() if l.startswith(('FAIL', 'ERROR', 'Traceback', 'AssertionError', 'RuntimeError'))]
            reason = '; '.join(lines[-5:])
        self.rows.append({'name': name, 'status': 'passed' if rc == 0 else 'failed', 'reason': reason, 'exitCode': rc,
                          'seconds': round(time.monotonic() - start, 1), 'command': [str(x) for x in command]})
        self.save()

    def block(self, name, reason):
        self.rows.append({'name': name, 'status': 'blocked', 'reason': reason})
        self.save()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--platform', choices=['macos', 'linux'], required=True)
    p.add_argument('--arch', choices=['arm64', 'x86_64'])
    p.add_argument('--kind', choices=['tar', 'deb', 'appimage'], default='tar')
    p.add_argument('--qt', type=Path, required=True, help='Qt used for the build (offscreen plugin, fence probe)')
    p.add_argument('--fixtures', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--suites', nargs='+', default=SUITES, choices=SUITES)
    p.add_argument('--jobs', default=os.environ.get('ROOM237_BUILD_JOBS', '4'))
    a = p.parse_args()
    work, fixtures, qt = a.work.resolve(strict=True), a.fixtures.resolve(strict=True), a.qt.resolve(strict=True)
    expected_json = fixtures.with_name(fixtures.name + '.expected.json')
    if not (work / 'COMPLETE').is_file() or not expected_json.is_file():
        p.error('need a completed work directory and FIXTURES.expected.json')
    if not (work / 'n/qa').is_dir() or not (work / 'n1/qa').is_dir():
        p.error('the work directory has no QA variants; build it with release_driver.py --qa')
    if a.platform == 'macos' and not a.arch:
        p.error('--arch is required on macOS')
    if a.out.exists():
        p.error('--out must be new')
    a.out.mkdir(parents=True)
    out = a.out.resolve()
    m = Matrix(out)
    py = sys.executable
    source = work / 'source'

    if a.platform == 'macos':
        app = out / 'package/Room237.app'
        app.parent.mkdir()
        clone(work / 'n/qa/macos' / a.arch / 'Room237.app', app)
        plugin_dir = app / 'Contents/PlugIns/platforms'
        plugin_dir.mkdir(parents=True, exist_ok=True)
        plugin = plugin_dir / 'libqoffscreen.dylib'
        subprocess.run(['lipo', str(qt / 'plugins/platforms/libqoffscreen.dylib'), '-thin', a.arch, '-output', str(plugin)], check=True)
        subprocess.run(['codesign', '--force', '--sign', '-', str(plugin)], check=True, capture_output=True)
        binary, bundle = app / 'Contents/MacOS/Room237', app
        core = app / 'Contents/MacOS/room237-core'
        update_platform = 'darwin-aarch64' if a.arch == 'arm64' else 'darwin-x86_64'
        payload_source = next((work / 'n1/qa/macos').glob(f'*macos-{a.arch}.update.tar.gz'))
        install_n = app
    else:
        install = out / 'package'
        install.mkdir()
        packages = work / 'n/qa/linux'
        if a.kind == 'appimage':
            a.suites = ['update']
            install_n = next(packages.glob('*.AppImage'))
            payload_source = next((work / 'n1/qa/linux').glob('*.AppImage'))
            update_platform = 'linux-appimage-x86_64'
        else:
            if a.kind == 'tar':
                with tarfile.open(next(packages.glob('*linux-x86_64.tar.gz'))) as tar:
                    tar.extractall(install, filter='data')
            else:
                subprocess.run(['dpkg-deb', '-x', str(next(packages.glob('*.deb'))), str(install)], check=True)
            binary = next(p for p in install.rglob('room237') if p.is_file() and os.access(p, os.X_OK))
            bundle, core, install_n = binary.parent, binary.parent / 'room237-core', binary.parent
            payload_source = next((work / 'n1/qa/linux').glob('*.update.tar.gz'))
            update_platform = 'linux-x86_64'

    probe = out / 'fence-probe/build/qa_fence_probe'
    if 'qa-helpers' in a.suites:
        _, env = fence_env(out / 'fence-probe')
        configure = ['cmake', '-S', TESTS / 'fence_probe', '-B', probe.parent, '-G', 'Ninja', f'-DCMAKE_PREFIX_PATH={qt}', f'-DROOM237_SOURCE_ROOT={source}']
        if os.environ.get('ROOM237_CORE_SOURCES'):
            configure.append(f"-DROOM237_CORE_SOURCES={os.environ['ROOM237_CORE_SOURCES']}")
        if a.platform == 'macos':
            configure.append(f'-DCMAKE_OSX_ARCHITECTURES={a.arch}')
        m.run('fence-probe-configure', configure, env, 300)
        m.run('fence-probe-build', ['cmake', '--build', probe.parent, '-j', a.jobs], env, 600)

    for name in a.suites:
        case = out / name
        fence, env = fence_env(case)
        fixture = fence / 'fixtures'
        shutil.copytree(fixtures, fixture)
        expected = fence / 'formats.expected.json'
        shutil.copy2(expected_json, expected)
        if a.platform == 'linux':
            env.update(LIBGL_ALWAYS_SOFTWARE='1', QT_MEDIA_BACKEND='ffmpeg')
            if a.kind != 'appimage':
                env['LD_LIBRARY_PATH'] = str(bundle / 'lib')
        if a.arch == 'x86_64' and platform.machine() == 'arm64':
            env['ROOM237_TEST_ARCH'] = 'x86_64'
        w = fence / 'work'
        if name == 'update':
            payload = payload_source
            if a.platform == 'macos':
                staged = case / 'payload'
                staged.mkdir()
                with tarfile.open(payload_source) as tar:
                    tar.extractall(staged, filter='data')
                platforms = staged / 'Contents/PlugIns/platforms'
                platforms.mkdir(parents=True, exist_ok=True)
                shutil.copy2(plugin, platforms / plugin.name)
                payload = case / 'offscreen-n1.update.tar.gz'
                with tarfile.open(payload, 'w:gz') as tar:
                    tar.add(staged, arcname='.')
            command = [py, TESTS / 'test_update.py', update_platform, w, '-', source / 'qt/packaging/sign_update.py', install_n, payload, '--fixture', fixture / 'plain.jpg']
        elif name == 'migration':
            if a.platform == 'linux':
                command = [py, TESTS / 'test_legacy_discovery.py', binary, w, '--offscreen-profile', TESTS / 'fixtures/tauri-profile/dev.dgazizullin.room237.qa.base']
            else:
                command = [py, TESTS / 'test_migration.py', binary, fixture, w]
        else:
            command = {
                'core-package': [py, TESTS / 'test_core_package.py', core, w],
                'qa-helpers': [py, TESTS / 'test_qa_helpers.py', probe, w],
                'root-fence': [py, TESTS / 'test_root_fence.py', binary, w, fence / 'evidence'],
                'isolation': [py, TESTS / 'test_isolation.py', binary, w],
                'external-album': [py, TESTS / 'test_external_album.py', binary, w],
                'listing-order': [py, TESTS / 'test_listing_order.py', bundle, w],
                'formats': [py, TESTS / 'test_formats.py', binary, fixture, expected, w],
                'functional': [py, TESTS / 'test_functional.py', binary, fixture, w],
                'near-duplicates': [py, TESTS / 'test_near_duplicates.py', binary, w],
                'duplicate-positive': [py, TESTS / 'test_duplicate_positive.py', binary, w],
                'album-create-scope': [py, TESTS / 'test_album_create_scope.py', binary, w],
                'fonts': [py, TESTS / 'test_fonts.py', binary, w],
                'hardening': [py, TESTS / 'test_hardening.py', binary, w],
                'processing': [py, TESTS / 'test_processing.py', binary, w],
                'formats-encrypted': [py, TESTS / 'test_formats.py', binary, fixture, expected, w, '--encrypted'],
            }[name]
            if name in ('formats', 'formats-encrypted') and a.platform == 'linux':
                command = ['xvfb-run', '-a', '-s', '-screen 0 1440x900x24 -nolisten tcp', *command]
                env['QT_QPA_PLATFORM'] = 'xcb'
        if name == 'qa-helpers' and not probe.is_file():
            m.block(name, 'fence probe did not build')
            continue
        m.run(name, command, env, 1500 if name == 'update' else 900)
        shutil.rmtree(fixture, ignore_errors=True)
    counts = m.save()
    print(json.dumps(counts))
    return 1 if counts['failed'] else 0


if __name__ == '__main__':
    sys.exit(main())
