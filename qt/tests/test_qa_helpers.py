#!/usr/bin/env python3
"""Synthetic/helper contract tests. These do NOT verify a live native panel.
Usage: test_qa_helpers.py QTCORE_FENCE_PROBE NEW_WORK.
"""
import json, os, subprocess, sys, unittest
from pathlib import Path
from types import SimpleNamespace
from qa import FENCE, NativeDirectoryPanel, canonical_absolute, confirm_native_directory, require_fenced
PROBE, WORK = sys.argv[1:3]
sys.argv = [sys.argv[0]]
require_fenced(WORK)
WORK = Path(WORK)
WORK.mkdir(parents=True, exist_ok=False)
ALLOWED = WORK / 'root'
ALLOWED.mkdir()
SECOND = WORK / 'second'
SECOND.mkdir()
OUTSIDE = Path(os.environ['ROOM237_QA_HELPER_OUTSIDE'])
OUTSIDE.mkdir(parents=True, exist_ok=False)

class Helpers(unittest.TestCase):
    def probe(self, expected, **request):
        run = subprocess.run([PROBE], input=json.dumps(request)+'\n', capture_output=True,
                             text=True, timeout=float(os.environ.get("ROOM237_QA_HELPER_TIMEOUT", "30")), env=dict(os.environ, ROOM237_QA_PORT='1'))
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout.strip(), json.dumps(expected), request)

    def test_absolute_containment_actual_cpp_and_python(self):
        same = OUTSIDE / Path(FENCE).name
        same.mkdir()
        inward = OUTSIDE / 'link-in'
        inward.symlink_to(ALLOWED, target_is_directory=True)
        outward = WORK / 'link-out'
        outward.symlink_to(OUTSIDE, target_is_directory=True)
        dangling = WORK / 'dangling'
        dangling.symlink_to(OUTSIDE / 'absent')
        file = ALLOWED / 'file'
        file.write_text('fixture')
        gone = ALLOWED / 'vanished'
        gone.write_text('fixture')
        gone.unlink()
        cases = [(ALLOWED, False, True), (Path(FENCE), False, True), (inward, False, True),
                 (same, False, False), (OUTSIDE, False, False), (str(ALLOWED)+'/../root', False, False),
                 (outward, False, False), (outward/'missing', True, False), (dangling/'missing', True, False),
                 (ALLOWED/'new'/'deep', True, True), (gone, True, True), (gone, False, False),
                 (file/'bad-child', True, False), ('relative', True, False), ('', True, False)]
        for path, missing, ok in cases:
            with self.subTest(path=str(path), missing=missing):
                self.probe(ok, op='inside', path=str(path), fence=FENCE, missing=missing)
                # require_fenced is the prewrite variant; existence of a root is a separate precondition.
                if ok:
                    self.assertEqual(require_fenced(path), Path(path).resolve())
                elif path != gone:
                    with self.assertRaises((ValueError, OSError)):
                        require_fenced(path)
        for boundary in ('', Path(FENCE).name, '/', str(ALLOWED/'missing'), str(ALLOWED)+'/..'):
            self.probe(False, op='inside', path=str(ALLOWED), fence=boundary, missing=True)

    def test_saved_roots_and_ipc_actual_cpp(self):
        for field in ('rootDir', 'decoyRoot'):
            self.probe(False, op='roots', state={field:str(OUTSIDE)})
            self.probe(True, op='roots', state={field:str(ALLOWED)})
        for key in ('rootDir','decoyRoot','directory','dir','root','albumPath','source','target',
                    'sourcePath','path','currentPath','destination','payload','install','probe','staged',
                    'backup','healthFile','resultFile'):
            self.probe(False, op='params', method='arbitrary', params={'nested':{key:str(OUTSIDE/'missing')}})
        for params in ({'paths':[str(OUTSIDE)]}, {'paths':'malformed'}, {'paths':[None]}):
            self.probe(False, op='params', method='arbitrary', params=params)
        for method in ('duplicate_group_members','mark_duplicate_group_different'):
            self.probe(True, op='params', method=method, params={'dir':str(ALLOWED),'root':'image.png'})
            self.probe(False, op='params', method=method, params={'root':'image.png'})
            self.probe(False, op='params', method=method, params={'dir':str(OUTSIDE),'root':'image.png'})
        self.probe(False, op='params', method='other', params={'dir':str(ALLOWED),'root':'image.png'})
        self.probe(True, op='params', method='set_media_favorite', params={'path':str(ALLOWED/'vanished')})
        self.probe(True, op='params', method='update_settings', params={'settings':{}})
        for method in ('update_stage','update_launch_helper','update_cleanup','update_check'):
            self.probe(False, op='params', method=method, params={})

    def test_confirmation_failures_once_no_retries(self):
        for scenario in ('match','unknown','ambiguous','outside','wrong','relative','read-error',
                         'confirm-error','readback-wrong','readback-error','disappeared'):
            with self.subTest(scenario=scenario):
                calls = []
                expected = ALLOWED
                def read():
                    calls.append('read')
                    if scenario == 'read-error': raise OSError('native error')
                    if scenario == 'disappeared': return ALLOWED/'missing'
                    return {'unknown':None, 'ambiguous':[ALLOWED,SECOND], 'outside':OUTSIDE,
                            'wrong':SECOND, 'relative':'root'}.get(scenario, ALLOWED)
                def confirm():
                    calls.append('confirm')
                    if scenario == 'confirm-error': raise RuntimeError('stale native ticket')
                def cancel(): calls.append('cancel')
                def active():
                    calls.append('readback')
                    if scenario == 'readback-error': raise OSError('app closed')
                    return SECOND if scenario == 'readback-wrong' else ALLOWED
                if scenario == 'match':
                    self.assertEqual(confirm_native_directory(read,expected,confirm,cancel,active),ALLOWED)
                    self.assertEqual(calls,['read','confirm','readback'])
                else:
                    with self.assertRaises((ValueError, OSError, RuntimeError, TypeError)):
                        confirm_native_directory(read,expected,confirm,cancel,active)
                    self.assertEqual(calls, ['read','confirm','readback'] if scenario.startswith('readback') else
                                     ['read','confirm','cancel'] if scenario == 'confirm-error' else ['read','cancel'])

    def test_bind_failure_cancels_only_matching_session(self):
        for scenario in ('unknown', 'bad-pid'):
            class Fake:
                proc = SimpleNamespace(pid=123)
                def __init__(self): self.calls=[]
                def op(self, operation, **kw):
                    self.calls.append((operation,kw))
                    if operation=='native_directory_abort': return {'cancelled':True}
                    if scenario=='unknown': raise RuntimeError('no native panel URL service')
                    return {'pid':999,'window':456,'token':'owned'}
            app=Fake()
            with self.assertRaises(RuntimeError): NativeDirectoryPanel(app,'owned-session')
            self.assertEqual(app.calls[-1],('native_directory_abort',{'session':'owned-session'}))
            self.assertEqual(len(app.calls),2)

    def test_adapter_protocol_scope(self):
        for scenario in ('match','wrong-pid','wrong-window','stale-token','missing-path','read-error','confirm-error'):
            with self.subTest(scenario=scenario):
                class FakeApp:
                    proc = SimpleNamespace(pid=123)
                    def __init__(self): self.calls=[]
                    def op(self, operation, **kw):
                        self.calls.append((operation,kw))
                        if operation == 'state': return {'root':str(ALLOWED)}
                        self_outer.assertEqual(operation,'native_directory_panel')
                        self_outer.assertEqual(kw['session'],'owned-session')
                        action=kw['action']
                        bind={'pid':123,'window':456,'token':'owned-dialog'}
                        if action=='bind': return bind
                        self_outer.assertEqual(kw['token'],'owned-dialog')
                        if action=='read':
                            if scenario=='read-error': raise RuntimeError('no selected URLs')
                            result=dict(bind,path=str(ALLOWED))
                            if scenario=='wrong-pid': result['pid']=999
                            if scenario=='wrong-window': result['window']=789
                            if scenario=='stale-token': result['token']='other'
                            if scenario=='missing-path': result.pop('path')
                            return result
                        if action=='confirm':
                            if scenario=='confirm-error': raise RuntimeError('selection changed before press')
                            return {'confirmed':True}
                        if action=='cancel': return {'cancelled':True}
                self_outer=self
                app=FakeApp();panel=NativeDirectoryPanel(app,"owned-session")
                if scenario=='match':
                    self.assertEqual(panel.confirm_expected(ALLOWED),ALLOWED)
                    self.assertEqual([kw.get('action',op) for op,kw in app.calls],['bind','read','confirm','state'])
                else:
                    with self.assertRaises((RuntimeError,ValueError)):
                        panel.confirm_expected(ALLOWED)
                    self.assertEqual([kw.get('action',op) for op,kw in app.calls],
                                     ['bind','read','confirm','cancel'] if scenario=='confirm-error' else ['bind','read','cancel'])
                with self.assertRaises(RuntimeError): panel.read_selected_path()

if __name__=='__main__': unittest.main(verbosity=2)
