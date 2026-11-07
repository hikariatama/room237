#!/usr/bin/env python3
"""Six real NSOpenPanel cases. Requires an authorized, unused screen slot; never used offscreen."""
import argparse
import json
import os
from pathlib import Path
import time
from qa import App, NativeDirectoryPanel, guard, window_id
from slot_contract import authorization, fresh_work, activate, sha

CASES = ('expected', 'wrong', 'same-basename', 'outward-symlink', 'cancel', 'decoy')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('work', type=Path)
    parser.add_argument('--authorization-file', required=True, type=Path)
    args = parser.parse_args()
    authorization(args.authorization_file, args.binary)
    os.environ['ROOM237_SLOT_AUTHORIZATION'] = str(args.authorization_file.resolve())
    work = fresh_work(args.work)
    rows = []
    for case in CASES:
        navigation = None
        task = work / case
        root, chosen, wrong = (task / x for x in ('root', 'expected/Album', 'wrong/Other'))
        sibling=task/'sibling/Album'; sibling.mkdir(parents=True)
        for path in (root, chosen, wrong): path.mkdir(parents=True)
        profile = task / 'profile'
        app = App(args.binary, root, profile, log=task/'app.log')
        panel = None
        try:
            app.wait_albums()
            target = activate(app)  # Exactly once per case, through this app's QA operation.
            guard(app.proc.pid, window=target)
            app.op('save_ui')
            before = (profile/'ui-state.json').read_bytes()
            oldroot = app.op('state')['root']
            # The panel opens at the case's folder (the backend cannot observe programmatic navigation of a shown panel);
            # the outward symlink still goes through 'select', whose fence refuses it, and 'cancel' needs no folder.
            start = {'expected': chosen, 'decoy': chosen, 'wrong': wrong, 'same-basename': sibling}.get(case)
            opened = app.op('native_directory_open', kind='decoy' if case == 'decoy' else 'root', **({'start': str(start)} if start else {}))
            time.sleep(.4)
            panel = NativeDirectoryPanel(app, opened['session'])
            guard(app.proc.pid, window=panel.binding['window'])
            if case == 'cancel':
                panel.cancel()
                assert app.proc.poll() is None and app.op('state')['root'] == oldroot
                assert str(window_id(app.proc.pid)) == str(target)
            else:
                selected = wrong if case=='wrong' else sibling if case=='same-basename' else chosen
                if case == 'outward-symlink':
                    # Negative destination is an owned sibling of this case's fence, never user data.
                    outside = Path(os.environ['ROOM237_QA_NATIVE_NEGATIVE_ROOT']) / case
                    outside.mkdir(parents=True, exist_ok=False)
                    selected = task / 'link-out'
                    selected.symlink_to(outside, target_is_directory=True)
                result = app.op('native_directory_panel', action='select', session=panel.session,
                                token=panel.binding['token'], expected=str(selected), check=False)
                refused = not result['ok']
                navigation = result.get('result') if result['ok'] else {'error': result.get('error')}
                if case in ('expected', 'decoy') and not (navigation or {}).get('settled'):
                    raise AssertionError('panel did not report the requested folder: ' + json.dumps(navigation))
                if case in ('wrong','same-basename','outward-symlink'):
                    if not refused:
                        try: panel.confirm_expected(chosen)
                        except (ValueError, RuntimeError): refused = True
                    assert refused, 'negative selection unexpectedly accepted'
                    if not panel.closed: panel.cancel()
                elif case == 'decoy':
                    assert panel.read_selected_path() == chosen.resolve()
                    panel.confirm(chosen)
                    # The decoy root is applied after the modal dialog returns; bounded readback.
                    deadline = time.monotonic() + 10
                    while True:
                        app.op('save_ui')
                        saved = json.loads((profile/'ui-state.json').read_text())
                        if saved.get('decoyRoot') == str(chosen.resolve()) or time.monotonic() > deadline: break
                        time.sleep(0.1)
                    assert saved['decoyRoot'] == str(chosen.resolve())
                    assert app.op('state')['root'] == oldroot
                else:
                    assert panel.confirm_expected(chosen) == chosen.resolve()
                    app.wait_albums()
                    assert app.op('state')['root'] == str(chosen.resolve())
            if case not in ('expected','decoy'):
                app.op('save_ui')
                assert app.op('state')['root'] == oldroot
                assert (profile/'ui-state.json').read_bytes() == before, 'saved profile changed on refusal'
            rows.append(dict(name=case, ok=True, pid=app.proc.pid, mainWindow=target,
                             panelWindow=panel.binding['window'], navigation=locals().get('navigation'), confirmation=getattr(panel, 'confirmation', None)))
        except BaseException as error:
            # Interruptions (SIGINT from the runner) are recorded too, so results.json always exists.
            rows.append(dict(name=case, ok=False, error=repr(error), pid=app.proc.pid, navigation=locals().get('navigation'), confirmation=getattr(panel, 'confirmation', None) if panel else None))
            raise
        finally:
            if panel and not panel.closed:
                panel.cancel()
            app.close()
            if rows and rows[-1].get('name') == case: rows[-1]['exitCode'] = app.exit_code
            (work/'results.json').write_text(json.dumps({'binarySha256':sha(args.binary),'results':rows}, indent=2))
    return 0
if __name__ == '__main__': raise SystemExit(main())
