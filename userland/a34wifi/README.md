# a34wifi

The user-space half of milestone 1 of the Galaxy A34's Wi-Fi (`docs/wifi-plan.md`): a small
tool that asks the kernel's Wi-Fi bring-up (`xnu/patches/0939-a34-wifi-m1.patch`,
`bsd/dev/a34_wifi.c`) for its steps and prints what the kernel says.

The kernel does nothing for Wi-Fi until it is asked, by boot-arg `a34wifi=N` or by this tool
(sysctl `kern.a34wifi.want`, root only). The tool exists because the boot-args are close to the
607 characters XNU keeps.

| Step | What the kernel does | Success in the log |
| --- | --- | --- |
| 1 | reads what the boot loader left: SPM, bus protection, PMIC rails, RC mode | `A34 WIFI: step 1` lines |
| 2 | CONNSYS (`conn_infra`) on: VCN18/VCN13, pins, MTCMOS, bus protection off | `conn_infra IP version 0x02060002` |
| 3 | `conn_infra` set up as Android's `conninfra.ko` does; the MT6635 A-die; VCN33 for Wi-Fi | `A-die chip id 0x66358a10: MT6635 E2` (the firmware is built for 0x8a10) |
| 4 | the Wi-Fi subsystem powered, the MCU's image (`soc5_0_ram_wmmcu_1_1_hdr.bin`) in CONNSYS's memory, the MCU's ROM done | `WFSYS on: version 0x02050002`, `WM MCU ROM done (mailbox 0x00001d1e)` |
| 5 | driver own, the command/event DMA rings, the ROM's versions, `WIFI_RAM_CODE_soc5_0_1_1.bin` loaded, FW_FINISH | `the ROM answers: TOP_HVR ...`, `THE WI-FI FIRMWARE IS READY` |

## Use

```sh
a34wifi                 # the state and the kernel's Wi-Fi log
a34wifi up 5 [SECONDS]  # steps up to 5, following the log until the kernel is done (default 120 s)
a34wifi log             # the log only
a34wifi files           # the firmware files in /a34/wifi
```

## Install (from the host, into the iOS root)

The firmware is Samsung's/MediaTek's and is never kept in this repository: `tools/wifi-fw.sh`
takes it from the phone's own firmware (`super.img` of A346EXXSADYG1). Then, with the root
mounted at `/mnt` and the a34wifi artifact unpacked:

```sh
tools/wifi-fw.sh .../A346EXXSADYG1/super.img /tmp/a34wifi
cp a34wifi-artifact/a34/wifi/a34wifi /tmp/a34wifi/
sudo tools/root-add-job.py /mnt com.a34.wifi /a34/wifi /tmp/a34wifi a34wifi up 5
```

The job runs once at boot with its output on `/dev/console`, so it lands in the kernel's
persistent log (`tools/pull-log.sh`). Asking for fewer steps (`a34wifi up 1`) is the cautious
first test.

## Build

`userland/a34wifi/build.sh` on a Mac with Xcode (`.github/workflows/a34wifi.yml`): arm64,
iOS 16.0, ad-hoc signed, no `LC_RPATH`.
