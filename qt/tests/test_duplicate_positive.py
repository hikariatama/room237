#!/usr/bin/env python3
"""Exact duplicate positive and same-stem/different-extension inventory acceptance."""
import json, sys, time
from pathlib import Path
from PIL import Image
from qa import App, require_fenced
binary, work = sys.argv[1],Path(sys.argv[2]); require_fenced(work)
album=work/'lib/Set'; album.mkdir(parents=True,exist_ok=False)
a=Image.new('RGB',(320,240),'red'); a.paste('blue',(30,40,180,170))
a.save(album/'same.png'); a.save(album/'copy.png'); a.save(album/'same.jpg',quality=95); a.save(album/'same.webp',lossless=True)
app=App(binary,work/'lib',work/'profile',log=work/'app.log'); results=[]
try:
 app.wait_albums(); app.op('activate',album='Set'); app.wait_idle(180); time.sleep(2)
 names=app.op('inventory',album='Set')['names']; results.append(dict(name='same stem extensions retained',ok={'same.jpg','same.png','same.webp','copy.png'}<=set(names)))
 page=app.op('core',method='find_duplicate_groups',params={'dir':str(album),'offset':0,'signature':None})
 members=[]
 for group in page['groups']:
  part=app.op('core',method='duplicate_group_members',params={'dir':str(album),'root':group['root'],'signature':page['signature'],'offset':0})
  members.append({x['name'] for x in part})
 results.append(dict(name='exact positive grouped',ok=any({'same.png','copy.png'}<=x for x in members)))
finally: app.close()
results.append(dict(name='clean exit',ok=app.exit_code==0))
(work/'results.json').write_text(json.dumps(dict(build=app.build,results=results),indent=2))
raise SystemExit(not all(r['ok'] for r in results))
