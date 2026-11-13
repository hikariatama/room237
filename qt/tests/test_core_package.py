#!/usr/bin/env python3
"""Packaged core smoke without the window: the update helper room237-core and the core protocol of the QA app.
Usage: test_core_package.py CORE_BINARY WORK. The app binary (Room237, room237 or Room237.exe) is CORE_BINARY's sibling."""
import json, os, subprocess, sys
from pathlib import Path
from qa import require_fenced, build_id
helper, work = Path(sys.argv[1]), Path(sys.argv[2])
require_fenced(work); work.mkdir(parents=True, exist_ok=False)
app = next(p for p in helper.parent.iterdir() if p.stem in ('Room237', 'room237') and p.is_file())
root = work/'root'; root.mkdir()
version = subprocess.run([str(helper), '--version'], capture_output=True, text=True, timeout=30)
requests = [dict(id=1,method='canonicalize_directory',params={'directory':str(root)}),dict(id=2,method='get_settings',params={})]
env = dict(os.environ); env.pop('ROOM237_QA_PORT', None)
run = subprocess.run([str(app),'--qa-core-server','--profile',str(work/'profile'),'--legacy-profile',str(work/'legacy')],
    cwd=work, env=env, input=''.join(json.dumps(r)+'\n' for r in requests),capture_output=True,text=True,timeout=60)
(work/'stdout.ndjson').write_text(run.stdout); (work/'stderr.log').write_text(run.stderr)
rows = [json.loads(line) for line in run.stdout.splitlines() if line.startswith('{')]
replies = {r.get('id'):r for r in rows if 'id' in r}
results = [dict(name='helper reports a version',ok=version.returncode==0 and bool(version.stdout.strip())),
           dict(name='canonical root',ok=replies.get(1,{}).get('result')==str(root)),
           dict(name='settings',ok=isinstance(replies.get(2,{}).get('result'),dict)),
           dict(name='clean exit',ok=run.returncode==0)]
(work/'results.json').write_text(json.dumps(dict(build=build_id(app),results=results),indent=2))
raise SystemExit(not all(r['ok'] for r in results))
