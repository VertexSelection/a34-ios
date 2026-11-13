#!/usr/bin/env python3
"""Verify Samsung .tar.md5 files: a tar with '<md5 hex>  <name>.tar\\n' appended."""
import hashlib, os, re, sys
bad = 0
for p in sys.argv[1:]:
    size = os.path.getsize(p)
    with open(p, "rb") as f:
        f.seek(max(0, size - 1024))
        tail = f.read()
        m = re.search(rb"([0-9a-fA-F]{32})  ([^\n\0]+)\n$", tail)
        if not m:
            print("???  %s  (no md5 trailer)" % os.path.basename(p)); bad += 1; continue
        body = size - (len(tail) - m.start())
        f.seek(0)
        h = hashlib.md5()
        left = body
        while left:
            b = f.read(min(1 << 22, left))
            if not b:
                break
            h.update(b); left -= len(b)
    ok = h.hexdigest() == m.group(1).decode().lower()
    bad += not ok
    print("%s  %s" % ("OK " if ok else "BAD", os.path.basename(p)))
sys.exit(1 if bad else 0)
