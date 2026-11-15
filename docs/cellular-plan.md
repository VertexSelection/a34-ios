# Cellular on the A34 port: findings and staged plan

Status: 2026-11-15, branch `cellular`. Built so far: the read-only stage 0 probe (kernel patch
`xnu/patches/0950-a34-cell-probe.patch`, boot-arg `a34cell=1`) and the user-space daemon core
`userland/a34cell` (AT channel, command policy, SMS PDUs, modem model; CI
`.github/workflows/a34cell.yml`). Nothing here has run on the phone yet.

## Safety rules (they come before everything below)

The phone's IMEI is registered in Türkiye. BTK's device registration regulation puts devices
with altered identity on the blacklist, and Law 5809 art. 55 makes changing, copying or
re-creating a device's electronic identity a crime
([regulation text](https://www.alomaliye.com/2014/07/12/elektronik-kimlik-bilgisini-haiz-cihazlarin-kayit-altina-alinmasina-dair-yonetmelik/),
[law commentary](https://dergipark.org.tr/en/pub/inuhfd/article/1808667)). So:

- `efs`, `sec_efs`, `nvram`, `nvdata`, `nvcfg`, `protect1`, `protect2`, `proinfo`, `param` and
  any calibration/identity storage are never written, erased, patched or backed up. The kernel's
  UFS driver can only write inside the root partition (`ufs_write_blocks()` checks the bounds
  before every write); 0950 adds a read path for other partitions that has no write
  counterpart and only serves partitions on a fixed list (stage 0: `md1img` only).
- The modem reads its NVRAM itself, through a file server we provide. That server is read-only:
  the modem's writes go to a RAM overlay for the session and are never flushed; the
  "BIN_REGION" backup/restore operations (which write the `nvram` partition on Android) and OTP
  writes are refused. Nothing it serves is logged beyond file names and sizes.
- The transmitter stays off (AT+CFUN=4) unless the user asks for it (`a34cell -radio`) AND the
  modem reports one well-formed identity of its own (15 digits, valid check digit, not a blank
  pattern). The value is wiped right after the check, never printed, stored or passed on. If the
  modem ever comes up without its NVRAM, it stays silent.
- Every AT command passes an allowlist (`userland/a34cell/policy.c`); the vendor's identity
  programming commands are refused unconditionally, as is MediaTek's own radio command.
- No META/factory boot modes (their tools are what writes calibration and identity), no One UI 8.

## What the hardware and Android do (evidence)

**The modem.** MT6877's integrated modem, "gen97" (`mediatek,md-generation = <6297>`), control
over CCIF with shared-memory ring buffers, data over DPMAIF v2 (`mediatek,mdhif-type = <6>`:
CCIF|DPMAIF; no CLDMA). Samsung's A34 kernel: `drivers/misc/mediatek/eccci`, `ccci_util`,
`ccmni`, DTS `arch/arm64/boot/dts/mediatek/mt6877.dts` (lines 3720-3800) in
[Fede2782/android_kernel_samsung_a34x](https://github.com/Fede2782/android_kernel_samsung_a34x)
(branch `sep-16.0/stock`, A346BXXSADYG1 OSRC). AP_CCIF 0x10209000, MD_CCIF 0x1020A000, IRQs SPI
238/239, MD watchdog SPI 110; DPMAIF 0x10014000 (AO_UL), 0x1022D000 (PD_UL), 0x1022C000
(PD_MD_MISC), 0x1022E000 (SRAM), SPI 253.

**The image.** `md1img` (70 MB in A346EXXSADYG1's CP; partition 200 MiB) is a list of MediaTek
sections (512-byte headers, magic 0x58881688): `md1rom` 28.4 MiB, `md1drdi` 14.4 MiB, `md1dsp`
5.7 MiB, each followed by `cert1md` + `cert2` (signed), filters, `md1_mddb*`, `md1_mdmlayout`,
`md1_file_map`. md1rom's CHECK_HEADER v6: platform `MT6877_S00`, build
`TITAN_A34_T0MP1_K6877V1*NLWG_6190M_IPC_CUSTOM` -- a Samsung "IPC" build of MediaTek's modem.

**Who loads it: LK, and only on a normal boot.** The kernel never loads the image: it reads
LK's tag table and the image's check header from modem memory ("modem image ready, bypass
load", `modem_sys1.c:199-209`; the probe gives up without LK's `ccci,modem_info_v2`,
`ccci_util_lib_fo.c:802-835`). LK (`lk-verified.img` of A346EXXSADYG1) verifies the sections
(`ccci_ld_md_sec.c`, "SECURE CHECK FAIL : md1img.img"), sets EMI MPU regions and passes the
layout on, and its strings say **"[ccci] recovery mode, no need load modem"** (and "[adsp]
recovery mode, no need to load adsp": the audio DSP is skipped too). Measured:

| | Android normal boot (`recon/android-DYG1/live.dts`) | TWRP, recovery boot (`recon/twrp-DYG1/live.dtb`) |
| --- | --- | --- |
| `/chosen ccci,modem_info_v2` | tag table 0xBFF60000 (+0xA98), v3, 20 tags, load flag 1, no error | all zero |
| modem regions | `md_mem_usage` 0xE0000000.., 0xE4000000 (126 MiB), 0xFAFA0000; `ap_md_c_smem` 0x8A000000 (41 MiB), `ap_md_nc_smem` 0x8E000000, `ccci_tag_mem` 0xBFF60000 | none |

Our XNU boots through `recovery` (README "How it boots"), so **today no modem image is loaded**.
XNU manages only 0x1_4000_0000 and up (`tools/mkadt_a34.py`), so on a normal boot all modem and
shared-memory regions (below 4 GiB) stay untouched by XNU.

**Starting it (Android's order, `ccci_fsm.c:1440-1607`, `md_sys1_platform.c`).** CCIF clocks
(SMC CCIF_CLK_REQUEST), CCIF reset and SRAM clear, ring buffers in shared memory, then power:
PMIC voltages (md-vmodem MT6315 #3 vbuck1 825 mV, md-vsram vbuck4 825 mV, md-vdigrf MT6359P
VPU 700 mV, DTS lines 8674-8712 / 9397-9405), topckgen md1 clock bits, srclken-o1 (SPM
0x10006000/0x10006008), SMC MD_FLIGHT_MODE_SET(0), MD1 MTCMOS on (SPM MD1_PWR_CON 0x10006E00;
status 0x10006EF0/0x10006EF4 bit 0; bus protection release), then SMC MD_POWER_CONFIG /
MD_KERNEL_BOOT_UP ("make boot vector take effect"). All SMCs are 0xC2000505 with a sub-ID. The
modem then sends HS1 on the control channel (MD_INIT_START_BOOT, reserved 0x5555FFFF, 16+224
bytes), the AP answers with its runtime feature list (64 entries in SMEM `RAW_RUNTIME_DATA`,
pattern 0x43434349) and waits for HS2 (30 s timeouts). On its first start LK has already
powered the domain and run the modem's boot ROM check; the kernel waits for "BROM PASS"
(MD_CHECK_DONE) and powers it off before the real start. CCCI packets: 16-byte header
`{u32 data[2]; u16 channel; u16 seq:15|assert:1; u32 reserved}` in rings framed
0xAABBAABB/len ... 0xCCDDEEFF.

**Ports** (`port_cfg.c`, all over CCIF): control 1/0, sys 3/2, `ccci_fs` 15/14 (the modem's file
server), **`ttyC0` AT 12/10**, `ccci_aud` 5/4 (speech control), IMS `imsv/imsc/imsa/imsdc/imsem/
imsm`, and Samsung's `ccci_umts_ipc0/1` 194/193, 196/195.

**Android's user space** (A346EXXSADYG1 vendor image): `ccci_mdinit` starts the modem and *is*
the modem's file server (the old `ccci_fsd` lives inside it): it serves
`/mnt/vendor/nvdata/md`, `protect_f/md`, `protect_s/md`, `nvcfg`, `md_sec`, and implements
`FS_CCCI_OP_BIN_REGION_ACCESS` (backup/restore to the `nvram` partition) and an OTP write -- the
two we refuse. Samsung's RIL (`libsec-ril.so`) appears to speak Samsung IPC over `umts_ipc0`
(its device strings; no MediaTek AT commands in it); MediaTek AT is used by `ccci_mdinit`, `md_monitor`, `factory` (via
`libccci_util.so`). IMS-related: `volte_clientapi_ua`, `volte_rcs_ua`, `rcs_volte_stack`,
`ipsec_mon`; no AP-side `volte_ua`/`volte_stack`/`volte_imcb` as on older MediaTek phones, and
MediaTek's bulletins name "Modem IMS Stack" and "Modem IMS Call UA" components on Dimensity
chips (e.g. [CVE-2023-32887](https://www.cybersecurity-help.cz/vdb/vulns/84924)): the VoLTE
user agent runs in the modem.

**iOS 16.7 (iPhone X build).** CommCenter (`CoreTelephony.framework/Support/CommCenter`, with
`libCommCenterBase`, `libCommCenterCommandDrivers`, `...MCommandDrivers`, `...KCommandDrivers`,
`AppleBasebandManager`, `AppleBasebandServices`, `libIPTelephony`) carries both Apple baseband
protocols: QMI for the Qualcomm iPhone X (A1865/A1902) and ARI for the Intel one (A1901)
(`AriHost`, `QMux` in the shared cache). Both are spoken over Apple's own IPC drivers
(`AppleConvergedIPC`, `AppleBasebandPCI`), with Apple's firmware update/personalisation
(`BBUpdater`) in the path. Message layouts are public for parsing
([BaseTrace](https://github.com/seemoo-lab/BaseTrace) QMI incl. Apple's extensions,
[ARIstoteles](https://github.com/seemoo-lab/aristoteles)), but not the bring-up, the
driver interface or the SIM/activation flows. Our root has CommCenter removed (ios-sweet's
`sweet-root-tweaks.py`: it crash-loops without a baseband and Setup waits on it).

**Prior art.** No XNU-on-non-Apple project has given CommCenter a baseband. HackiOS
([cellular.md](https://raw.githubusercontent.com/oosexgang/hackios-xnu-kernel/main/docs/cellular.md))
boots a Qualcomm modem from its own kernel code, serves the modem's file systems read-only with
writes kept in RAM (the model for our file server), shows bars through a SpringBoard hook reading
a sysctl, and carries data as an Ethernet-like `en1` with the kernel answering ARP/DHCP; calls and
SMS are "not yet". Its MediaTek port and `hakios-mtk` stub CommCenter's services with an empty XPC
server (`nullxpc`). ios-topaz boots its modem firmware; SIM/LTE not started. ruby-ios-bringup (same
SoC): cellular "not yet". Linux mainline has no driver for MediaTek's integrated phone modems;
`drivers/net/wwan/t7xx` (the PCIe T700) shares the CCCI header, port and state-machine design (with
CLDMA instead of CCIF) and is the best open reference for the shape of the code -- we write our own
code from facts, the GPL sources are not copied into XNU.

## Verdict per capability

| Capability | Verdict | Effort (on-device iterations included) | Gate |
| --- | --- | --- | --- |
| Modem running, AT answering | feasible | 4-8 weeks | normal-boot path (LK loads/verifies md1img); CCIF + handshake; read-only file server |
| SIM, registration, bars | feasible after the above | 1-2 weeks | SpringBoard hook (no CommCenter) |
| SMS send/receive | feasible (AT, PDU mode done) | 2-4 weeks | UI: notifications/own app first; Messages integration is RE work |
| Mobile data | feasible | 4-8 weeks | DPMAIF v2 driver + an interface (HackiOS's `en1` pattern) |
| Voice calls | modem side easy; **audio is the long pole** | months | AFE + MT6359 codec + speech control (`ccci_aud`); ADSP is also not loaded on recovery boots |
| VoLTE (IMS) | the modem runs the UA itself, which helps a lot | months, after calls | IMS PDN/config commands on a Samsung "IPC" modem build; VoWiFi needs ePDG/IKE on our side |
| Native Phone/Messages apps | hard | months | needs a CommCenter stand-in on CoreTelephony's private XPC, or a baseband emulation (QMI/ARI) -- both unexplored |

## Staged plan

### Stage 0 -- read-only probe (built, ready for the first test)

Kernel `0950` (`a34cell=1`, off by default) logs, read only: LK's `ccci,modem_info_v2` and modem
regions from LK's device tree (still at 0x47c80000), MD1 power status, CCIF clock gates, the
modem's boot status via SMC (only if MD1 is on), `md1img`'s sections and build, and the GPT
bounds of the modem partitions (bounds only). `kern.a34cell` holds a one-line summary;
`a34cell` prints it and exits when there is no modem port.

First phone test (ready, not flashed):
- Image: `build/xnu-recovery-0950-cell.img` (kernel of xnu-a34 run 43677350436 on `cellular` =
  main 0938 + 0950, copied as `build/kernel-0950-cell.development.sweet`; boot args = the README's
  set plus `a34cell=1`, 433 characters). Flash and boot it like the other test images
  (`tools/run-test.sh build/xnu-recovery-0950-cell.img`).
- Optional user space: `build/a34cell` (artifact of `.github/workflows/a34cell.yml`), into the
  root image with `sudo tools/root-add-job.py /mnt com.a34.cell /a34 build/a34cell/a34 a34cell`.
- Success, in `tools/pull-log.sh`'s log, about 3 s after "SWEET UFS": `A34 CELL: stage 0 probe`;
  `LK device tree at 0x47c80000` (the `dtb_addr_mrdump` block LK reserves for its DTB) and a
  `ccci,modem_info_v2 all zero: the boot loader did not load the modem` line (expected on this
  recovery boot); `MD1 power domain off` (or on, with a boot status line); the CCIF clock gates;
  partition bounds of md1img/md_udc/nvcfg/nvdata/nvram/protect1/protect2/proinfo (bounds only);
  24 `md1img` section lines with `md1rom CHECK_HEADER v6: platform MT6877_S00`; the summary.
  With the daemon: `a34cell: kernel: stage0 lk=... md1=... ccifclk=... md1img=24sec,MT6877_S00`,
  then `no modem port /dev/a34at0 ... nothing to do`. If anything hangs, the last `A34 CELL`
  line names the step (it only reads SPM/infracfg status registers, LK's DTB and md1img).

### Stage 1a -- XNU from `boot`, so that LK loads the modem (approved 2026-11-15)

- Format: Android boot image v4 like the stock `boot` (page 4096, header 1584 bytes, stock
  os_version 0x1e000197), kernel = gzip of our loader + XNU payload (no TWRP kernel: on a normal
  boot LK hands over vendor_boot's/init_boot's ramdisk, so the loader's Volume Up escape finds
  nothing and goes on to XNU), no ramdisk, "SEANDROIDENFORCE", AVB hash footer for the 32 MiB
  partition (LK's GPT log: `boot` 0x2000 sectors of 4 KiB) with AOSP's test key. vbmeta keeps
  flags=3 (Magisk's), so LK does not verify it; vendor_boot, init_boot and vbmeta are not
  touched. `tools/build-xnu-boot.sh KERNEL "ARGS" OUT.img` builds it (temporary directory, checks
  32 MiB, header and footer).
- TWRP: `recovery` keeps whatever it holds (an xnu-recovery image: hold Volume Up through the
  white bar; or plain TWRP). Get there with the phone off: Volume Up + Power with the USB cable in
  (Samsung's recovery combo); download mode stays Volume Up + Volume Down.
- Images (kernel `build/kernel-0961-cell.development.sweet` = main 0961 + 0950, xnu-a34 run
  43713846188; boot-args = the 0961-base set + `a34cell=N`, 526 characters):
  `build/xnu-boot-0961-cell1.img` (`a34cell=1`, read only) and
  `build/xnu-boot-0961-cell2.img` (`a34cell=2`: also the first contact, below).
- Write steps (from TWRP; `A34_ROOT=/home/vertex/a34-ios`, scripts from this branch):
  1. `tools/backup-boot.sh` -- reads `boot` only into `build/boot-backup-<sha1>.img` (32 MiB,
     header v4, SHA-1 checked against the phone; says whether it is stock DYG1).
  2. `tools/run-boot-test.sh build/xnu-boot-0961-cell1.img` -- refuses without a backup or for
     anything but a 32 MiB v4 image; writes `boot` only, checks SHA-1, clears a boot-recovery
     note in misc, plain reboot.
  3. Pull the log from TWRP as usual (`tools/pull-log.sh`).
  4. Back to Android: `tools/run-boot-test.sh build/boot-backup-<sha1>.img`.
- Success: `A34 CELL: LK /chosen ccci,modem_info_v2: ... -> modem LOADED by LK`, the modem regions
  (`md_mem_usage`, `ap_md_c_smem`, `ap_md_nc_smem`, `ccci_tag_mem`), ~20 `LK tag` lines (the
  shared-memory layout), `MD1 power domain ON`, `MD boot ROM check ... = BROM PASS`, md1img.
  Then cell2: `SMC MD_KERNEL_BOOT_UP`, `CCIF from the modem: RCHNUM ...`, and
  `HS1 -- the modem is running and asks for the AP's features` with its 64 feature bytes.

First run (2026-11-15, `0961-cell1`, log build/logs/0961-cell1-a): LK loaded the modem (tag
table v3, 20 tags, load flag 1), MD1 on, CCIF clocks on, BROM PASS, boot status 0x5443000c (the
DTS's expected value), md1rom `MOLY.NR15.R3.TC10.A34.PR.SP.V2.P58`. The LK tags are the legacy
set (smem_layout at 0x8e000000/0x1e0000, nc_smem_info_ext x3, md1_bank4_cache_info at
0x8a000000/0x2960000 with a 0x08000000 cache offset, hdr_tbl_inf: bank0 0xe0000000), so 0951
takes the gen6297 fallback tables. SpringBoard then crashed on a UIKit asset lookup
(BoundingPathBitmap-1125x2436...) in a GL boot that followed Metal boots of the same root.
Checked since: none of LK's reserved regions on either path (normal: + ADSP, SPM, SCP, VPU,
modem, shared memory) lies in XNU's 0x1_4000_0000-0x2_3FFF_F000; the loader now refuses/trims
if one ever does, and 0950 logs them all with an overlap count. To tell boot path, kernel and
root state apart: `xnu-recovery-0980-cell1.img` is the same kernel and boot-args through
`recovery`. Second run (`0980-cell1`, Metal boot-args) failed the same way right after a good
`recovery` boot of the same root. Cause found: those
boot images were built in a directory without `research/ipsw-x/d22ap.adt`, and
`mkadt_a34.py` then makes an ADT without `/product` (it only prints a note); SpringBoard's
`IODeviceTree:/product` lookups answered "none" (good boots: IOPlatformDevice) and UIKit's
asset lookup for the iPhone X's screen failed. Neither the `boot` path nor 0950 was at fault.
`tools/build-xnu-boot.sh` now refuses to build without the iPhone X tree and checks the ADT
for `/product`.

Third run (`0991-cell2`, log build/logs/0991-cell2-a): with Android's start sequence (MD1
bus protection set and acked, power off, MD_FLIGHT_MODE_SET 1/0 answered 0, power on, bus
protection released, MD_KERNEL_BOOT_UP, whose a0 is 1 on the working path too) the modem sent
**HS1**: header ffffffff 00000000 00000000 5555ffff, feature query patterns 0x49434343. It asks
feature 0 (BOOT_INFO) as MUST and every other one as optional, version 0, so the AP's policy
offers all it can and nothing cuts the runtime data; CCCI_FAST_HEADER stays off.
CCIF_CLK_REQUEST answers -16 and MD_CHECK_FLAG 1 on this firmware (Android ignores both).

Fourth run (`0991-cell3`, log build/logs/0991-cell3-a): HS1 answered (868 bytes of runtime
data, SRAM reply v2.1), rings built, the file server served 246 requests (nvdata, protect1,
protect2 mounted read only; 87 opens, 2 not found; 18 files moved to RAM), then the modem
raised its exception bits (CCIF 16, 17, 18, 19) after RPCs we answered with -1: GET_GPIO_NUM
x8, GET_EINT_ATTR x18, DTSI_QUERY, GET_GPIO_ADC. Those four are now answered as Android's
port_rpc.c does (device tree and read-only hardware), and the modem's exception overview is
logged when it asserts (`xnu-boot-0995-cell3`).

Fifth run (`0995-cell3`, log build/logs/0995-cell3-a): every RPC answered (GPIO_SIM1_HOT_PLUG
176, SIM1/2 EINT attributes, md_product_name_model_id 0xf0f0f0f, ADC channel 6 raw 1938), the file
server served 1403 requests (386 opens, 232 reads, 45 files in RAM, 0 errors), then the modem
asserted in `mcu/service/hif/ncccisrv/ccci_shm_bm/src/ccci_shm_bm.c` line 603 (offender MCU,
type 0x50): its CCB buffer manager. We had sent post_cfg_for_ccb's layout (three CCB users of
2 MiB at one base, 6 MiB in CCB_SHARE_MEMORY) without Android's later collect_ccb_info (DHL
0x70000, MD_MONITOR 0x8000, META 0x104000, packed; total 0x17c000), and we never zeroed the regions Android clears at
first boot (BOOT_WAITING_FOR_HS1: all of md1_noncacheable_tbl/md1_cacheable_tbl without
SMF_NCLR_FIRST, incl. CCB data and CCB_CTRL). Both fixed in `xnu-boot-0996-cell3`, which
panicked at boot: the clear mapped the 7 MiB NVRAM_CACHE with ml_io_map, whose 8 MiB io_submap
allocates with KMA_NOFAIL. Shared memory now comes from kernel_map (ml_io_map_unmappable): big
regions only through 1 MiB windows unmapped after use, kept mappings capped at 1 MiB, LK's tag
table and device tree unmapped once copied (`xnu-boot-0999-cell3`).

Sixth run (`0999-cell3`, log build/logs/0999-cell3-a): no panic; 20 regions (40417 KiB)
cleared, the CCB sizes went out (DHL 0x70000, MD_MONITOR 0x8000, META 0x104000), FS as before,
then the same assert at ccci_shm_bm.c line 602. The kernel never writes CCB_CTRL; Android's
ccci_mdinit does right after starting the modem: libccci_util `ccci_ccb_init_users` (DHL,
MD_MONITOR, META) writes, per ccb_configs[] entry, a 64-byte header pair at
(ccb_ctrl_offset + k) * 64 through /dev/ccci_ccb_ctrl: DL {0, 0, 0, 0, 0, page size, buffer
size, 0xEEFF0011}, UL the same (from the vendor binaries' arm64 code). Our kernel writes them
after the clear; CCB_CTRL headers are logged before/after and at an exception, the CCB data
areas only as counts of non-zero words (they hold modem logs).

Seventh run (`1000-cell3`, log build/logs/1000-cell3-a): the CCB headers worked (the modem
stamped 0xDDBBCCAA into them); the next assert is the L1 RF front end,
`mml1_cdf_device_driver_fe_elna.c` line 899. Compared with Android: DTSI_QUERY answers
0x0f0f0f0f there too (no md_attr_node in vendor_boot, dtbo or the live tree); the GPIO answers
match gpio_usage_mapping. Two differences fixed: the AUXADC value Android hands the modem is
calibrated with the devinfo efuse word at 0x1c4 (1938 -> 1951 on this phone), and
md1_pmic_setting_on sets md-vdigrf (MT6359P VPU buck) to 700 mV before power-on.
md-vmodem/md-vsram (MT6315 on SPMI, 825 mV) are not driven. VPU, the RF LDOs and
GPIO_RF_PWREN_RST_PIN (GPIO 75) are logged before power-on and at an exception.

Eighth run (`1001-cell3`, log build/logs/1001-cell3-a): same eLNA assert. The modem runs its own
rails (VPU 550 mV before power-on, 650 mV set by the modem at the exception; VFE28 off before,
on at the exception); GPIO 75 is a GPIO output, high. ADC 1940 -> 1953 as Android. No AP-side
pinctrl, clock-buffer init command or Samsung RF node exists in the live tree for the modem.
What Samsung's CCCI build adds over MTK's (TC10 features) is now done too: the BIG_DATA regions
(features 46/47), AP_DEBUG_LEVEL 0 (LOW, as ccci_mdinit passes ro.boot.debug_level; we sent -1)
and the UPDATE_MD_SEC_SMEM SMC after let-go. Not done: the SBP code ccci_mdinit may take from
nvdata APCFG/APRDCL/MD_SBP (unknown format; Samsung sets no project SBP), the AMMS interrupt
(DRDI/POS free requests), MT6315 rails.

Ninth run (`1001-cell3b`, log build/logs/1001-cell3b-a): UPDATE_MD_SEC_SMEM answered 0x10000;
no assert and no exception bits, but no HS2. The modem's file-server and RPC traffic is the
same, request for request, as in the asserting run, up to the point where that one asserted;
then it waits. Next: AMMS requests polled and acknowledged as Android's amms driver does
(SIP 0xC2000250-0x258), every CCIF doorbell logged, and a stall report at 30/120 s (CCIF of both
sides, boot status, the exception record as it stands). An a34celldbg=2 twin shows whether the
debug level only turned the eLNA assert into a silent stop.

Tenth run (`1011-cell3`, log build/logs/1011-cell3-a): AMMS works (pending 0x1 right after the
ADC query; DRDI 0xe1c60000 + 0xe60000 acknowledged); still no HS2, and the stall report shows
the unsignalled exception record: the same eLNA assert. Disassembly of md1rom (MediaTek's
nanoMIPS GNU tools, read only): line 899 checks that an eLNA table command's entry count (u16
at +12) is at most 200 (3-byte entries into a 600-byte table): the modem's RF custom data
gives it garbage. The ADC level table (0x916d48b8, 11 bins up to 1.5 V) puts our 1938-1953 in
the same bin as Android's, so the DRDI set is the same. Next: the DRDI memory the modem used is
compared, once given back, with md1drdi from the md1img partition; RF pin modes logged.

Eleventh run (`1013-cell3-trace`, log build/logs/1013-cell3-trace-a): the DRDI region is mostly
zeros once given back (the modem or ATF clears it), so that check says nothing about what the
modem copied. RF pin modes GPIO 66-97 are as the preloader leaves them (MIPI_M, MIPI0, MIPI1 and
some BPI lines in mode 1; the live Android tree uses some of the others, 76, 90, 96, 97, as AP
GPIOs; no AP-side pinctrl for the rest), the same as under Android. With the user's approval the
modem's own boot-trace text (masked) is logged: it is its NVRAM layer's trace; the 48 KiB cap
was used by file-operation lines, the first other lines being NVRAM init and a "default value"
failure. Next: file-operation lines skipped, 128 KiB.

Twelfth run (`1013-cell3-trace2`): the modem's NVRAM layer completes its core and post init; its
only errors are its own record checks, not file-server failures (0 FS errors): a default value
missing for LID 0xEF07 (file LD4D_007) and 0x985 (NR06_008), a multi-record read of an unknown
LID 0xCC0, and a caller asking record 2 of the single-record LIDs 0x985 (NR06_008) and 0xD
(UM15_000), whose writes it then rejects (ret 13). The LID -> file mapping comes from the
modem's LID table in md1rom (40-byte entries: records << 16 | LID, record size, default,
attributes, name, version). The FS semantics checked: writes grow the RAM copy, reads are byte
ranges, the two missing files are missing on Android too. Next: what the first-boot clear wipes
(non-zero words per region, counts only) and a34cellclear to leave groups uncleared.

### Stage 1b -- the control path in the kernel (`a34cell=2`)

Only on a boot where LK loaded the modem. Read LK's tag table (smem layout tags
`nc_smem_layout*`/`c_smem_layout*`, check header), map the shared memory (below 4 GiB, outside
XNU's RAM), power on in Android's order with the DTS's exact voltages and nothing else, CCIF
rings, HS1 -> runtime feature list -> HS2, then character devices: `/dev/a34at0` (ttyC0),
`/dev/a34fs` (the file server channel), later `ccci_aud` and the IMS channels. Log lines:
`A34 CELL: HS1`, `runtime data sent`, `HS2: modem ready`, per-port open.

### Stage 1c -- the modem's file server, read only, inside the kernel

Rules (coordinator, 2026-11-15): the server only reads the modem's own files in place on the
phone and hands them to the modem, as ccci_fsd does; their contents never go to the host, the
repository or any log (file names and sizes only, never bytes); the partitions are never
written (the modem's writes stay in RAM for the session); nothing changes identity.

So it runs in the kernel, not in user space: no device node or sysctl can read those
partitions, and the bytes only ever travel from the disk to the modem's FS channel (CCCI 14/15).
- `sweet_ufs_read_part()`'s read-only list gains `nvdata`, `protect1`, `protect2`, `nvcfg` --
  still read only, still no write path in the driver for them.
- `ext4ro` (userland/a34cell/ext4ro.c, the same code in the kernel): read-only ext4 without
  journal replay, tested on mke2fs images (extents with index nodes, block maps, htree
  directories, 1 KiB and 4 KiB blocks).
- The protocol from `ccci_mdinit`'s file server (being read out of the binary: op IDs, packet
  layout, path mapping). Writes, deletes, renames: kept in a RAM overlay, never flushed.
  BIN_REGION (the `nvram` partition backup/restore) and OTP: refused.
- Log: `A34 CELL FS: open <path> -> <size>`, `read <n> bytes`, counts; never data.

### Stage 2 -- SIM and network

`a34cell -radio` (radio gate), registration URCs (`+CREG/+CEREG/+C5GREG`, parsed), operator,
signal. iOS: a SpringBoard hook (our `a34inject`) reading `/var/run/a34cell.plist` on the
`com.a34.cell.state` notification and setting the status bar's signal/operator items. If the
Samsung "IPC" modem build turns out not to accept MediaTek's AT on ttyC0 for these, the fallback is
Samsung IPC on `ccci_umts_ipc0` (older SIPC versions are documented by libsamsung-ipc).

### Stage 3 -- SMS

Receive (+CMT, PDU decode with Turkish shift tables) into the user's inbox on the phone and a
notification; send (AT+CMGS) through a local socket of `a34cell`. Messages.app integration later.

### Stage 4 -- mobile data

DPMAIF v2 driver (rings, BAT, the reserved DPMAIF memory), one network interface per PDP context;
PDP via `AT+CGDCONT`/`AT+CGACT`, address/DNS via `AT+CGPADDR`/`AT+CGCONTRDP`, configured by
`a34cell`. HackiOS's `en1` + DHCP answer is the fallback if configd needs an Ethernet-like port.

### Stage 5 -- calls

AT side in `a34cell` (ATD/ATA/AT+CHUP/+CLCC/+CLIP/RING, allowlisted). UI: a CallKit provider for
incoming calls in the native call screen; outgoing from our own dialer first. Audio: MediaTek's
voice path is modem DSP <-> AFE PCM (`MD1_TO_AFE`/`AFE_TO_MD1`, see mainline
`sound/soc/mediatek/mt6797/mt6797-dai-pcm.c`) <-> MT6359 codec, switched on by speech messages on
`ccci_aud`; none of that exists in our kernel yet (no audio hardware driver at all).

### Stage 6 -- IMS / VoLTE / VoWiFi

The modem runs the IMS stack and the call UA, so the AP's part is smaller than on older MediaTek
phones: enable IMS and its features, keep the IMS PDN (APN "ims") up, report registration
(`AT+CIREG=2`, `+CIREGU`, parsed already), and route call audio as in stage 5. Open questions: the
IMS enable/config commands on this Samsung build (FuriLabs' MediaTek plugin sends
`AT+EIMS` from `setImsCfg`; on the A34 Samsung's IMS service may configure the modem over Samsung
IPC instead), IPsec (Android runs `ipsec_mon`; whether the modem keeps the IMS SAs internal), and
VoWiFi's ePDG/IKE on the AP (`ccci_ikeraw`, `ccci_epdg1..4`). VoLTE uses no new identity: the
modem registers with the SIM's IMS identity and the device's own IMEI.

## Biggest unknowns

1. Whether LK boots our loader from `boot` (v4) and leaves everything as on a normal Android boot.
2. The file server protocol (to be read out of `ccci_mdinit`) and whether the modem reaches HS2
   when its NVRAM writes do not persist.
3. Whether this Samsung "IPC_CUSTOM" modem answers MediaTek/3GPP AT on ttyC0 for registration,
   SMS and calls (factory and md_monitor use AT, Samsung's RIL does not).
4. Call audio: AFE, codec, speech control, ADSP -- a whole audio stack.
5. The iOS face: without CommCenter the native Phone/Messages/Settings stay dark; our hooks and
   apps come first.
