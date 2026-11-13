#!/usr/bin/env python3
"""Pull readable log lines out of an expdb dump and grep them.

  expdb-grep.py EXPDB.bin REGEX [CONTEXT]

Lines that look like they carry device identifiers are masked.
"""
import re, sys
d = open(sys.argv[1], "rb").read()
pat = re.compile(sys.argv[2])
ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 0
lines = []
for m in re.finditer(rb"[\t -~]{4,}", d):
    for ln in m.group().decode("ascii", "replace").split("\n"):
        ln = ln.strip()
        if ln:
            lines.append((m.start(), ln))
SECRET = re.compile(r"(serial|imei|uniqueno|\bsn\b|ap_serial|mac.?addr|em\.did|chip.?id|hw.?id|\bdid\b)\s*[=:]\s*\S+", re.I)
shown = set()
for i, (off, ln) in enumerate(lines):
    if pat.search(ln):
        for j in range(max(0, i - ctx), min(len(lines), i + ctx + 1)):
            if j in shown:
                continue
            shown.add(j)
            o, l = lines[j]
            print("%9d  %s" % (o, SECRET.sub(lambda m: m.group(1) + "=<masked>", l)[:220]))
        if ctx:
            print("   ----")
