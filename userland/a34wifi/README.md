# a34wifi

The user-space half of milestone 1 of the Galaxy A34's Wi-Fi (`docs/wifi-plan.md`): a small
tool that asks the kernel's Wi-Fi bring-up (`xnu/patches/0940-a34-wifi-m1.patch`,
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
| 6 | (M2, 0961) the running firmware's command mailbox: NIC capability (versions, MAC), BASIC_CONFIG | `the firmware answers: NIC_CAPABILITY_V2`, `MAC ...` |
| 7 | (M2) one **passive** scan (nothing transmitted) of 2.4 GHz 1-13 and 5 GHz 36-48, 149-165 | `SCAN_DONE in N ms`, then one line per network: BSSID, channel, dBm, security, SSID |
| 8 | (M3, 0964) join the network of `/a34/wifi/network`: the loudest BSS of its name, channel, authentication, association, for WPA2 the four-way handshake and the keys | `TX status N: acknowledged`, `ASSOCIATED, AID n`, `the four-way handshake is done`, `JOINED` |
| 9 | (0965) the network interface `enN` on the data rings, its link up | `step 9: enN attached`; then `a34wifi dhcp` |

## Use

```sh
a34wifi                 # the state and the kernel's Wi-Fi log
a34wifi up 7 [SECONDS]  # steps up to 7, following the log until the kernel is done (default 120 s)
a34wifi scan [SECONDS]  # once step 7 is done: one scan more (kern.a34wifi.rescan)
a34wifi log             # the log only
a34wifi files           # the firmware files in /a34/wifi
a34wifi mac             # the phone's Wi-Fi address (made once and kept in /a34/wifi/mac)
a34wifi join [SECONDS]  # step 8 (or, once done, join again after the link was lost)
a34wifi dhcp [SECONDS]  # after step 9: DHCP by configd's IPConfiguration, waits for the address
a34wifi tcp HOST [PORT] # a TCP connection and an HTTP HEAD: does the data path carry anything
a34wifi connect [HOST [PORT]]  # all of it for a boot job: up 9, dhcp, tcp (default 1.1.1.1 80)
```

### The phone's MAC address (0963)

The chip's efuse holds none, and Samsung's is in `/efs`, which nothing here reads. `a34wifi`
makes a random locally administered address the first time (`mac`, or `up N` with N >= 6) and
keeps it in `/a34/wifi/mac`; the kernel uses it from step 6 on. Delete the file for a new one.

### The network to join (0964)

`/a34/wifi/network`, written by the phone's owner -- it is never in this repository, and neither
the kernel nor `a34wifi` ever prints the passphrase:

```
ssid=Name Of The Network
psk=the passphrase
```

`psk=` is 8 to 63 characters or 64 hex digits; without it the network must be open. An optional
`bssid=xx:xx:xx:xx:xx:xx` picks one access point. Everything after `=` counts, spaces too. Install
it readable by root only, not with `root-add-job.py` (which makes every file 0755):

```sh
sudo install -m 600 -o 0 -g 0 network /mnt/a34/wifi/network
```

## Install (from the host, into the iOS root)

The firmware is Samsung's/MediaTek's and is never kept in this repository: `tools/wifi-fw.sh`
takes it from the phone's own firmware (`super.img` of A346EXXSADYG1). Then, with the root
mounted at `/mnt` and the a34wifi artifact unpacked:

```sh
tools/wifi-fw.sh .../A346EXXSADYG1/super.img /tmp/a34wifi
cp a34wifi-artifact/a34/wifi/a34wifi /tmp/a34wifi/
sudo tools/root-add-job.py /mnt com.a34.wifi /a34/wifi /tmp/a34wifi a34wifi connect
```

(`up 7` stops after the scan; `up 9` and `connect` need the network file; `connect` also needs
`com.apple.configd` running for DHCP.)

If the log says the AP never acknowledges our frames (`TX status N: NOT acknowledged`), the next
boot can try PHY_ACTION (0966: calibration with the vendor's default NVRAM tags before the RAM
code), either with boot-arg `a34wifiphy=1` or for the job:

```sh
sudo tools/root-add-job.py --env /mnt com.a34.wifi A34WIFI_PHYACTION=1
```

The job runs once at boot with its output on `/dev/console`, so it lands in the kernel's
persistent log (`tools/pull-log.sh`). Asking for fewer steps (`a34wifi up 1`) is the cautious
first test.

### iOS's Wi-Fi side (0969)

From 0969 the interface appears as a Wi-Fi interface in the registry (`A34WLAN`) as soon as the
firmware is up, and the kernel answers wifid's Apple80211 requests. wifid's job asks for a
device-tree node our tree does not have; remove that condition once:

```sh
sudo tools/root-add-job.py --unlimit /mnt com.apple.wifid
```

`a34wifi up 7` (or `connect`) brings the firmware up; Settings > Wi-Fi then lists and joins
networks. `a34wifi dhcp` waits 10 s for configd's own DHCP before asking for a service of its own.

## Build

`userland/a34wifi/build.sh` on a Mac with Xcode (`.github/workflows/a34wifi.yml`): arm64,
iOS 16.0, ad-hoc signed, no `LC_RPATH`.
