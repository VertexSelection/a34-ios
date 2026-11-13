# a34-ios

Apple's XNU kernel and the unmodified iOS 16.7.16 userland (iPhone X build)
on the Samsung Galaxy A34 5G (SM-A346E, MediaTek MT6877 / Dimensity 1080),
booted from the stock Samsung bootloader. Research project; nothing here is
usable as a daily system, and installing it erases the phone.

## Status

| Area | State |
| --- | --- |
| Boot | Samsung LK → our loader in `recovery` → XNU 8796 (ruby-ios-bringup patch set + `xnu/patches/09xx`) |
| Userland | launchd → backboardd → SpringBoard → **lock screen** |
| Display | LK's framebuffer; frames copied by the CPU (~3 ms) and sent to the command-mode DSI panel from a separate kick work loop (~7.6 ms per transfer); CPU rendering (no GPU) |
| Touch | Goodix GT9895 on I2C0, polled (patch 0903) |
| Buttons | volume up/down, power (patch 0902) |
| CPU | all 8 cores (6× Cortex-A55 at ~1.8 GHz, 2× Cortex-A78), one scheduler cluster |
| Storage | iOS root (HFS+) at the start of `userdata` |
| GPU | Mali-G68 MC4 driven from iOS: kernel bring-up and a panfrost-style IOKit service (0908–0916, `a34mali=1`), Mesa 26.3 Panfrost (OpenGL ES 3.1, `userland/mesa`), and CoreAnimation rendering through it via ios-topaz's EAGL hook (`userland/a34gl`, injected into backboardd with `a34inject=/a34/liba34gl-inject.dylib,backboardd`): **the UI is GPU-rendered, ~20 fps**; some visual glitches |
| Boot time | empty `IOAcceleratorES` (0907, from ios-topaz) saves ~45 s; first frames ~65 s after XNU starts |
| Not yet | Metal, Wi-Fi, Bluetooth, cellular, audio, camera, RTC, battery level, sleep |

The base firmware is One UI 7 (A346EXXSADYG1). Do not go to One UI 8 or
later: it removes bootloader unlocking. Never write the `efs`, `sec_efs`,
`nvram`, `nvdata`, `nvcfg`, `protect1/2` or `proinfo` partitions (IMEI).

## How it boots

1. `recovery` holds a boot image whose "kernel" is our loader
   (`tools/payload/a34boot.c`) followed by the XNU payload and TWRP's kernel.
2. The loader wakes the UFS, clears the bootloader's recovery note, turns off
   LK's watchdog, prepares the GIC and the display, and shows grey squares.
3. Holding **Volume Up** on the white bar starts TWRP instead (rescue path).
4. Otherwise it copies XNU, an Apple device tree built for the A34
   (`tools/mkadt_a34.py`) and boot args into place and jumps to XNU.
5. XNU mounts the iOS root from `userdata` and runs launchd.

The kernel keeps a persistent log at the end of `userdata`;
`tools/pull-log.sh` reads it from TWRP.

## Build and install (outline)

- Kernel: `.github/workflows/xnu-a34.yml` on GitHub's macOS runners.
- iOS root: `tools/build-iosroot.sh iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw`
  (you supply the IPSW; it applies the first-boot fix in `tools/icl-nop.py`).
- Write the root from TWRP: `tools/write-root.sh build/ios16-root.img`.
- Boot image: `tools/build-xnu.sh KERNEL "BOOT-ARGS"`, then
  `tools/run-test.sh build/xnu-recovery.img`.

Boot args in use: `debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=ufsroot0
ufsroot=userdata ufslun=2 ufsrw=1 rootrw=1 sweethw=0 sweetgt=0
launchd_unsecure_cache=1 unentitled_ios_sim_launch=1 ignition_level=1 wdt=-1
wdt_test=no_user_checkin sweetfb_noconsole=1 sweetfb_hz=30 rubkeys=1 a34touch=1 a34mali=1
a34inject=/a34/liba34gl-inject.dylib,backboardd`
(`cpus=8`; `a34hb=10` adds a 10-second heartbeat with per-CPU interrupt counts and
display timings to the kernel log)

## Layout

| Path | Contents |
| --- | --- |
| `tools/payload/` | loader (`a34boot.c`), UFS access, test shim and touch probe |
| `tools/mkadt_a34.py` | Apple device tree for the A34 |
| `tools/build-xnu.sh`, `tools/mkrecovery.py` | boot image packing and signing |
| `tools/build-iosroot.sh`, `tools/write-root.sh`, `tools/icl-nop*.{py,sh}` | iOS root image |
| `tools/run-test.sh`, `tools/pull-log.sh` | flash-and-boot, kernel log |
| `xnu/` | A34 kernel patches on top of ruby-ios-bringup |
| `third_party/ios-sweet/` | loader pieces and tools from ios-sweet (MIT) |
| `.github/workflows/` | kernel builds |

No Apple firmware, Samsung firmware or device dumps are kept in this repository.

## Credits

- [lky005/ruby-ios-bringup](https://github.com/lky005/ruby-ios-bringup): XNU on
  the same SoC (Redmi Note 12 Pro 5G). Its patches are fetched at build time,
  not copied here. The first-boot InstalledContentLibrary fix is theirs.
- [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) (MIT) and
  [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz): the XNU-on-Android-phone work this all builds on.
- Samsung's A34 kernel sources (via
  [Fede2782/android_kernel_samsung_a34x](https://github.com/Fede2782/android_kernel_samsung_a34x)):
  I2C, clock, PMIC and touch register facts.
- [oosexgang/hakios-mt6768](https://github.com/oosexgang/hakios-mt6768) (HackiOS, APSL 2.0 / MIT):
  the Mali bring-up in 0908 is ported from its `redwood_mali.c`.
- [UniXNU](https://github.com/UniXNU/UniXNU): reference for loaders and device trees.
- [TWRP](https://twrp.me) for a34x, [Magisk](https://github.com/topjohnwu/Magisk),
  [Heimdall](https://git.sr.ht/~grimler/Heimdall), [samloader-rs](https://github.com/topjohnwu/samloader-rs).
