# Wi-Fi on the Galaxy A34: what it takes, and milestone 1

Written 15 November 2026 from files alone. **M1 and M2 have since run on the phone** (0961, with
`a34bt=1` in the same boot: section 3, "M2 on the phone"); 0963-0965 (the MAC address, joining,
the data path) are built and host-checked, not yet run.
Sources, all read on the host:

- the phone's own firmware, A346EXXSADYG1 (`firmware/A346EXXSADYG1_TUR`, gitignored): `vendor`
  and `vendor_dlkm` cut out of `super.img` (`tools/wifi-fw.sh` does the same), kept under
  `research/wifi/` (gitignored) with `SHA256SUMS`;
- Samsung's GPL kernel for this phone, One UI 7 (`A346BXXSFEZC7`, kernel 6.6, the same source
  as DYG1's modules: `vermagic=6.6.50-android15-8-abA346EXXSADYG1`), branch `sep-17.0/stock` of
  [Fede2782/android_kernel_samsung_a34x](https://github.com/Fede2782/android_kernel_samsung_a34x),
  sparse-cloned to `research/wifi/src/a34x`. Paths below starting `connectivity/` are under its
  `vendor/mediatek/kernel_modules/connectivity/`;
- what Android said while running (`recon/android-DYG1/`: `dmesg.txt`, `live.dts`, `iomem.txt`);
- our iOS 16.7.16 root (wifid, configd, IPConfiguration, the IO80211 and SystemConfiguration
  frameworks of the dyld cache);
- HackiOS (`research/metal/prior/hakios-mtk`, branch `mtk@a962798`), and the other ports.

## 1. The hardware path

| Piece | What it is | Evidence |
| --- | --- | --- |
| SoC side | CONNSYS of the MT6877, CONNAC 2.0, MediaTek's chip name **soc5_0** | `connectivity/wlan/core/gen4m/Kbuild.6877`: `MTK_COMBO_CHIP := CONNAC2X2_SOC5_0`, `CONNAC_VER := 2_0`, `CONFIG_MTK_COMBO_WIFI_HIF := axi`; dmesg: `asicConnac2xFillCmdTxdInfo`, module `wlan_drv_gen4m_6877` |
| Radio (A-die) | **MT6635** (Wi-Fi 6 2x2, BT, GPS, FM), on the RF SPI of `conn_infra` | `mt6877.c`: `consys_adie_detection_mt6877()` returns 0x6635; `mt6877_pos.c`: chip id 0x66358A00/10/11 = E1/E2/E3; `/vendor/firmware/mt6635_fm_v1_*.bin`; the firmware headers say hardware 0x8a10 (E2) |
| Front end | two FEM variants (`wlan7204h`, `wlan7102c`), GPIO-selected | `live.dts`: `connfem@18000000`, pinctrl `wlan7204h_wf0/wf1`, `wlan7102c_wf0/wf1` |
| Host interface | AXI: the 7 MB register window at 0x18000000, WFDMA rings in AP memory | `live.dts`: `wifi@18000000` reg 0x18000000/0x700000, interrupts GIC SPI 0x1cf and 0x1de (level), `memory-region` = `wifi-reserve-memory` (0xf20000, 16 MB-aligned, 0x40000000..0xbfffffff; at 0xbe000000 on the running phone, `iomem.txt`) |
| Infra | `conninfra.ko` (not WMT/STP/BTIF, which the MT6768 that HackiOS did needs) | `/vendor_dlkm/lib/modules`: `conninfra.ko`, `connadp.ko`, `connfem.ko`, `wlan_drv_gen4m_6877.ko`, `wmt_chrdev_wifi_connac2.ko`, `bt_drv_6877.ko`; `init.wlan_drv.rc` loads the Wi-Fi ones once `vendor.connsys.driver.ready` |
| CONNSYS memory | 7.5 MB "EMI", 1 MB-aligned in 0x50000000..0x8fffffff | `live.dts`: `consys-reserve-memory`; `conninfra/.../emi_mng.c`; layout in `mt6877_connsyslog.h`, `mt6877_coredump.c` |
| Power | MT6359P LDOs VCN18 (1.8 V), VCN13 (1.3 V), VCN33_1 and VCN33_2 (3.3 V, Wi-Fi) | `mt6877_pmic.c`; `drivers/regulator/mt6359p-regulator.c` |

### Firmware files (on the phone, `/vendor/firmware`)

| File | Size | What | Where it goes |
| --- | --- | --- | --- |
| `soc5_0_ram_wmmcu_1_1_hdr.bin` | 512 268 | the Wi-Fi MCU's RAM image (48-byte header: build `20250122153007`, `ALPS`, hw/sw 0x8a10, load address 0xf0380000 = EMI + 0x380000) | CONNSYS memory |
| `WIFI_RAM_CODE_soc5_0_1_1.bin` | 969 556 | the Wi-Fi firmware proper: five regions plus a tailer (version `____000000`, built `20250122153052`) | three written through the AP2WF register window, two into CONNSYS memory (below) |
| `soc5_0_ram_mcu_1_1_hdr.bin`, `soc5_0_ram_bt_1_1_hdr.bin` | 141 618, 2 911 132 | the BT/GPS ("BGF") MCU and Bluetooth | not needed for Wi-Fi |
| `*_1c_1*` | | the same for the "c" flavour (single antenna, `flavor_bin = "c"` in the `wifi` node) | not ours: the A34's node has no flavour |
| `WIFI`, `wifi.cfg` | 6 146, 196 | default NVRAM image and driver settings | read later (M2) |

No `soc5_0_patch_*` file exists: on soc5_0 the RAM image above takes the patch's place.

### How Android brings it up

`conninfra.ko` powers CONNSYS for whichever radio asks first (`connectivity/conninfra/conn_drv/
connv2/core/conninfra_core.c: opfunc_power_on_internal()` -> `platform/consys_hw.c:
consys_hw_pwr_on()`), with MT6877's steps in `platform/mt6877/mt6877_pos.c` and
`mt6877_pmic.c`; then `wlan_drv_gen4m_6877.ko` powers the Wi-Fi subsystem and downloads its
firmware (section 3). On first power-on (`curr_status == 0`):

1. **PMIC** (`consys_plt_pmic_common_power_ctrl_mt6877`): VCN18 1.8 V and VCN13 1.3 V on. If
   SRCLKEN_RC is in RC mode (`0x1000f804` bit 0) their PMRC_EN bits 7..4 are set (hardware
   on/off by the clock-request lines) and they are also software-enabled; in legacy mode
   HW_OP_EN/CFG bit 0. Registers: `drivers/regulator/mt6359p-regulator.c` (enable `CON0` bit 0,
   status `MON` bit 0, VOSEL `ANA_CON0[11:8]`: VCN13 index 4 = 1.3 V, VCN33 index 13 = 3.3 V).
2. **Pins** (`consys_set_if_pinmux_mt6877`): the D-die/A-die interface (`GPIO_MODE24[31:16]`,
   `GPIO_MODE25[26:0]`) to aux 1, 2 mA, `CONN_TOP_DATA`/`CONN_BT_DATA` pulled up (`IOCFG_RT`
   0x11ea0000).
3. **MTCMOS**: in this build through the power domain (`consys_platform_spm_conn_ctrl_mt6877`
   -> `pm_runtime_get_sync`), which is `drivers/clk/mediatek/clk-mt6877-pg.c:
   spm_mtcmos_ctrl_conn_pwr()`: `CONN_PWR_CON` (0x10006e04) PWR_ON, PWR_ON_2ND, wait for
   `PWR_STATUS`/`PWR_STATUS_2ND` (0x10006ef0/ef4) bit 1, clear CLK_DIS and ISO, set RST_B; then
   `spm_mtcmos_ctrl_conn_bus_prot()` releases the bus protection through
   `INFRA_TOPAXI_PROTECTEN_CLR` (0x100012a4): bits 14|19, then 13|18. The `#else` branch of
   `consys_conninfra_on_power_ctrl_mt6877()` spells out the same by hand, plus TOPRGU's CONNSYS
   reset (`WDT_SWSYSRST` 0x10007200 bit 9, key 0x88) and "wait 5 ms for the XO".
4. **Alive**: `conn_infra`'s IP version at 0x18001000 reads **0x02060002** (10 tries, 1 ms).
5. **Set-up**: EMI remap (`0x180601c4` = EMI base >> 16; peripherals 0x1000), `d_die_cfg`
   (sysram cleared, the CONN-to-AP EMI window 0x04000000..0x43ffffff, efuse copied),
   `spi_master_cfg` (the A-die registers the sleep controller drives), `a_die_cfg` (A-die out
   of reset, its **chip id over the RF SPI: 0x6635_8a00/8a10/8a11**, efuse and thermal trim,
   XO/LDO settings), `afe_wbg_cal`, `subsys_pll_initial`, radio status in sysram,
   `low_power_setting` (RC or legacy oscillator control, bus timeouts, clock dividers, TOPRGU
   bit 12, then `conn_infra` under hardware sleep control), and the radio's own rails
   (`consys_plt_pmic_wifi_power_ctrl_mt6877`: VCN33_1 and VCN33_2 at 3.3 V).

From then on every access to `conn_infra` is bracketed by a wake-up (`0x180601a0` = 1, IP
version polled) and a release. Nothing of this needs a vendor blob, an SMC or a firmware
service: the only firmware involved before the Wi-Fi MCU is the A-die's own ROM. The EMI
region is given an EMI-MPU region (27) by Android (`mt6877_emi.c`, through the kernel's EMI-MPU
driver); M1 does not repeat it, assuming DRAM no region covers is open to CONNSYS -- an
assumption, which held on the MT6768 (HackiOS: CONNSYS wrote its memory without one).

### Then the Wi-Fi driver

The module on the phone is built from `connectivity/wlan/core/gen4m_s1` (its strings name
`gen4m_s1/chips/soc5_0/soc5_0.c`, and it has `WIFI-SW-WFDMA`/`WIFI-LLS`, which only `gen4m_s1`
has). Samsung's published `gen4m_s1/chips/soc5_0/soc5_0.c` lacks the ROM-EMI and chip-info
parts the module carries (`soc5_0_wlanPowerOnInit`, `soc5_0_ram_wmmcu_%u%s_%x_hdr.bin` are in
the module's strings); `gen4m/chips/soc5_0/soc5_0.c` has them, and the register sequences are
the same in both trees.

1. **WFSYS on** (`gen4m_s1/chips/soc5_0/soc5_0.c: wf_pwr_on_consys_mcu()`, called from
   `asicConnac2xPwrOnWmMcu()` after `conninfra_pwr_on(WIFI)`): wake conn_infra for Wi-Fi
   (`0x180601a4` bit 0, version polled), hold the WM MCU (`0x18000120` bit 0 = 0), Wi-Fi sleep
   protection off (`0x18001540` bit 0), WFSYS power on (`0x18000010` = key 0x5746 | bit 7),
   acks (`0x180602cc` bit 30; `0x18001544` & 0xa0000000 = 0; `0x184c300c` bits 23, 21 = 0),
   WFSYS version `0x184b0010` = **0x02050002**, bus-hang/debug controls (`0x184f0440`, the
   AP2WF window at `0x18500000` pointed at `0x810f0000` through `0x18400120`, `0x1801d000`).
2. **The MCU's RAM image** (`CFG_POWER_ON_DOWNLOAD_EMI_ROM_PATCH = 1` for soc5_0 in
   `Kbuild.main`; `soc5_0_wlanPowerOnInit` -> `wlanImageSectionDownloadStage(IMG_DL_IDX_MCU_ROM_EMI)`
   -> `gen4m/chips/common/fw_dl.c`'s ROM-EMI branch): `soc5_0_ram_wmmcu_1_1_hdr.bin` minus its
   48-byte `ROM_EMI_HEADER` (`gen4m/include/chips/fw_dl.h`) to EMI + (`u4PatchAddr` & 0xffffff)
   = **EMI + 0x380000**, its date to EMI + 0x4954e0 (`WMMCU_MCU_ROM_EMI_DATE_ADDR`). Then the MCU
   released, and its ROM's progress word `0x18060260` polled for **0x1d1e** (1 s), FW own.
3. **Host side** (`wlanAdapterStart`): driver own (`0x18060010`: write bit 1, wait for bit 2 to
   clear; Android's log: "DRIVER OWN Done [3199 us]"), `asicConnac2xCapInit` (`0x18060018` = 1,
   `0x180241f4` = 0xffff), the host WFDMA at `0x18024000` (`halWpdmaInitRing`: off, rings,
   `soc5_0asicConnac2xWfdmaManualPrefetch`, on with `GLO_CFG |= 0x50209075`), the ROM asked for
   `TOP_HVR`/`TOP_FVR` (0x88000000/4) by `INIT_CMD_ID_ACCESS_REG`.
4. **Firmware** (`wlanDownloadFW`): LOG_TIME_SYNC, PHY_ACTION (pre-calibration: a TLV with the
   NVRAM's ePA/eLNA bytes and connfem's front-end id and pins, `chips/common/pre_cal.c`), PCCIF
   on (`0x10003220` = 0), then `WIFI_RAM_CODE_soc5_0_1_1.bin`: its 36-byte tailer (chip 0x11,
   5 regions, `____000000` of `20250122153052`) and five 40-byte region entries before it, the
   data back to back from offset 0:

   | Region | Address | Length | Feature | How |
   | --- | --- | --- | --- | --- |
   | 0 | 0x00904000 | 0x12a0 | 0 | AP2WF window (`downloadImgByDynMemMap`) |
   | 1 | 0x02201c00 | 0xc0 | 0 | AP2WF window |
   | 2 | 0x00404000 | 0x77a0 | 0 | AP2WF window |
   | 3 | 0xf04ad400 | 0xa4e70 | 0x80 | EMI + 0x4ad400 (`wlanDownloadEMISection`) |
   | 4 | 0xf05bd400 | 0x3f080 | 0x80 | EMI + 0x5bd400 |

   No DMA download, no patch, no semaphore (`downloadPatch = NULL`): the window writes are plain
   stores after pointing `0x18400120` at the region. Then `INIT_CMD_ID_DYN_MEM_MAP_FW_FINISH`
   (0x41, `{0, 0}`: no region is a RAM entry) answered by `INIT_EVENT_ID_CMD_RESULT`, and the
   ready bits `0x180600f0` [1:0] = 3.
5. **INIT commands** go to HW TX ring 17 (`kalDevPortWrite`), events come on RX ring 0. The
   running firmware's commands do **not** use a DMA ring on this chip: "SW WFDMA"
   (`fgIsEnSwWfdma = true`) copies each into a 4 x 1600-byte mailbox in the EMI (offset read from
   `0x180537fc`; default 0x2bcc00, at 0x713400 on the running phone per `iomem.txt`) and rings
   the CCIF (`0x1803d00c` = 3); answers still come on RX ring 0 (dmesg: `halSwWfdmaWriteCmd:
   Write CMD DRV[1] FW[0]`). Beacons arrive as RX data on rings 2/3; scan done is event 0x0d.
6. **Interrupts**: GIC SPI 0x1cf (WFDMA, status `0x18024200`) and 0x1de (CCIF, `0x1803d010`).

The memory Android gave CONNSYS on the running phone: EMI at 0x8f800000 (the `WIFI-SW-WFDMA` and
`WIFI-LLS` resources sit inside it), Wi-Fi DMA at 0xbe000000. All DMA addresses are 32-bit
(`soc5_0.c`: DMA mask 32, no address extension set).

## 2. What iOS needs

Our kernel has none of Apple's kexts, so there is no `IO80211Family` and no Broadcom driver:
whatever wifid talks to, we publish. Read in the 16.7.16 root (strings and disassembly of
`/usr/sbin/wifid`, IO80211.framework, SystemConfiguration, configd, IPConfiguration); HackiOS
shipped the same contract on 16.5 and the 16.7.16 binaries still carry its constants.

1. **launchd starts wifid only on a device tree with a `wlan` node**:
   `com.apple.wifid.plist` has `_LimitLoadToDeviceTree = (wlan, marconi-wifi)`; our ADT
   (`tools/mkadt_a34.py`) has neither. Add `wlan` with `local-mac-address` (MobileGestalt reads
   the Wi-Fi address there; without it each query waits up to 5 s, HackiOS main).
2. **IOKit**: a controller whose class is named `IO80211Controller` (under `IONetworkController`,
   with `IOMACAddress`, `IOBuiltin`) and a child `IONetworkInterface` with `IOInterfaceName`/
   `BSD Name` `en0`, `IOInterfaceNamePrefix` `en`, `IOInterfaceType` 6, `IOInterfaceUnit` 0,
   `IOPrimaryInterface`, `IO80211InterfaceRole` = `Infrastructure` (wifid matches on that
   property: the strings `IOPropertyMatch`, `IO80211InterfaceRole`, `Infrastructure` sit
   together in wifid). No user client: `newUserClient` refused, IO80211.framework logs
   "Falling back to IOCTL path" and uses ioctls. Plus `IOResources/IONetworkStack` with an
   openable `IONetworkStackUserClient` (configd's InterfaceNamer otherwise never reports
   `*QUIET*` and wifid/SpringBoard wait 5 minutes; HackiOS commit 2ad4bb3).
3. **The interface**: an Ethernet `ifnet` (`ifnet_allocate_extended`, family Ethernet, subfamily
   Wi-Fi, `ether_demux`/`ether_frameout_extended`, `bpfattach`); `SIOCGIFMEDIA` answers
   `IFM_IEEE80211|IFM_AUTO` (+`IFM_ACTIVE` when joined); `KEV_DL_LINK_ON/OFF` on link changes.
4. **Apple80211 ioctls** on that interface: `SIOCGA80211` 0xc02869c9 / `SIOCSA80211` 0x802869c8,
   a 40-byte `struct apple80211req` (both constants are in 16.7.16's IO80211 code). About 20
   request types matter (SSID, BSSID, channel, state, RSSI/noise, scan request and results,
   card capabilities -- 13 bytes of bits, or iOS sees no WPA2 --, power, associate and its
   result, disassociate, supported channels); the rest answer `EOPNOTSUPP`. The structure sizes
   HackiOS uses (0x1550 scan request, 0x48c scan result, 0x250 associate, 0x12d8 channels,
   0x2720 channel info) all occur as immediates in 16.7.16's IO80211.
5. **Events**: IO80211 reads `PF_SYSTEM` kernel events filtered to vendor 1 / class 6 / subclass
   1 (Apple80211): power, SSID, BSSID, link (data at least 32 bytes), association done, scan
   done.
6. **A join** from Settings: wifid stores the passphrase (securityd), sends `SIOCSA80211`
   ASSOCIATE (type 20, 592 bytes, passphrase as typed); the kernel joins and does the WPA2
   handshake itself, posts "association done", wifid reads the result (type 21), then configd
   makes the "Wi-Fi" service and IPConfiguration runs DHCP. Jobs that must run: wifid, configd,
   mDNSResponder (+Helper), trustd and securityd (or every HTTPS certificate is "invalid"),
   nsurlsessiond, CFNetworkAgent (HackiOS `userland/build.sh`).

**The shorter path, for the radio bring-up (M3)**: the same `ifnet` without the IOKit objects is
not given a service by configd, but one root-only Mach call to IPConfiguration
(`ipconfig_set_service`, `com.apple.network.IPConfiguration`, ConfigMethod DHCP) makes it the
primary interface; HackiOS measured it under QEMU (virtio-net, commit 49f9e1a): DHCP in 2.8 s,
DNS, HTTPS from NSURLSession, Maps data -- with Settings still saying Wi-Fi off. 16.7.16 still
has `__ipconfig_set_service` and that Mach service. So M3 can join from our own tool (scan,
join, WPA2 in the kernel, `setservice` from user space) and M4 adds the wifid layer, which is
about 1 100 lines HackiOS already wrote (APSL): `redwood_apple80211.c` (739),
`redwood_sta.c`/`.h` (371 + 122), `redwood_net.c`/`.h` (379 + 63), `RedwoodWLAN.cpp` (137),
`MiniIONetworkStack.cpp` (114), `redwood_wpa.c` (528). A plain Ethernet `IONetworkInterface`
(not `IO80211Controller`) would probably get an "Ethernet" service from configd by itself;
nobody has tried it, and ios-sweet's USB `ue0` (no IOKit object) was ignored.

## 3. Milestone 1, as built

**Kernel**: `xnu/patches/0940-a34-wifi-m1.patch` (written as 0939 against 0938, `543f537`; on main as 0940 after Bluetooth's 0939):
`bsd/dev/a34_wifi.c` (~1 900 lines), one line in `bsd/conf/files`, one call in
`bsd/dev/sweet_ufs.c` after `a34_mali_init()`. **Off by default**: without boot-arg
`a34wifi=N` and without a write to sysctl `kern.a34wifi.want` nothing is mapped or touched.
Steps run in order on their own kernel thread, each needing the ones before it; the first that
fails stops the sequence for the rest of the boot (`kern.a34wifi.failed`), and every line goes
to the kernel log as `A34 WIFI: ...` and to `kern.a34wifi.log`.

| Step | Does | Success line |
| --- | --- | --- |
| 1 | read only: SPM, bus protection, TOPRGU, RC mode, VCN13/18/33_1/33_2 | `step 1: CONN domain is off or protected (as expected after LK)` |
| 2 | section 1 steps 1-4: VCN18/VCN13, pins, MTCMOS, bus protection, IP version | `conn_infra IP version 0x02060002 (expected 0x02060002): CONNSYS is on` |
| 3 | section 1 step 5 and VCN33_1/VCN33_2 | `A-die chip id 0x66358a10: MT6635 E2`, `conn_infra set up in N ms` |
| 4 | Wi-Fi driver steps 1-2 (EMI cleared first) | `WFSYS on: version 0x02050002`, `WM MCU ROM done (mailbox 0x00001d1e)` |
| 5 | Wi-Fi driver steps 3-4, without LOG_TIME_SYNC and PHY_ACTION | `the ROM answers: TOP_HVR ...`, `FW_FINISH answered`, `THE WI-FI FIRMWARE IS READY` |

Fixed memory, below 4 GB and outside XNU's (which starts at 0x1_40000000): CONNSYS's EMI at
**0x50000000** (8 MB; Android's 0x780000 rounded up), Wi-Fi DMA at **0x58000000** (4 MB, of
which M1 uses 0x50000: TX ring 17 and RX ring 0, 32 slots of 4 KB each). Both are clear of
LK's reservations (`live.dts` `mblock-*`) and of the TWRP ramdisk LK loads at 0x51100000
(~36 MB; recovery image header). Everything is mapped out of `kernel_map`
(`ml_io_map_unmappable`), not the 8 MB `io_submap` that 0932 exhausted.

The CONN domain's registers are only read while SPM says it is powered and unprotected
(reading CONNSYS while it is off hangs the bus); every poll is bounded and says what it read.
PMIC writes are only the four LDOs Android's conninfra drives, to Android's voltages (1.8 V,
1.3 V, 3.3 V, 3.3 V). Nothing is written to storage; the firmware is read from the iOS root.

Checked here: the patch applies to `543f537` (`git apply --check`, `patch --dry-run`); the file
passes `gcc -fsyntax-only -Wall -Wextra -Wconversion -Werror` against stand-in declarations of
the XNU interfaces it uses (each checked against xnu-8796.141.3's headers: `ml_io_map_unmappable`
and `ml_io_unmap` in `osfmk/arm/machine_routines.h`, `vnode_lookup`/`vn_rdwr`/`rootvnode` in
`bsd/sys/vnode*.h`, `vsnprintf` in `bsd/libkern/libkern.h`, `thread_wakeup` and `lck_mtx_sleep`
outside any private guard). `tools/wifi-host-check/run.sh FW_DIR` repeats that check and then
runs the file's firmware code on the host (ASan/UBSan) with buffers standing for CONNSYS's memory,
the AP2WF window and the DMA rings: the ROM-EMI image lands at EMI + 0x380000 and its date at
+0x4954e0, regions 3/4 at +0x4ad400/+0x5bd400, region 2 in the window with `0x18400120` =
0x00404000, the FW_FINISH frame is `DW0 0x01000048 DW1 0x00010000`, CID 0x41, 0xa0, length 40,
and `GLO_CFG` ends at 0x50209075 -- byte for byte against the files. **CI built the kernel with
0940** (then numbered 0939; `xnu-a34` on branch `wifi`, run
[43678571231](https://github.com/VertexSelection/a34-ios/actions/runs/43678571231), commit
`8276cdc`, artifact `xnu-a34-kernel`): the patch applied and `a34_wifi.c` compiled with no
error; its 13 warnings are 10 `-Wcast-function-type-strict` from XNU's own `LCK_*_DECLARE`,
`SYSCTL_*` and `kalloc` macros (as in the other A34 files), 2 `-Wdeclaration-after-statement`
(as `a34_diag.c`/`a34_mali.c`) and 1 `-Wmissing-noreturn` (the worker thread).

**User space**: `userland/a34wifi` (`a34wifi up N`, `a34wifi log`, `a34wifi files`; built and
ad-hoc signed by `.github/workflows/a34wifi.yml`), and `tools/wifi-fw.sh SUPER_IMG DIR`, which
cuts `vendor` out of `super.img` (LP metadata), mounts it read-only and copies the four files
with their A346EXXSADYG1 checksums. Install: `sudo tools/root-add-job.py /mnt com.a34.wifi
/a34/wifi DIR a34wifi up 5` (a job run once at boot, output on the console, so in the kernel's
persistent log). The tool exists because the boot-args are near the 607 characters XNU keeps
(the 120 Hz Metal image uses 605).

### 0960: one CONNSYS power-up for Bluetooth and Wi-Fi

Bluetooth's 0939 (`bsd/dev/a34_connsys.c`) and Wi-Fi's 0940 each powered CONNSYS on their own,
so they could not run in one boot. `xnu/patches/0960-a34-connsys-shared.patch` (against
`e927304`) makes `a34_connsys.c` the one owner of the PMIC rails and of the A-die's set-up:

- `a34_connsys_get(radio)` / `a34_connsys_put(radio)` (radio = conninfra's `CONNDRV_TYPE_*`:
  0 Bluetooth, 3 Wi-Fi), counted, under one lock; `a34_connsys_on()` stays Bluetooth's get, so
  `a34_bt.c` is unchanged.
- The first radio of a boot gets the whole first power-on with its own choices
  (`conninfra/.../mt6877_pos.c`): the sleep controller's A-die table and the A-die clock buffer
  "Bluetooth only" (`ON3/4`, `OFF3/4` = `0x74e03f75`... ; XTAL `0x100`/`0x70ff0000`) only when
  that radio is Bluetooth, else the full values (`0x74e0fff5`...; XTAL `0xc180`/`0x50550080`);
  its bit in sysram's radio status; Bluetooth's VCN13/VS2 raise only for Bluetooth
  (`consys_plt_pmic_raise_voltage_mt6877` acts on `drv_type == 0` only).
- A later radio gets `consys_hw_pwr_on()`'s `curr_status != 0` path: conn_infra woken (its TOP
  wake-up bit, version polled), the radio's status bit, the sleep table and the A-die clock
  buffer redone for the radios now on, conn_infra let sleep again.
- Each radio's rails: Bluetooth VCN33_1 (its `EN_0`); Wi-Fi VCN33_1's `EN_1` (`MULTI_SW` bit 15)
  and VCN33_2 at 3.3 V (`consys_plt_pmic_wifi_power_ctrl_mt6877`, RC mode as the rest).
- `put` only drops the count: nothing is powered off before a reboot.
- Wi-Fi's steps 2 and 3 are now `a34_connsys_get(Wi-Fi)` and a check from Wi-Fi's side (its own
  wake-up bit `0x180601a4`, `a34_connsys_readable()`, the A-die id, the radio status, the LDOs);
  Wi-Fi uses CONNSYS's one memory region (`a34_connsys_emi_pa()`, default 0x60000000,
  `a34btemi=`) and clears only its own parts of it (log 0x465400 + 192 KB, dump 0x495400 +
  0x18000), since Bluetooth's images live there too. Its DMA memory stays at 0x58000000.
- Gained from 0939: the EMI-MPU region over CONNSYS's memory (SiP SMC, `a34btmpu=1`), which was
  unknown 3 below. Lost: 0940's legacy (non-RC) LDO branch -- `a34_connsys.c` is RC mode's, and
  step 1 says which mode the phone is in.

### The first phone test

Kernel with 0940 (and 0960, below), root with the `com.a34.wifi` job, **no new boot-arg**. Cautious order:
`a34wifi up 1` (read only), then `up 3` (CONNSYS on, A-die found), then `up 5`; each is one
boot, or one run of the job's command by hand. What to read in `tools/pull-log.sh`'s log
(`grep 'A34 WIFI'`):

- step 1: `CONN_PWR_CON` with bits 2/3 clear and `PWR_STATUS` bit 1 clear (domain off), the RC
  mode, the four LDOs (VCN18 is likely on already: it is shared);
- step 2: `CONN MTCMOS on: CONN_PWR_CON 0x...d` (RST_B, PWR_ON, PWR_ON_2ND), then
  `IP version 0x02060002`. A hang here (no further line, phone frozen) means a CONNSYS read
  with the bus still protected or the domain unpowered: report the last line;
- step 3: `A-die chip id 0x66358a10` (or `8a11`), the efuse words, `conn_infra set up`;
- step 4: `WFSYS on: version 0x02050002`, the ROM-EMI copy, `WM MCU ROM done`;
- step 5: `driver own in N ms` (Android: ~3 ms), `the ROM answers: TOP_HVR 0x...`, five region
  lines, `FW_FINISH answered`, **`THE WI-FI FIRMWARE IS READY`** = milestone 1.

### Milestone 2 (0961): the firmware's commands and one scan -- built, not yet run

`xnu/patches/0961-a34-wifi-m2-scan.patch` (on top of 0960) adds steps 6 and 7 to `a34_wifi.c`
(same switches: `a34wifi=7` or `a34wifi up 7`; `a34wifi scan` = `kern.a34wifi.rescan=1` scans
again). Read from `gen4m_s1` and checked against Android's dmesg where it shows the same thing:

- **Command channel** ("SW WFDMA", `os/linux/hif/common/sw_wfdma.c`; soc5_0's `fgIsEnSwWfdma = true`,
  `fgIsEnAfterFwdl = false`): a mailbox in CONNSYS's memory at `0x180537fc[23:0]` (the ROM's;
  0x713400 on the running phone per `iomem.txt`'s `WIFI-SW-WFDMA`, default 0x2bcc00): `u32 drv_idx`
  (host), `u32 fw_idx` (firmware), 4 slots of 1600 bytes. Step 5 now zeroes it before the RAM code,
  as `halHifSwInfoInit` -> `halSwWfdmaInit`/`halSwWfdmaReset` do. A command (the same 32-byte TXD +
  `CONNAC2X_WIFI_CMD` frame as on a ring: CID +36, 0xa0, set/query +38, sequence +39, payload +64)
  is copied into slot `drv_idx`, `drv_idx` advanced, then 3 written to the CCIF's TCHNUM
  (`0x1803d00c`); full when `(drv_idx + 1) % 4 == fw_idx`. INIT commands (M1) stay on HW TX ring 17
  (`asicCapInit`: `u2TxInitCmdPort = TX_RING_CMD_IDX_3`, `gl_init.c:5307`, before the download).
- **Answers** on RX ring 0, polled (Android polls too until its interrupts are on): a 24-byte RXD,
  then `WIFI_EVENT` (EID +4, sequence +5, payload +12); RXD `DW0[31:27] = 7` with
  `DW0[31:16] & 0x380f` = 0x3800 for an event, 0x3801 for an 802.11 frame the firmware forwards.
  The CCIF's WF-to-AP channels (`0x1803d010`) are acknowledged (`0x1803d014`) as Android does.
- **Step 6**: `GET_NIC_CAPABILITY_V2` (0x8a, query) -> event 0xec (TLVs: hardware, firmware version,
  MAC in the chip's efuse (tag 7), A-die), then `BASIC_CONFIG` (0x02, 12 zero bytes). MAC: the
  efuse's if it has one, else `02:41:33:34:57:46` (locally administered); the phone's NVRAM is
  neither read nor written.
- **Step 7**: `BSS_ACTIVATE_CTRL` (0x11: BSS 0, AIS, own-MAC slot 0, broadcast WTBL 4), then
  `SCAN_REQ_V2` (0x03, 1192 bytes: Android's dmesg shows `LEN[1256]` = 64 + 1192) **passive** --
  nothing is transmitted by an uncalibrated radio -- over an explicit list (`ucChannelType` 4:
  2.4 GHz 1-13, 5 GHz 36-48 and 149-165, so no country setting is needed), `ucScnFuncMask` 0x02
  (DBDC off, `wifi.cfg DbdcMode 0`); RX ring 0 drained for up to 20 s until `SCAN_DONE` (0x0d, 476
  bytes; dmesg: "size of ScanDone476"); then the BSS deactivated. Beacons and probe responses come
  **forwarded on RX ring 0** (0x3801; the dmesg's counters moved on the event ring, not the data
  rings, during scans): RXD, then its groups (`DW1[15:11]`: G4 16, G1 16, G2 8, G3 8, G5 72 bytes in
  that order), `DW2[15:14]` x 2 bytes of padding, the 802.11 frame; channel `DW3[15:8]`; RCPI in
  G3's second word, dBm = min(RCPI, 220) / 2 - 110 (`nic_rxd_v2.c`, `scan.c`). Each network heard is
  one log line: BSSID, channel, dBm, RSN/WPA/WEP/open, SSID.
- **Structures**: copied field by field into a host program from `include/wsys_cmd_handler_fw.h`
  and `nic_cmd_event.h` (none is packed) and their offsets printed; `tools/wifi-host-check` then runs
  the code on synthetic RX entries (a forwarded beacon with an RX vector, a SCAN_DONE) and checks the
  mailbox frame byte for byte.
- **Built in CI**: `xnu-a34` on branch `wifi` with 0939, 0940, 0960, 0961
  ([43681034639](https://github.com/VertexSelection/a34-ios/actions/runs/43681034639); 0960 alone:
  [43679809066](https://github.com/VertexSelection/a34-ios/actions/runs/43679809066)): every A34
  patch applied, `a34_connsys.c`, `a34_bt.c`, `a34_wifi.c` compiled, no errors; the warnings are of
  the kinds XNU's own macros give every A34 file.

**First M2 test** (after M1 has passed; Wi-Fi only, without `a34bt`): kernel with 0939, 0940, 0960
and 0961; the `com.a34.wifi` job running `a34wifi up 7`; then `grep 'A34 WIFI\|A34 CONNSYS'`:

- `step 5: the running firmware's command mailbox (SW WFDMA) at EMI +0x713400 (the ROM's ...)`
  (another offset is fine if it is the ROM's), then `THE WI-FI FIRMWARE IS READY`;
- `step 6: the firmware answers: NIC_CAPABILITY_V2, N elements`, the firmware's version and date
  (Android's tailer says `20250122153052`), the MAC line;
- `step 7: passive scan 1 of 22 channels asked`, `SCAN_DONE in N ms` (a few seconds), and one line
  per access point near the phone. `a34wifi scan` repeats it.
- If step 6 says `NOT ANSWERED` with `mailbox drv 1 fw 0`: the firmware never took the command
  (the doorbell or the mailbox offset); with `fw 1` and no event: the answer path (RX ring 0).
  If `SCAN_DONE` comes with nothing heard: PHY_ACTION/NVRAM (below) are next.

### M2 on the phone (kernel 0961-radio, `a34bt=1` in the same boot, `a34wifi up 7`)

From `build/logs/0961-radio-a` (`grep 'A34 WIFI'`): CONNSYS was already up for Bluetooth, step 2
only added Wi-Fi's VCN33_1/VCN33_2 (both "on" in step 3's report); the A-die read `0x66358a11`
(MT6635 E3); WM MCU ROM done 62 ms after its release; the mailbox at EMI +0x713400 (the ROM's);
FW_FINISH answered and **the firmware ready 780 ms after it**; NIC_CAPABILITY_V2 answered (20
elements, 492 bytes) -- its hardware/firmware-version tags read as zeros (the same TLV layout as
`nic_cmd_event.c` parses; the firmware leaves them empty), **the efuse MAC is 00:00:00:00:00:00**;
the passive scan of 22 channels finished in 1149 ms with **7 networks heard on 2.4 and 5 GHz**
(-70 to -97 dBm, RSN and open). So the firmware receives without PHY_ACTION and NVRAM.

### 0963: the phone's own MAC address

The efuse has none and Android's comes from `/efs` (`.mac.info`), which we never read. Each phone
now keeps its own on its iOS root: `/a34/wifi/mac`, one line `xx:xx:xx:xx:xx:xx`, a random
locally administered unicast address that `a34wifi` makes the first time (`a34wifi mac`, and
`a34wifi up N` for N >= 6) and never changes; the kernel reads it in step 6 (`a34w_mac_choose`:
the file, else the efuse's, else a random one for that boot only, said so in the log). Two A34s on
one network no longer share `02:41:33:34:57:46`. `root-add-job.py` never deletes files in
`/a34/wifi`, so the address survives reinstalls.

### 0964: milestone 3, joining (step 8)

What gen4m_s1 does on soc5_0 to join as a station, read in the source (file:line references
are under `connectivity/wlan/core/gen4m_s1/`) and done the same way, host side where Android is:

- **The network**: `/a34/wifi/network`, written by the phone's owner (`ssid=`, `psk=` a
  passphrase of 8-63 characters or 64 hex digits, optional `bssid=`); never in the repository;
  the passphrase never reaches the log (only its length) and is wiped once the PMK is made.
- **The target**: step 7's passive scan, run with the BSS kept active (HakiOS run 457: a scan
  that deactivated BSS 0 killed a connection); the loudest BSS of that SSID wins; its beacon's
  elements are kept (rates, RSN, TIM). WPA2-PSK needs CCMP for both keys and AKM 2 without
  "PMF required" (a WPA2/WPA3 transition network is fine).
- **Channel**: `CH_PRIVILEGE` (0x1c, 24 bytes: JOIN, 4000 ms, DBDC auto) and its grant
  `EVENT_ID_CH_PRIVILEGE` (0x10), token matched; a new token per request (HakiOS run 449). Held
  through the handshake and given back after it (ais_fsm.c holds it until the port is authorized).
- **The AP's station record**: `UPDATE_STA_RECORD` (0x13) is **168 bytes, version 0** -- the
  A34's module is built without 802.11ax (soc5_0 never sets `CONFIG_MTK_WIFI_11AX_SUPPORT`;
  `cnmStaSendUpdateCmd` in the shipped .ko moves 0xa8) --, type 0x41 (legacy AP), **WTBL 5**
  (0-3 are BIP's, 4 the broadcast entry, `privacy.c:808-842`), broadcast field 0xff, legacy rates
  only (no HT/VHT elements are sent: 54 Mb/s at most, enough for a first connection).
- **Authentication and association**: 802.11 frames built here, sent as `nicTxCmd` sends
  `COMMAND_TYPE_MANAGEMENT_FRAME` on soc5_0: **a CONNAC2X TXD and the frame in the SW-WFDMA
  mailbox** (not a ring: `soc5_0.c:419`; HakiOS's MT6768 used HW ring 15). TXD: byte count, EtherType
  offset 16, packet format 2, MCU queue 1; WTBL 5, VTA, 802.11 header of 24, long format; lifetime
  2 s, 30 tries, a sequence number, the BSS's lowest basic rate fixed (CCK 1 M / OFDM 6 M), its TX
  status asked for (PID). The host check holds the Authentication TXD against
  `nic_txd_v2_compose`'s byte for byte (`0310003e 80026405 801e000b 8001f000 0 00000201 0 000bc000`).
  The answers come forwarded on RX ring 0 (0x3801), and `EVENT_ID_TX_DONE` (0x0f) says whether the
  AP acknowledged each frame (`TX status N: acknowledged` in the log) -- the first proof that the
  radio transmits.
- **Connected**: `SET_BSS_INFO` (0x12, 116 bytes: connected, BSSID, rates, open 0/1 or WPA2-PSK
  7/CCMP 6, BMC WTBL 4, its RLM part for 20 MHz), the station record again in state 3 with the AID
  (answered by sequence), `INDICATE_PM_BSS_CONNECTED` (0x16), `POWER_SAVE_MODE` CAM (0x05; HakiOS
  run 459: without it the AP thinks the station asleep after ~80 s), the RX filter (0x0a: directed,
  multicast, broadcast).
- **WPA2**: the four-way and group handshakes on the host -- the firmware has no 4-way offload
  (`gl_init.c`) -- with `bsd/dev/a34_wpa.c`, HakiOS's `lx_wpa.c` (MIT; its tests, against the
  standards' vectors and an independent Python access point, run in `tools/wifi-host-check`: 195
  checks). **EAPOL goes as data on TX ring 0**, not by command (`gl_kal.c:4161`: "No more sending
  via cmd"), at the lowest basic rate, never protected for pairwise messages; message 1 comes on RX
  ring 2 (data). Keys: `ADD_REMOVE_KEY` (0x07, 64 bytes, no answer): pairwise to WTBL 5 with TX,
  group to WTBL 4 with its index, CCMP (4), 16 bytes.
- **Data rings** (needed for EAPOL): step 5 now sets up, before the DMA is on, TX ring 0 (64
  slots), RX rings 2/3 (64 x 2352) and 4/5 (16 x 3840, the token reports), and the DMA scheduler
  as `soc5_0DmashdlInit` does (PLE max 1 page, groups 0-2 quota 0x10..0x200 with refill, the
  queue-to-group map, ack threshold 3). A frame on TX ring 0 is a token buffer -- TXD, TXP
  (`fillTxDescAppendByHostV2`: the token, the frame's address and length) and the Ethernet frame,
  the descriptor pointing at the first 64 bytes --; the token stays the DMA's until its report
  (RX ring 4, `HW_MAC_MSDU_REPORT` version 3) gives it back.
- **After joining** the thread services RX ring 0 and the data rings every 2 ms (rekeys, the AP's
  deauthentication, data). A failed join can be asked for again (`a34wifi join`); after the link
  is lost, `kern.a34wifi.rejoin` (also `a34wifi join`) joins again.
- **Not sent, on purpose**: PHY_ACTION, NVRAM (0x48), domain info. On this phone Android's own
  COM_FEM tag is all zeros (connfem never binds on MT6877, `connfem.cfg` is empty), the device
  tree's FEM pin groups are reference-board leftovers (GPIO188 is the GPS LNA, GPIO195 the
  vibrator) and must not be applied, and the firmware has fallbacks for both ("NVRAM control is
  invalid, FW will use default", "Rom cal unfinish, do SW ch cal"). If the AP never acknowledges
  our frames (`NOT acknowledged` TX statuses), 0966 is the next try, without a new kernel.

### 0966: PHY_ACTION, opt-in

Boot-arg `a34wifiphy=1`, or `kern.a34wifi.phyaction=1` before step 5 (`a34wifi` sets it when the
job has `A34WIFI_PHYACTION=1`): between the ROM's ACCESS_REG and the RAM code, INIT command 8 as
Android's `wlanPreCal` sends it when it has no calibration of its own to give back
(`chips/common/pre_cal.c:576-645`, layout `include/nic_init_cmd_event.h:239-291`): `{0x556789aa,
3 tags, version 1, length}`, CAL `{force calibration}`, NVRAM (tags 7-9 of the vendor's default
`/vendor/firmware/WIFI` -- installed by `tools/wifi-fw.sh`, never the phone's `nvdata` copy --
cut as `wlanGetEpaElnaFromNvram` cuts them: 260 bytes) and COM_FEM (104 zero bytes, as Android's
own on this phone); 388 bytes in all, answered by INIT event 5 (event 0x81 status 0: calibrated).
The step goes on either way. Off by default: steps 1-7 ran without it.

### 0965: the data path (step 9)

`bsd/dev/a34_wifi_net.c`: an Ethernet interface made with XNU's interface KPI (legacy model, no
Skywalk nexus, family Ethernet, subfamily Wi-Fi, media IEEE 802.11, link events to configd),
the shape of HackiOS's `redwood_net.c` (APSL), named `enN` with the first free unit
(`kern.a34wifi.ifname`). Frames from the stack go to TX ring 0 (ciphered once the keys are in);
data from RX ring 2 -- header-translated by the hardware, or 802.11 + LLC/SNAP turned into
Ethernet here -- go up; the link follows the join. An address comes from user space: `a34wifi
dhcp` asks configd's IPConfiguration plugin to run DHCP on the interface (`ipconfig_set_service`,
message 20011, as HakiOS's `net_test.m`, MIT), which also publishes the router and DNS servers to
the rest of iOS; it needs `com.apple.configd` running. `a34wifi tcp HOST [PORT]` then tries a TCP
connection and an HTTP HEAD.

**Built in CI**: `xnu-a34` on branch `wifi` with every patch through 0980 (0963-0966 applied in
numeric order before 0970/0971/0980; they also apply on top of them):
[43718845503](https://github.com/VertexSelection/a34-ios/actions/runs/43718845503); 0964 alone
[43716869237](https://github.com/VertexSelection/a34-ios/actions/runs/43716869237), 0965
[43717483182](https://github.com/VertexSelection/a34-ios/actions/runs/43717483182). No warnings of
our own (only the kinds XNU's macros give every file). `a34wifi`:
[43717560704](https://github.com/VertexSelection/a34-ios/actions/runs/43717560704).
`tools/wifi-host-check`: everything above on the host (the open join against a simulated firmware
and AP, EAPOL and data both ways, the token reports, PHY_ACTION from the real `WIFI`), 195 WPA checks.

**The M3 test** (kernel with 0963-0966 on top of main's patches, 0966 off; `a34bt=1` may stay):

1. On the host, once: write the network file and install it readable by root only, e.g.
   `printf 'ssid=Name\npsk=passphrase\n' > network; sudo install -m 600 -o 0 -g 0 network
   /mnt/a34/wifi/network` (not through `root-add-job.py`, which makes every file 0755).
2. The `com.a34.wifi` job runs `a34wifi connect` (= `up 9`, `dhcp`, `tcp 1.1.1.1 80`; it makes
   `/a34/wifi/mac` first and checks the network file; DHCP needs `com.apple.configd` running).
3. `grep 'A34 WIFI'`: step 6's MAC line says "kept in /a34/wifi/mac"; step 8 says `network
   "..."`, the target (BSSID, channel, dBm), `channel N granted`, `Authentication sent`, then
   either `TX status 1: acknowledged` and `authenticated` -- the radio transmits -- or `NOT
   acknowledged` (TX: PHY_ACTION next) / `no answer` with acknowledged (the AP refused us: its
   reason follows); `ASSOCIATED, AID n`; for WPA2 `EAPOL: message 1 of 4`, `EAPOL out on TX ring 0`,
   the token report's first line, `message 3 of 4`, `the four-way handshake is done`; `JOINED`;
   step 9 `enN attached`; every 30 s a `carried:` line with frame counts.
4. If the TX statuses say `NOT acknowledged`: boot again with `a34wifiphy=1` (or the job's
   `A34WIFI_PHYACTION=1`); step 5 then says `PHY_ACTION ... event 0x81 status 0`.

### The first join on the phone (kernel 0988, `a34wifi connect`; `build/logs/0988-all-a`)

Step 8 joined veryCoolNetwork's 5 GHz BSS (channel 48, -74 dBm) as a **legacy** station: channel
granted in 340 ms, Authentication and Association acknowledged at the first try, AID 20, the
four-way handshake done (pairwise key WTBL 5, group key 2 WTBL 4), JOINED (WPA2-PSK, CCMP) in
2794 ms, en0 attached. Then **the AP disassociated us, reason 37** ("the peer does not want to use
the mechanism"), before any DHCP frame went out (the DHCP request came 60 s later, after the
loss). Nothing before it was in the log: action frames were dropped unread. What the sources say
(`gen4m_s1`): the firmware answers ADDBA itself and, for a station record without HT, drops the
AP's request (`que_mgt.c:7810-7817`, the RAM code's `mqm_ba` strings); and this AP steers its
clients on a ~60 s cycle with 802.11v BSS transition requests (Android's dmesg: three, all answered
"no suitable candidate", never thrown out). A legacy, non-QoS client that cannot be asked to move
is what such an AP throws out. So 0967 (see what the AP does) and 0968 (join as Android does).

### 0967: what the AP does after the join, said; leaving and coming back

- Every action frame from the AP is said (category, action, its first 24 bytes; BSS transition
  requests decoded), the disassociation/deauthentication frame whole, ADDBA/DELBA/beacon-timeout
  events, the target's element list at the join. An ADDBA request that reaches the host is
  declined (status 37), as mac80211 does without aggregation.
- The first 48 data frames either way are traced: EtherType, IP protocol and ports, the DHCP
  message type, ciphered or not (`kern.a34wifi.trace` for more); ARP and DHCP go out with their TX
  status asked for, so "TX status N: acknowledged" says the AP took them.
- When the AP lets go (or the beacons stop), the firmware's side is undone as Android's disconnect
  does it (PM abort, BSS info disconnected, STA record removed) and the network is joined again by
  itself after 3 s, 5 times a boot at most; the BSS that let us go scores 20 dB less for 5 minutes
  (this AP is dual-band and steers). Step 8 always scans twice (one scan missed the 2.4 GHz BSS).

### 0968: an HT and WMM station that can be asked to move

As Android joins (`assoc.c`, `rlm.c`, `cnm_mem.c`, `que_mgt.c`, `wnm.c`), each part only if the AP
has it: the Association Request carries HT Capabilities (HT20, one stream: LDPC, SM power save
off, short GI 20, RX STBC; A-MPDU 64 KB; MCS 0-7), Extended Capabilities with BSS transition (bit
19) and the WMM Information element; the AP's station record is HT and QoS (its own HT
capabilities from its Association Response, the 40 MHz bits cleared; A-MPDU both ways, block ack
64/64, no A-MSDU in A-MPDU; RTS policy legacy); SET_BSS_INFO is QBSS and HT, its RLM part from the
AP's HT Operation at 20 MHz; the AP's WMM parameters go by `UPDATE_WMM_PARMS` (0x1d) before it. A
BSS transition request is answered with status 7 (no suitable candidate), with an MBO rejection
reason when it carried MBO -- what Android answered this AP. The firmware now has an HT record for
the AP, so it takes the AP's block ack requests itself. `kern.a34wifi.legacy=1` (boot-arg
`a34wifilegacy=1`) joins as 0964 did, for comparison. Not yet: VHT/80 MHz and two streams.

### 0969: iOS's Wi-Fi side (milestone 4)

What wifid (and through it Settings and the status bar) needs, after HackiOS (`redwood_apple80211.c`,
`RedwoodWLAN.cpp`, `MiniIONetworkStack.cpp`, APSL 2.0), on our own station boundary (`a34_sta.h`):

- `iokit/Kernel/arm/A34WLAN.cpp`: an `IO80211Controller` (IOMACAddress) under the platform, below
  it an `IONetworkInterface` with `IOInterfaceName`/`BSD Name` = the interface and
  `IO80211InterfaceRole` = Infrastructure, without a user client (IO80211.framework then falls
  back to ioctls); and `IOResources/IONetworkStack` with a user client that can be opened, which
  configd's interface namer waits for before it says the registry is quiet (until then
  `SCNetworkInterfaceCopyAll`, wifid and SpringBoard wait up to five minutes). Made when the
  interface is, which is now as soon as the firmware is up (after step 7), joined or not.
- `bsd/dev/a34_apple80211.c`: the Apple80211 ioctls (`SIOCGA80211`/`SIOCSA80211`, reaching the
  interface's ioctl): card capabilities (WPA2, WME, AES-CCM), power, op mode, channels (ours),
  scan request and results (one per request, with the beacon's elements: wifid reads the
  protection out of them), associate (the name and the passphrase as typed, or the key), the
  association result (1, 21 wrong passphrase, 9 timeout, 2 else), disassociate, state, SSID,
  BSSID, channel, RSSI, the current network; and the kernel events (class 6, subclass 1: power,
  SSID/BSSID/link changed, association done, scan done). Every request is said in the log (who,
  which, the length; never the name or the key).
- `a34_wifi.c`: the station boundary: requests queued to its thread (a scan, a join -- the same
  join as step 8's, of the network iOS names --, leaving, power), the last scan's results kept
  with their elements, and every link change told to both the interface and iOS. A join that is
  lost is joined again (0967) with the network last asked for, whoever asked.
- wifid's job names a device-tree node (`_LimitLoadToDeviceTree`: wlan, marconi-wifi) that our
  ADT does not have: `tools/root-add-job.py --unlimit ROOT com.apple.wifid` removes it (HackiOS's
  `mkjobs.py` does the same). HackiOS's Settings join also needed configd, mDNSResponder (+Helper),
  trustd, securityd, nsurlsessiond and CFNetworkAgent running.
- The host check runs wifid's side end to end: the capabilities, power, a scan through the
  simulated firmware and its results, a join with the name alone, ASSOCIATE_RESULT 1, STATE 4,
  the events in order.

**Built in CI** with every patch through 0988 (0963-0969 apply in numeric order before 0970):
0967 [43724219300](https://github.com/VertexSelection/a34-ios/actions/runs/43724219300), 0968
[43725478583](https://github.com/VertexSelection/a34-ios/actions/runs/43725478583), 0969
[43726426826](https://github.com/VertexSelection/a34-ios/actions/runs/43726426826) (A34WLAN.cpp and
a34_apple80211.c compiled and linked, no warnings of our own); `a34wifi`
[43726426380](https://github.com/VertexSelection/a34-ios/actions/runs/43726426380).

**The next test** (kernel with 0963-0969 and the rest of main; network file as before):

1. Job `a34wifi connect` as before. Expect at step 8 `joining as an HT (20 MHz, one stream) QoS
   (WMM) station ...`, `its elements: ...`, then JOINED; then `the AP sent an action frame ...`
   lines -- a BSS transition request should be answered (`BSS Transition Management Response
   sent`) -- and no `LEFT the network`; `out ... DHCP DISCOVER (ciphered ...)`, `TX status N:
   acknowledged`, `in ... DHCP OFFER`, `en0: IPv4 address ...`, then `a34wifi: tcp 1.1.1.1 ...
   connected`. If the AP still lets go, the frame and what came before are in the log, and it
   comes back by itself 3 s later.
2. With that working, iOS: `root-add-job.py --unlimit /mnt com.apple.wifid` and the jobs above;
   grep `A34 WIFI: apple80211` for wifid's requests, `A34WLAN` and `IONetworkStack opened by
   configd`; Settings > Wi-Fi should list the networks, show the joined one, and join another
   with a typed passphrase (`iOS asks us to join`).
3. If 0968 makes things worse: boot-arg `a34wifilegacy=1` is 0967's behaviour.

### On the phone: online (kernel main 33f1f28 + 0991, `a34wifi connect`; `build/logs/0991-all-a`)

Joined as an HT (20 MHz, one stream) QoS station on channel 5 in 4843 ms; DHCP DISCOVER out
ciphered, OFFER and ACK in, en0 192.168.1.x by DHCP (through our IPConfiguration `setservice`:
configd made no Wi-Fi service of its own in the first 10 s), a TCP connection to 1.1.1.1:80
answered "HTTP/". BSS transition requests came and were answered (we stay, status 7); no
disassociation. Of iOS's side, already: A34WLAN in the registry, `IONetworkStack opened by
configd`, configd asking Apple80211 (SSID, BSSID, auth type, capabilities). wifid was not started
(its job's `_LimitLoadToDeviceTree`). And MobileGestalt (in SpringBoard) looked for the Wi-Fi
address: services named `sdio`, `wlan`, `marconi-wifi` (none), then a wait for an
`IONetworkController` to be published (never: "class-match IONetworkController -> 0").

### 0995: "wlan", an IONetworkController with the address; and Settings' root

- The controller is now named `wlan`, carries the address as `local-mac-address`,
  `mac-address-wifi0` and `IOMACAddress`, and is an `IONetworkController` (the baseline's class,
  `SweetNetwork.cpp`, declared word for word), as IO80211Family's controllers are: MobileGestalt's
  look-up and its wait both find it.
- No log line says a passphrase's or key's length any more (step 8, the Apple80211 join line,
  a34wifi's check); the passphrase itself never was in one: it comes from `/a34/wifi/network` or
  from Settings, and wifid keeps Settings' in the keychain as on any iPhone.
- `tools/wifi-settings-setup.sh ROOT` (idempotent): removes wifid's `_LimitLoadToDeviceTree`;
  checks configd, mDNSResponder (+Helper), trustd, securityd, nsurlsessiond and CFNetworkAgent --
  all stock jobs of our root, configd and mDNSResponder started at boot, the rest on demand by
  their Mach services (no `exec` line until something asks), none disabled -- and stops, changing
  nothing, if one is missing; leaves the `com.a34.wifi` fallback job; makes `/a34/wifi/network`
  root's, mode 0600, without reading it.

Built in CI with every patch through 0995:
[43729951490](https://github.com/VertexSelection/a34-ios/actions/runs/43729951490) (no warnings of
our own); `a34wifi` [43729951074](https://github.com/VertexSelection/a34-ios/actions/runs/43729951074).

**The Settings test**: kernel with 0995; `sudo tools/wifi-settings-setup.sh /mnt`; keep the
`a34wifi connect` job. In the log: `A34WLAN: en0 in the registry ("wlan": ...)`, `exec wifid`,
`A34 WIFI: apple80211: wifid gets/sets ...` (the request list says what wifid needs: a REFUSED
type is one to answer next), MobileGestalt's `IONameMatch wlan -> ` the service. In Settings >
Wi-Fi: the switch, the networks around (wifid's scans: `scan for iOS`), the joined network ticked;
joining another network with a typed passphrase (`iOS asks us to join "..."`, then JOINED).

### 0999: the registry goes quiet (stuck at the logo with wifid: `build/logs/0995-wifi-a`)

(Built on the branch as 0997; on main it is `xnu/patches/0999-a34-iokit-quiet.patch`, main's 0997
and 0998 being the Mali job lock and the display pacing.)

With wifid started (0995 and the setup script) the phone stayed at the Apple logo. The kernel's
sampler (baseline patch 0054, `SMP` lines; symbolized with `ipsw dyld a2s` against
`research/metal/dsc` and the cache slide it prints) shows SpringBoard's main thread, and a second
one from `SBWiFiManager`, in `wifi_manager_open` < `WiFiManagerClientCreate` (MobileWiFi): a
synchronous request to wifid, never answered. wifid had made its first Apple80211 requests
(capabilities, power, channels) and then nothing; and the log already said why it would wait:
`IONetworkStack opened by configd; the registry's busy state is 1`. XNU's `StartIOKitMatching`
takes a busy count on the registry root for the IOKit daemon, given back only when
kernelmanagerd says it has launched (`IOService::iokitDaemonLaunched`); our root has no
kernelmanagerd (no job), and `OSKext::iokitDaemonAvailable()` says yes unless `-restore`. So the
registry never went quiet, configd's interface namer never said "quiet", wifid waited for that
(SCNetworkInterfaceCopyAll), and SpringBoard for wifid. (ios-topaz's notes record the same:
"IOKit never quiesced, wifid hung boot".) Without wifid, MobileWiFi's open failed at once, which
is why every boot before reached the home screen.

0999 (`iokit/Kernel/IOStartIOKit.cpp`, `IOService.cpp`, diffed against 8796.141.3 with the
baseline's 0021/0022/0083): on this board the hold is not taken (boot-arg `a34iokithold=1` takes
it as XNU does), and `iokitDaemonLaunched` gives back only a hold that was taken (built in CI with
every patch of main: [43733929245](https://github.com/VertexSelection/a34-ios/actions/runs/43733929245)). Expected in the
log: `the registry's busy state is 0` when configd opens IONetworkStack. Next boot with wifid:
boot-arg `sweetsample=SpringBoard,backboardd,wifid` (the sampler's list: first sample at 90 s)
shows wifid's own threads if it still waits on something. Undo for the root:
`sudo tools/wifi-settings-setup.sh --undo ROOT` (`root-add-job.py --relimit ROOT
com.apple.wifid`: `_LimitLoadToDeviceTree` back to its stock `["wlan", "marconi-wifi"]`).

### Settings works (kernel main with 0995 + 0999; `build/logs/0999-wifi-a`)

Wi-Fi in Settings, and Safari (YouTube's page) over it. What is left of the log (bluetoothd
flooded it) shows ~97 MB received, 3.4 MB sent, every token given back -- and **4264 of ~27 000
frames dropped on the way out**: with 64 tokens in flight (a token comes back only when the
firmware reports the frame sent) a burst from the stack found none free and was dropped. One
BSS transition request was answered (we stay). The system never sleeps on this port (only "SWEET
FB: display off"), so "sleep/wake" is the display turning off with the link kept.

### 1010: robustness (built as one patch; numbers 1010-1019 are Wi-Fi's)

- **TX without drops**: 256 tokens and ring slots (Android's 256), their buffers after RX ring 5's;
  with none free the sender takes in the token reports itself (rings 4/5 under their own lock) and
  waits up to 5 ms before dropping; drops are counted by cause in the `carried:` line.
- **Coming back**: after the AP lets us go or the beacons stop, joining again never gives up while
  the network is wanted -- 3, 10, 30 s, then every 60 s -- and stops when iOS asks to leave or
  switches Wi-Fi off. (wifid also rejoins known networks by itself.)
- **Roaming by BSS transition**: the AP's candidates (Neighbor Report elements, preference
  subelement) are read; we go -- BTM Response status 0 with the target BSSID, then leave and join
  again preferring that BSS -- when the AP says it will disassociate us (mode bit 2), or when a
  candidate we heard in the last two minutes is 8 dB louder than the AP is now and above -75 dBm;
  otherwise status 7, as Android answered this AP (accepting every steer would move us between the
  bands every minute, each move a few seconds without a link). How loud the AP is now comes from
  the RX vector (group 3) of its data frames, averaged; wifid's RSSI and the status bar get that.
- **Hidden networks**: a join whose name two passive scans did not hear probes for it (an active
  scan for that name: ucScanType 1, SSID type 4, the name in arSSID[0]); a scan wifid asks for with
  a name is that active scan too.
- **Power save**: the firmware does 802.11 power save itself (PS-Poll, the PM bit, DTIM wake-ups,
  keep-alive, ARP offload: the RAM code's strings) once it has the DTIM and our AID
  (INDICATE_PM_BSS_CONNECTED, sent at the join). We join in CAM and go to FAST_PSP (Android's: dozes,
  awake while traffic flows) 15 s later, after the handshake and DHCP; an AP without a TIM element
  keeps us in CAM, as gen4m does. `kern.a34wifi.powersave` / boot-arg `a34wifips`: -1 automatic,
  0 CAM, 1 MAX_PSP, 2 FAST_PSP. (The ~80 s stall another MediaTek port saw in PS came from the
  firmware dozing without a correct DTIM/AID; we send both before leaving CAM.)
- **The Wi-Fi switch**: off leaves the network, stops rejoining and refuses scans/joins; on tells
  wifid (power event) and lets it scan and join again. The firmware stays up (no re-run of steps
  4-5).

`a34wifi get [URL] [SECONDS]` times an HTTP GET (default `http://speedtest.tele2.net/10MB.zip`) and
prints Mbit/s, for the throughput work.

### Biggest unknowns

0. **Whether a scan hears anything without PHY_ACTION and NVRAM** (M2): Android sends both
   before its first scan; M2 sends neither (passive, so nothing is transmitted). The firmware
   should still receive; if not, they are the next patch.
1. **Whether the firmware starts without PHY_ACTION** (pre-calibration with NVRAM front-end
   data and connfem's id). Android always sends it before the download; M1 does not. If
   FW_FINISH is refused or the ready bits never come, PHY_ACTION (with the default `WIFI`
   NVRAM image's ePA/eLNA bytes -- never the phone's `nvdata` -- and the FEM id, which needs
   reading the FEM GPIO pins as `connfem.ko` does) is the next patch.
2. **Pre-cal at boot**: Android's conninfra runs a BT+Wi-Fi calibration pass at boot
   (`opfunc_pre_cal`, dmesg `[pre_cal] bt=[2] wf=[2]`); without it the firmware should
   calibrate itself (`FORCE_CAL`), which is what M1 relies on.
3. **EMI MPU**: Android programs region 27 for CONNSYS's memory; M1 assumes unconfigured DRAM is
   open to CONNSYS. A ROM that never reaches 0x1d1e after a correct copy points here.
4. **RC-mode details**: whether LK leaves SRCLKEN_RC so that VCN33 (hardware-switched in RC
   mode) actually comes on when CONNSYS asks; step 3's LDO report will say.
5. Things Android does at module load that M1 skips: the EMI's log/coredump headers
   (connsyslog), LOG_TIME_SYNC, the front-end GPIO modes, DMASHDL, the FW log ring. None should
   stop the firmware from starting; some will matter for M2 (RF) and M3 (data).

## 4. The plan

Each milestone is a kernel patch behind its own switch (default off) plus, where needed, a
user-space piece; each is held against what Android's driver does on this phone.

| | Goal | Kernel | User space | Size (new lines) | Main risks |
| --- | --- | --- | --- | --- | --- |
| **M1** | CONNSYS powered, A-die found, the Wi-Fi MCU running Samsung's firmware (its ready bits), all in the log | 0940, 0960 (`a34_wifi.c` steps 1-5) | `a34wifi` (sysctl), `tools/wifi-fw.sh` | ~1 900 (built) | see section 3 |
| **M2** | scan results in the kernel log -- **passed on the phone (0961)** | steps 6-7: PHY_ACTION before the download if M1 shows it is needed; the running firmware's command channel -- the SW-WFDMA mailbox in the EMI (offset from `0x180537fc`, 4 x 1600 bytes, `drv_idx`/`fw_idx`, CCIF `0x1803d00c` = 3) with answers on RX ring 0 --; `GET_NIC_CAPABILITY_V2` (0x8a: firmware version, MAC address tag 7), `BASIC_CONFIG`, the front-end GPIO modes (connfem pinctrl), `BSS_ACTIVATE_CTRL` (0x11) for the station's BSS 0, `SCAN_REQ_V2` (0x03); RX rings 2/3 (beacons/probe responses as RX data) and event `SCAN_DONE` (0x0d) on ring 0; SSID, BSSID, channel, RSSI parsed | `a34wifi scan` | ~900 | the connac2x command/event layouts (gen4m's v2 structures, HackiOS's connac1 ones as a guide); whether the firmware transmits probes without NVRAM data (the default `WIFI` image, never the phone's `nvdata`) and without country/power-limit commands |
| **M3** | join an open network, then WPA2-PSK; an IP by DHCP; apps reach the internet -- **built (0963-0965), not yet run** | steps 8-9 (0964, 0965): channel privilege, the AP's STA record, BSS info, auth/assoc frames through the mailbox, EAPOL as data on TX ring 0, keys (`ADD_REMOVE_KEY`), the data rings and DMASHDL, an Ethernet `ifnet` (`a34_wifi_net.c`, after HackiOS's `redwood_net.c`), the WPA2 handshake in the kernel (`a34_wpa.c` = HakiOS's `lx_wpa.c`); the MAC kept on the root (0963) | `a34wifi up 9` / `join` (network from `/a34/wifi/network`), `a34wifi dhcp` (IPConfiguration's `setservice`, HakiOS `net_test.m`, MIT), `a34wifi tcp` | ~1 900 + ~550 ported | whether the radio transmits without PHY_ACTION/NVRAM (the TX statuses say); the management-frame TXD on the mailbox; the WTBL numbering (5 for the AP) |
| **M4** | Settings' Wi-Fi: networks listed, password typed, joined -- **built (0969), not yet run** | the wifid layer (section 2): `IO80211Controller`/`IONetworkInterface`, Apple80211 ioctls and events, `IONetworkStack`; ADT `wlan` node with `local-mac-address` | wifid, configd, mDNSResponder, trustd, securityd, nsurlsessiond, CFNetworkAgent enabled in the root | ~1 100 ported + ~200 | boot stalls if IOKit never quiesces (ios-topaz 0128) or InterfaceNamer never reports quiet; wifid holding SpringBoard (ios-sapphire); the 16.7.16 structure layouts (sizes match, field offsets to confirm from the log) |

**The smallest path to a scan, then a join**, as the sources show it: M1's firmware start, then
everything on the mailbox -- NIC capability, basic configuration, BSS 0 activated, one scan
request -- and three RX rings polled (events on 0, frames on 2/3); no interrupts, no DMA
scheduler, no data TX rings yet. A join adds station/BSS records, channel privilege, the
authentication/association frames, the EAPOL frames and keys (all as commands, as HackiOS does
on the MT6768), then the data rings (TX 0, RX 2) behind an Ethernet `ifnet`.

Not planned: Wi-Fi Direct, hotspot, 6 GHz, power saving beyond what keeps the link up, and
Bluetooth (same CONNSYS, its own MCU and firmware; HackiOS reached HCI on the MT6768).

## 5. Prior art

| Project | Wi-Fi | Licence | For us |
| --- | --- | --- | --- |
| HackiOS `mtk` (MT6768, MT6631, CONNAC 1) | Settings lists and joins WPA2, DHCP, HTTPS, Safari (runs 441-467, `docs/wifi-x6826b.md`) | kernel files APSL 2.0 (XNU-derived, `xnu/README.md`), rest MIT | the whole iOS-facing layer (section 2), the WPA2 handshake, the firmware command set (scan, BSS/STA records, keys, power save) and its lessons (runs 448-459: management frames on the command ring, EAPOL at a fixed basic rate, BSS kept active through scans, CAM power save after joining); the method: one boot-arg per step, each held against Android's log. **Not** its chip control: WMT over STP over BTIF and CONNAC 1 DMA have no counterpart on soc5_0 |
| HackiOS `main` (WCN6750, ath11k) | the same, on Qualcomm | LICENSE + LICENSE.APSL | the controller class chain, `local-mac-address` |
| hakios-mt6768 | as `mtk` | MIT, `kernel/` APSL | as `mtk` |
| ios-topaz (SM6225, WCN3988) | joins WPA2, keychain, reconnects, Safari | no LICENSE file (CREDITS: patches APSL) | patch 0128 (IOKit quiesces without kernelmanagerd) -- to check against our baseline |
| y2sios (Exynos 990, BCM4375) | WPA2, DHCP, IPv4/IPv6, HTTPS | MIT + APSL | noise/SNR in scan results (one bar otherwise) |
| ios-sweet (SM6150) | none; USB Ethernet `ue0` pings, iOS ignores it | MIT | the negative result |
| ruby-ios-bringup (MT6877) | none (an old `IONetworkController` stub, unused) | MIT, patches APSL | -- |
| UniXNU (Ath12k) | iPadOS 17, Linux ath12k + wpa_supplicant in user space | BSD-2-Clause-Patent | different OS layouts |
| Nik300 viva | nothing public on Wi-Fi (kernel private) | -- | -- |

Nobody has run CONNAC 2 (soc3_0/soc5_0, conninfra) under XNU: power-on, firmware download and
WFDMA are new work here; everything above the radio is not.

## 6. How the readings were made

- Firmware: `simg2img` of `flash/super.img` (sparse), a small LP-metadata reader for `vendor`
  and `vendor_dlkm` (the same as in `tools/wifi-fw.sh`), `mount -t erofs -o loop,ro`; copies and
  `SHA256SUMS` in `research/wifi/`. Headers read with `xxd` and a 20-line Python tailer parser.
- Sources: `git clone --filter=blob:none --no-checkout --depth 1 -b sep-17.0/stock` of
  Fede2782/android_kernel_samsung_a34x with a sparse checkout of
  `vendor/mediatek/kernel_modules/connectivity/{conninfra,wlan,connfem,common}` and of the
  regulator, clock power-gating and scpsys files under `kernel/kernel_device_modules-6.6`.
- Which tree the phone runs: `strings` of `wlan_drv_gen4m_6877.ko` (source paths, format
  strings) and its `vermagic`.
- iOS: strings and disassembly of 16.7.16's `wifid`, `configd`, IPConfiguration, IO80211 and
  SystemConfiguration (root image and dyld cache); `com.apple.wifid.plist` from the root.
- XNU interfaces: xnu-8796.141.3 headers (the version CI builds for macOS 13.5).

## 1011: faster

0999-wifi-a joined as an HT 20 MHz, one-stream station (MCS 7: 72 Mbit/s on the air at best).
1011 joins as wide and with as many streams as the chip, the AP and two switches allow:

- **The chip's PHY**: the NIC capability's tag 8 (ucHt, ucVht, uc5gBand, ucMaxBandwidth, ucNss,
  ...; nic_cmd_event.c) is read at step 6 and logged ("step 6: PHY: ..."). Android shows the A34 at
  two streams (MCS 15).
- **Two streams** when the chip has them and the AP receives MCS 8-15: the request advertises MCS
  0-15 and TX STBC, BSS info's NSS 2; with one stream the AP's record is masked to MCS 0-7.
- **Width, 5 GHz only**: 40 MHz when the AP's HT Operation has a secondary channel and the width
  bit; 80 MHz when it also has VHT Operation at 80 and the chip has VHT. CH_PRIVILEGE gets the SCO,
  channel width and centre segment, BSS info's RLM the same (and u2VhtBasicMcsSet = the basic rate
  set, as rlm.c fills it), the AP's record its VHT capabilities as they are and ucVhtOpMode
  (width | (AP's streams - 1) << 4); the request carries VHT Capabilities (MCS 0-9 per stream,
  RX LDPC, short GI 80, max A-MPDU 2^20-1, no beamformee). 2.4 GHz stays at 20 MHz (gen4m's
  default; 40 MHz there collides with the neighbours).
- **Switches**, read at the next join: `kern.a34wifi.width` / boot-arg `a34wifiwidth` (20, 40, 80;
  default 80) and `kern.a34wifi.nss` / `a34wifinss` (1, 2; default 2). If a wide join misbehaves on
  a given AP, `a34wifiwidth=20 a34wifinss=1` is 0968's join.
- **RX reordering**: the firmware takes the AP's ADDBA and reports it (EVENT_ID_RX_ADDBA: TID,
  window, SSN). From then unicast QoS data of that TID goes up in sequence order (group 4's sequence
  control and QoS control), as que_mgt.c does: held inside the window, the window moved by a frame
  beyond it, late/duplicate frames dropped, a hole given up after 100 ms; DELBA ends it. A-MSDU
  subframes after the first go straight up. Counters in the carried line: "reordered, late or twice,
  holes given up".
- **RX ring 2** (all our data) has 256 slots instead of 64 (Android's size): at 300 Mbit/s about
  50 frames arrive between two 2 ms polls.
- **Measuring**: `a34wifi get [URL] [SECONDS]` (default http://speedtest.tele2.net/10MB.zip, at most
  30 s) prints the bytes each second and the Mbit/s at the end.

## 1012: a deaf start, restarted

Boot 0977-120-a (and earlier 0980-btmetal-a): the firmware came up, every command was answered,
every scan finished -- and the radio heard nothing at all, on 2.4 and 5 GHz, for the whole boot
(13 scans, "0 beacons counted by the firmware"). In both, FW_FINISH was answered early: 116 and
426 ms, against 710-790 ms in the ~30 boots that heard networks. The firmware does its RF
power-on calibration there; cut short, the receiver is not set up.

Not Bluetooth/Wi-Fi coexistence (PTA) starving RX: PTA arbitrates 2.4 GHz air time only, and 5 GHz
was as deaf; the firmware's own beacon counter was 0 on 22 channels. LE scanning with the same
parameters ran during the Wi-Fi start in good boots too (1011-120-a, 1001-120-a, 0994-perf-a), so it
is intermittent (2 of ~25 boots with Bluetooth up). On this SoC (connv2) Android sends no WMT/COEX
command from the host: conninfra enables the coex/PTA clocks at power-on (a34_connsys.c ph13 does
the same), the firmwares arbitrate between themselves; Wi-Fi's own extras at on are the
co-antenna VFE28 LDO (only with NVRAM ucSupportCoAnt, modem co-antenna) and the "Wi-Fi on" flag
for the modem's PCCIF (we write it). What Android does differently: Wi-Fi never calibrates live
next to a busy Bluetooth -- conninfra's pre-calibration at boot runs BT's then Wi-Fi's calibration
with nothing else on (PHY_ACTION force calibration), keeps the result in EMI, and every later Wi-Fi
on sends PHY_ACTION "use the backup".

1012, from the Wi-Fi side:
- FW_FINISH under 600 ms is said in the log.
- A scan is "deaf" when the firmware counted no beacon on 20+ channels and forwarded no frame.
  Step 7 and `a34wifi scan` scan up to three times, 5 s apart, while deaf.
- Still deaf: the Wi-Fi firmware is started again -- WFSYS off as soc5_0.c wf_pwr_off_consys_mcu
  does it (sleep protection, WFSYS_ON_TOP off, its semaphore reset, the A-die's top clock 1 off,
  the EMI/infra requests toggled; conn_infra and Bluetooth untouched), the ROM mailbox and ready
  bits cleared, then steps 4-6 and the scans again. Once a boot by default
  (`kern.a34wifi.restarts_most` / boot-arg `a34wifirestarts`; 0 keeps a deaf radio for study);
  `kern.a34wifi.restarts` counts them. Three deaf scans by iOS while not joined do the same.
- Before a restart the log has the radio status, the A-die's top clocks, CONN_ON_MISC, LPCTL and
  WF sleep status.

If restarts keep being needed, the next steps are Android's: calibrate with PHY_ACTION (0966,
`a34wifiphy=1`), or have Bluetooth hold its LE scan for the ~0.8 s of Wi-Fi's FW_FINISH.

## 1013: the A-die from cold at every boot

Then 1011-120-b: the very kernel and root that had joined (1011-120-a) booted again, warm, after
0977-120-a -- and heard nothing, with FW_FINISH at a normal 783 ms. So the deafness survives warm
reboots. Everything in the CONN power domain starts from reset at each boot (ph3 finds
CONN_PWR_CON 0x12, off): conn_infra, WFSYS, the Wi-Fi firmware. What does not:
- the A-die (MT6635), a chip of its own on the PMIC's VCN18, VCN13, VCN33_1/2: ph1 and ph15 switch
  them on by software, nothing switches them off before a reboot, LK leaves the PMIC alone -- the
  A-die runs on through a warm reboot with what the last boot's firmwares set in it;
- CONNSYS's memory (DRAM at 0x60000000, 7.5 MB): only the images, Wi-Fi's log/dump areas and the
  mailbox are rewritten each boot.

1013 (bsd/dev/a34_connsys.c, the first power-on of a boot, before any radio):
- ph0 cold: CONNSYS's memory zeroed.
- ph1 cold: the rails' state logged ("ph1: before: ..."); any seen on are switched off as Android's
  power-off does (VCN33_2, VCN33_1: software and SRCLKEN_RC enables; 1 ms; VCN13, VCN18), left off
  100 ms, then ph1/ph15 switch them on as always. A cold boot logs "a cold start already".
- ph8: the A-die's reset held 100 us before it is released (it was only released).
- `a34conncold=0` skips the memory and the rails (not the reset).

Shared with Bluetooth (the first power-on is usually Bluetooth's): it costs ~100 ms there.

