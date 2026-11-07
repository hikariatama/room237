#!/usr/bin/env python3
"""Actual packaged Rust-core protocol smoke, separate from Qt; no GUI."""
import json, os, subprocess, sys
from pathlib import Path
from qa import require_fenced, build_id
binary, work = Path(sys.argv[1]), Path(sys.argv[2])
require_fenced(work); work.mkdir(parents=True, exist_ok=False)
root = work/'root'; root.mkdir()
requests = [dict(id=1,method='canonicalize_directory',params={'directory':str(root)}),dict(id=2,method='get_settings',params={})]
run = subprocess.run([str(binary),'--profile',str(work/'profile'),'--legacy-profile',str(work/'legacy')],
    cwd=work, input=''.join(json.dumps(r)+'\n' for r in requests),capture_output=True,text=True,timeout=60)
(work/'stdout.ndjson').write_text(run.stdout); (work/'stderr.log').write_text(run.stderr)
rows = [json.loads(line) for line in run.stdout.splitlines() if line.startswith('{')]
replies = {r.get('id'):r for r in rows if 'id' in r}
results = [dict(name='canonical root',ok=replies.get(1,{}).get('result')==str(root)),dict(name='settings',ok=isinstance(replies.get(2,{}).get('result'),dict)),dict(name='clean exit',ok=run.returncode==0)]
(work/'results.json').write_text(json.dumps(dict(build=build_id(binary),results=results),indent=2))
raise SystemExit(not all(r['ok'] for r in results))
