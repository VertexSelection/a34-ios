# a34-ios

Bring-up of Apple's XNU and iOS userland on the Samsung Galaxy A34 5G
(SM-A346E, MediaTek MT6877 / Dimensity 1080), booted from the stock Samsung
bootloader. Research project; nothing here is usable as a daily system.

## Status

| Step | State |
| --- | --- |
| Bootloader unlock, One UI 7 (A346EXXSADYG1) base, root | done |
| Own code started by Samsung LK from the `recovery` partition | done |
| LK framebuffer and display-engine state at handoff | mapped |
| Loop-free test harness (shim runs, then boots TWRP) | done |
| XNU build pipeline (GitHub Actions, macOS runner) | in progress |
| XNU loader, device tree, root filesystem | not started |

## Layout

| Path | Contents |
| --- | --- |
| `tools/payload/` | bare-metal test payloads (`hello.S`, `shim-start.S`, `shim.c`) |
| `tools/mkrecovery.py` | packs a payload into a recovery image the bootloader accepts |
| `tools/build-hello.sh`, `tools/build-shim.sh` | build and sign test images |
| `tools/expdb-grep.py` | reads the bootloader's own log from an `expdb` dump |
| `tools/fw-*.py`, `tools/fw-fetch.sh`, `tools/tarmd5.py` | firmware download, repair and flash planning |
| `.github/workflows/` | kernel builds on GitHub's macOS runners |

No Apple firmware, Samsung firmware or device dumps are kept in this repository.

## Credits

- [lky005/ruby-ios-bringup](https://github.com/lky005/ruby-ios-bringup): XNU on
  the same SoC (Redmi Note 12 Pro 5G). Its patches are fetched at build time,
  not copied here.
- [UniXNU](https://github.com/UniXNU/UniXNU): reference for loaders and device trees.
- [TWRP](https://twrp.me) for a34x, [Magisk](https://github.com/topjohnwu/Magisk),
  [Heimdall](https://git.sr.ht/~grimler/Heimdall), [samloader-rs](https://github.com/topjohnwu/samloader-rs).
