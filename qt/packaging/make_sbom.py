#!/usr/bin/env python3
"""make_sbom.py DEPS_LOCK OUT_JSON [NAME=VERSION=LICENSE ...]: CycloneDX list of the C libraries compiled into the core.
The libraries, versions, download URLs and SHA-256 come from the core-sources group of ci/deps.lock.json."""
import json
import sys
from pathlib import Path
LICENSES = {'sqlite-amalgamation': ('SQLite', 'blessing'), 'libwebp': ('libwebp', 'BSD-3-Clause'), 'monocypher': ('Monocypher', 'BSD-2-Clause OR CC0-1.0'), 'zlib': ('zlib', 'Zlib'), 'libfido2': ('libfido2', 'BSD-2-Clause'), 'libcbor': ('libcbor', 'MIT'), 'openssl': ('OpenSSL', 'Apache-2.0')}
lock, out = map(Path, sys.argv[1:3])
data = json.loads(lock.read_text())
versions = {'sqlite-amalgamation': data['versions']['sqlite'], 'libwebp': data['versions']['libwebp'], 'monocypher': data['versions']['monocypher'], 'zlib': data['versions']['zlib'], 'libfido2': data['versions']['libfido2'], 'libcbor': data['versions']['libcbor'], 'openssl': data['versions']['openssl']}
components = []
for artifact in data['artifacts']:
    if artifact['group'] != 'core-sources':
        continue
    if artifact['name'] not in LICENSES:
        sys.exit(f"Missing license metadata: {artifact['name']}")
    name, license_id = LICENSES[artifact['name']]
    components.append({'type': 'library', 'name': name, 'version': versions[artifact['name']], 'licenses': [{'expression': license_id}],
                       'externalReferences': [{'type': 'distribution', 'url': artifact['url']}], 'hashes': [{'alg': 'SHA-256', 'content': artifact['sha256']}]})
for extra in sys.argv[3:]:
    name, version, license_id = extra.split('=', 2)
    components.append({'type': 'library', 'name': name, 'version': version, 'licenses': [{'license': {'name': license_id}}]})
out.write_text(json.dumps({'bomFormat': 'CycloneDX', 'specVersion': '1.5', 'version': 1, 'components': components}, indent=1) + '\n')
print(len(components))
