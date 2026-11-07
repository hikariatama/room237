#!/usr/bin/env python3
"""Verify a signed update feed: minisign prehashed Ed25519 signature, trusted comment and every payload.

  verify_feed.py FEED PUBKEY_FILE ROOT                      loopback QA feed; URL paths are relative to ROOT
  verify_feed.py FEED PUBKEY_FILE ROOT --url-prefix PREFIX   release feed; every URL is PREFIX + a file name in ROOT
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
from urllib.parse import unquote, urlsplit
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

KINDS = {'tar.gz', 'appimage'}
PLATFORMS = {'darwin-aarch64', 'darwin-x86_64', 'linux-x86_64', 'linux-appimage-x86_64', 'windows-x86_64'}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('feed', type=Path)
    parser.add_argument('pubkey', type=Path)
    parser.add_argument('root', type=Path)
    parser.add_argument('--url-prefix', help='https://github.com/OWNER/REPO/releases/download/TAG/')
    parser.add_argument('--require-platforms', action='store_true', help='every desktop platform key must be present')
    args = parser.parse_args()
    pub = base64.b64decode(args.pubkey.read_text().strip().splitlines()[-1], validate=True)
    assert len(pub) == 42 and pub[:2] == b'Ed', 'invalid trust public key'
    key = Ed25519PublicKey.from_public_bytes(pub[10:])
    lines = args.feed.with_name(args.feed.name + '.minisig').read_text().splitlines()
    sig = base64.b64decode(lines[1], validate=True)
    assert sig[:2] == b'ED' and sig[2:10] == pub[2:10], 'signature key ID mismatch'
    key.verify(sig[10:], hashlib.blake2b(args.feed.read_bytes(), digest_size=64).digest())
    assert lines[2].startswith('trusted comment: ')
    trusted = lines[2][len('trusted comment: '):]
    key.verify(base64.b64decode(lines[3], validate=True), sig[10:] + trusted.encode())
    feed = json.loads(args.feed.read_bytes())
    assert trusted == 'version:' + feed['version'], 'trusted comment does not name the feed version'
    if args.require_platforms:
        assert set(feed['platforms']) == PLATFORMS, sorted(feed['platforms'])
    root = args.root.resolve()
    for platform, entry in feed['platforms'].items():
        assert platform in PLATFORMS, 'unknown platform key ' + platform
        assert entry['kind'] in KINDS, 'unsupported update payload kind'
        url = urlsplit(entry['url'])
        if args.url_prefix:
            assert args.url_prefix.startswith('https://github.com/') and args.url_prefix.endswith('/')
            assert entry['url'].startswith(args.url_prefix), 'URL outside the release: ' + entry['url']
            name = unquote(entry['url'][len(args.url_prefix):])
            assert name and '/' not in name and not url.query and not url.fragment, entry['url']
            payload = root / name
        else:
            assert url.hostname in ('localhost', '127.0.0.1', '::1'), 'non-local feed URL'
            payload = root / unquote(url.path).lstrip('/')
        assert payload.resolve().is_relative_to(root), 'payload escapes the feed root'
        h = hashlib.sha256()
        with payload.open('rb') as f:
            for block in iter(lambda: f.read(1024 * 1024), b''):
                h.update(block)
        assert h.hexdigest() == entry['sha256'] and payload.stat().st_size == entry['size'], platform
    print(f"signature, trusted comment, {len(feed['platforms'])} payload hashes/sizes/kinds: passed")


if __name__ == '__main__':
    main()
