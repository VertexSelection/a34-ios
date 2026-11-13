#!/usr/bin/env python3
"""Map unpacked Samsung firmware images to PIT partitions for a Heimdall flash.

  fw-plan.py PIT_TEXT IMAGE_DIR [--args]

PIT_TEXT is `heimdall print-pit` output. Each image is matched to the partition
whose "Flash Filename" equals the image's file name. Prints the plan and exits
non-zero if any image has no partition, maps to more than one, or does not fit.
With --args, prints only the Heimdall arguments, one "--name path" per line.
"""
import os, re, sys

pit_text, img_dir = sys.argv[1], sys.argv[2]
entries, cur = [], None
for line in open(pit_text, encoding="utf-8", errors="replace"):
    line = line.rstrip("\n")
    if line.startswith("--- Entry"):
        cur = {}; entries.append(cur); continue
    if cur is None or ":" not in line:
        continue
    k, _, v = line.partition(":")
    cur[k.strip()] = v.strip()

by_file = {}
for e in entries:
    fn = e.get("Flash Filename", "")
    if fn:
        by_file.setdefault(fn, []).append(e)

# Order: the big images first (the phone keeps its old, working boot chain while they
# are written), then the boot images, and the low-level boot chain last and together.
ORDER = ["super.img", "userdata.img", "prism.img", "optics.img", "cache.img", "omr.img",
         "md1img.img", "misc.bin", "recovery.img", "vendor_boot.img", "init_boot.img", "boot.img",
         "dtbo.img", "up_param.bin", "param.bin", "efuse.img", "uh.bin",
         "cam_vpu1-verified.img", "cam_vpu2-verified.img", "cam_vpu3-verified.img",
         "audio_dsp-verified.img", "pi_img-verified.img", "vbmeta_system.img", "vbmeta.img",
         "dpm-verified.img", "mcupm-verified.img", "sspm-verified.img", "scp-verified.img",
         "spmfw-verified.img", "tzar.img", "gz-verified.img", "tee-verified.img",
         "preloader.img", "lk-verified.img"]
files = sorted(f for f in os.listdir(img_dir) if not f.endswith(".pit") and os.path.isfile(os.path.join(img_dir, f)))
files.sort(key=lambda f: ORDER.index(f) if f in ORDER else len(ORDER))

bad, plan = 0, []
for f in files:
    size = os.path.getsize(os.path.join(img_dir, f))
    es = by_file.get(f, [])
    if len(es) != 1:
        print("PROBLEM %-28s matches %d partitions" % (f, len(es))); bad += 1; continue
    e = es[0]
    name = e["Partition Name"]
    blocks = int(e["Partition Block Count"])
    bsz = 4096 if "UFS" in e.get("Device Type", "") else 512
    cap = blocks * bsz
    fits = cap == 0 or size <= cap
    sparse = open(os.path.join(img_dir, f), "rb").read(4) == b"\x3a\xff\x26\xed"
    if not fits or f not in ORDER:
        bad += 1
    plan.append((name, f, size, cap, e.get("Binary Type", "?"), e.get("Identifier", "?"), sparse, fits, f in ORDER))

if "--args" in sys.argv:
    if bad:
        sys.exit(1)
    for name, f, *_ in plan:
        print("--%s %s" % (name, os.path.join(img_dir, f)))
    sys.exit(0)

print("%-14s %-4s %-28s %13s %13s %-7s %s" % ("partition", "id", "image", "image bytes", "capacity", "type", "notes"))
for name, f, size, cap, bt, ident, sparse, fits, known in plan:
    notes = []
    if sparse: notes.append("sparse")
    if not fits: notes.append("DOES NOT FIT")
    if not known: notes.append("NOT IN ORDER LIST")
    print("%-14s %-4s %-28s %13d %13s %-7s %s" % (name, ident, f, size, cap if cap else "rest", bt.split()[0] + ("" if "(AP)" in bt else bt[1:]), ", ".join(notes)))
unused = [e["Partition Name"] for e in entries if e.get("Partition Name") and e.get("Flash Filename", "") not in files]
print("\nimages: %d   problems: %d" % (len(plan), bad))
print("partitions left untouched (%d): %s" % (len(unused), " ".join(unused)))
sys.exit(1 if bad else 0)
