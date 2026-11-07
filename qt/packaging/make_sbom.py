#!/usr/bin/env python3
"""make_sbom.py CARGO_LOCK OUT_JSON [NAME=VERSION=LICENSE ...]. Uses locked Cargo metadata."""
import json
import os
import subprocess
import sys
import tomllib
from pathlib import Path
lock, out = map(Path, sys.argv[1:3])
packages = tomllib.loads(lock.read_text())["package"]
result = subprocess.run(['cargo', 'metadata', '--locked', '--format-version=1', '--manifest-path', str(lock.with_name('Cargo.toml'))], capture_output=True, text=True)
if result.returncode:
    sys.exit('cargo metadata --locked failed:\n' + result.stderr)
metadata = {(p['name'],p['version']):p for p in json.loads(result.stdout)['packages']}
components=[]
for p in packages:
    if not p.get('source'):
        continue
    m=metadata.get((p['name'],p['version']),{})
    c={'type':'library','name':p['name'],'version':p['version'],'purl':f"pkg:cargo/{p['name']}@{p['version']}"}
    if m.get('license'):
        c['licenses']=[{'expression':m['license']}]
    elif m.get('license_file'):
        license_path=Path(m['manifest_path']).parent/m['license_file']
        c['licenses']=[{'license':{'name':'license-file', 'text':{'content':license_path.read_text()}}}]
    else:
        sys.exit(f"Missing license metadata: {p['name']} {p['version']}")
    if p.get('checksum'):
        c['hashes']=[{'alg':'SHA-256','content':p['checksum']}]
    components.append(c)
for extra in sys.argv[3:]:
    name,version,license_id=extra.split('=',2)
    components.append({'type':'library','name':name,'version':version,'licenses':[{'license':{'name':license_id}}]})
out.write_text(json.dumps({'bomFormat':'CycloneDX','specVersion':'1.5','version':1,'components':components},indent=1)+'\n')
print(len(components))
