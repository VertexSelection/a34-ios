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

First phone test:
- Kernel: artifact of xnu-a34 on branch `cellular` (main 0938 + 0950).
- Boot args: the README's set plus `a34cell=1` (433 characters; the 607 limit holds).
- Optional user space: artifact `a34cell` of `.github/workflows/a34cell.yml`;
  `sudo tools/root-add-job.py /mnt com.a34.cell /a34 a34cell/a34 a34cell`.
- Success, in `tools/pull-log.sh`'s log: `A34 CELL: stage 0 probe`, an LK line that says
  `all zero: the boot loader did not load the modem` (expected on this recovery boot), `MD1 power
  domain off` (or on), `md1img ... md1rom ... CHECK_HEADER v6: platform MT6877_S00`, 24
  sections, the partition bounds, and the summary. With the daemon: `a34cell: kernel: stage0 ...`
  then `no modem port /dev/a34at0 ... nothing to do`. Any hang: the probe touches only SPM and
  infracfg status registers, LK's DTB copy and md1img; report the last `A34 CELL` line.

### Stage 1a -- boot so that LK loads the modem (needs the user's decision)

LK loads and verifies md1img only when it boots normally, i.e. from `boot`. Replicating its
loader (signature check, EMI MPU through ATF, the tag table) from XNU is not realistic. So XNU has
to start from `boot`: our loader as the kernel of a boot image v4 (A34's stock `boot` is v4 with
`vendor_boot`/`init_boot`; `recovery` keeps our current image with the TWRP escape). That
replaces Android's kernel (Android cannot start until stock `boot.img` is flashed back) and is
untested: the next step is a v4 packer (`tools/mkboot-v4.py`) and one boot to see LK hand over
(`A34 CELL: ... modem LOADED by LK`, the regions, MD1 on after BROM). Rescue stays: Vol Up+Power
to `recovery`, download mode for stock `boot.img`. Same switch will be needed for audio (ADSP).

### Stage 1b -- the control path in the kernel (`a34cell=2`)

Only on a boot where LK loaded the modem. Read LK's tag table (smem layout tags
`nc_smem_layout*`/`c_smem_layout*`, check header), map the shared memory (below 4 GiB, outside
XNU's RAM), power on in Android's order with the DTS's exact voltages and nothing else, CCIF
rings, HS1 -> runtime feature list -> HS2, then character devices: `/dev/a34at0` (ttyC0),
`/dev/a34fs` (the file server channel), later `ccci_aud` and the IMS channels. Log lines:
`A34 CELL: HS1`, `runtime data sent`, `HS2: modem ready`, per-port open.

### Stage 1c -- the modem's file server, read only

Protocol from `ccci_mdinit`'s file server (op names in its strings: open/read/write/seek/close/
get-file-detail/..., BIN_REGION, OTP), served by `a34cell-fsd` from `nvdata`, `protect1`,
`protect2`, `nvcfg` through a read-only ext4 reader on `sweet_ufs_read_part()` (list extended to
those four, still read only; no journal replay -- unlike a Linux `ro` mount). Writes: RAM overlay
per session. Refused: BIN_REGION, OTP. Success: HS2, then `a34cell: ATI: ...` and
`AT+CPIN?` in the log with the transmitter still off.

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
