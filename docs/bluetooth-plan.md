# Bluetooth on the Galaxy A34 under iOS 16.7.16: research and plan

Status (2026-11-15):

- **Milestone 1 passed on the phone** (boot `0961-radio-a`, kernel 0961 with Wi-Fi up in the same
  boot, `a34bt=1`): CONNSYS up (A-die MT6635 E3, `0x66358a11`), both images in EMI and read back,
  BGF IP 0x02040200, MCU idle after 40 ms, `WMT_FUNC_CTRL` status 0, HCI_Reset status 0, controller
  HCI 12 rev 0x3143, LMP 12 sub 0xffd7, manufacturer 0x0046, address 00:00:46:67:61:01 (MediaTek's
  default), 6 of 6 commands, up in 7.9 s. The `a34bt probe` job opened the raw channel but sent
  nothing: 0939's packet framing had no length for HCI commands (type 1); fixed in 0970. Its stdout
  did not reach the log either (nor `a34wifi`'s): `a34bt` now writes to `/dev/console` itself.
- **Boot 0980-btmetal-a (0970 + 0971, `a34bt=2`)**: address `ed:68:5c:0b:9a:53` set, Skywalk pipe,
  nodes, control and registry service all up; `a34bt`'s own LE scan heard 30-50 reports / 10 s from 6
  Apple devices (best -46 dBm), no proximity pairing. bluetoothd ran 15 times and never connected.
  From the binary: it passes `FindLocalMacAddress` and the registry check (`+0x266bc`), then has BlueTool
  boot the chip (`+0x272034`: `{kMsgId: RunBuiltinScript, kMsgArgs: {script: boot, btaddress}}` to
  `com.apple.BlueTool`, reply read at `+0x27249c`: `result` 0 ok, 10/13 "Unknown platform", not a
  dictionary = status 1); BlueTool's job was never started, so status 1 = "Init failed, still in high
  power result=1" and `_exit` (`+0x3a6128`). Next round: `a34bt-btd` + `liba34bt-btd.dylib` answer
  BlueTool inside bluetoothd and put its stderr/os_log on the console; 0972 fixes the LE counter and
  counts Apple advert types.
- **Boot 0988-all-a (0972, `a34bt-btd`)**: BlueTool answered in-process; bluetoothd then `_exit(1)`
  from its stack init: `readRootKeys` (`+0x594fac`) could not store its identity root in the keychain
  (service `MobileBluetooth`; securityd cannot wrap keys on this kernel's AppleKeyStore, selector 10).
  `liba34bt-btd` now keeps bluetoothd's `MobileBluetooth` items in
  `/var/mobile/Library/Bluetooth/com.a34.keychain.plist` and puts its os_log on the console.
- **Boot 0988-btkc-a**: keychain answered; bluetoothd chose H4BC, read its (empty) paired-device list,
  then crashed in `liba34bt-btd`'s own os_log formatter (an os_log `mask.hash` item taken for a string
  pointer). Fixed; the library now also reports crashes as image+offset frames.
- **Boot 0989-bt-a**: no crash; identity root refused again: the library only caught service
  `MobileBluetooth`, but bluetoothd's root keys are under `BluetoothGlobal` (LE devices under
  `BluetoothLE`, `BluetoothLESync`). All four services are kept now, each call logged.
- **Boot 0989-dbe-a**: keychain kept; bluetoothd connected (`com.apple.uart.sk.bluetooth`, the Skywalk
  channel) and ran HCI init over the bridge, every command answered 0, then refused
  Write_Default_Erroneous_Data_Reporting itself (status 628): the MT6635's supported-commands mask lacks it
  (octet 18 bit 3), and bluetoothd checks the mask first (descriptor table `+0x8448e0`). 0973 advertises
  and answers 0x0C5A, 0x0C5B, 0x0C84 in the kernel.
- **Milestone 2 built, not yet run**: `xnu/patches/0970-a34-bt-address.patch` (the phone's own
  address, the raw-channel fix) and `xnu/patches/0971-a34-bt-bridge.patch` (`a34bt=2`: bluetoothd's
  transport, see section 6b), `userland/a34bt` (`a34bt scan`, `a34bt-bluetool`).

Sources are cited as `file:line`. "Samsung" means Samsung's A34 kernel source as published by
[Fede2782/android_kernel_samsung_a34x](https://github.com/Fede2782/android_kernel_samsung_a34x),
branch `sep-16.0/stock`, commit
[`79c21a1`](https://github.com/Fede2782/android_kernel_samsung_a34x/tree/79c21a19d1913e830f63a82aa34e1b5a2ebad3b9)
("Import A346BXXSADYG1 OSRC", the same DYG1 build as our firmware); `connectivity/` is its
`vendor/mediatek/kernel_modules/connectivity/`, `kdm/` its `kernel/kernel_device_modules-6.6/`.
"HackiOS" means [oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel)
branch `mtk` at `a962798` (Bluetooth in `c36d664`; MIT, XNU-derived files APSL 2.0), cloned locally
at `research/metal/prior/hakios-mtk`. Only register facts and protocols are taken from MediaTek's
GPL driver, no code. Phone facts are from `recon/android-DYG1/` (`live.dts`, `dmesg.txt`,
`iomem.txt`) and the DYG1 vendor partition; none of that is in git.

## 1. The hardware path

```
iOS bluetoothd ─(H4BC: kernel control + Skywalk channel, milestone 2)─┐
userland/a34bt ─(raw HCI: kernel control com.a34.bt.hci, milestone 1)─┤
                                                                       ▼
   XNU: a34_bt.c ── H4 packets, PIO ──► BTIF 0x1100c000 (UART-like FIFO, hardware handshake)
                                              │
                         CONNSYS (MT6877 "CONNAC 2.0", conn_infra 0x18000000, version 0x02060002)
                           ├─ conn_infra: power, clocks, sleep protection, its bus (a34_connsys.c)
                           ├─ BGF subsystem 0x18800000: N10 MCU (BT firmware), IP version 0x02040200
                           └─ A-die (radio)       EMI: CONNSYS's memory in DRAM, firmware copied here
```

| fact | evidence |
| --- | --- |
| CONNSYS is the MT6877 "consys", conn_infra at 0x18000000, HW version id 0x2060002 | `live.dts` `consys@18000000` `"mediatek,mt6877-consys"`; `dmesg.txt:462` "Consys HW version id(0x2060002)"; `connectivity/bt/mt66xx/include/btif/platform_6877.h:178` `CONN_INFRA_CFG_ID 0x02060002` |
| Bluetooth driver is `bt_drv_6877.ko` (bt/mt66xx, BTIF flavour) over `btif_drv.ko` and `conninfra.ko` | vendor_dlkm `lib/modules`; `connectivity/bt/mt66xx/6877/Kbuild` includes `../btif/Kbuild`; `dmesg.txt:235` "btmtk_btif_send_and_recv" |
| HCI goes over BTIF as plain H4 (no STP, unlike the MT6768/MT6631 of HackiOS) | `connectivity/bt/mt66xx/btif/btmtk_btif_main.c:1460` `btmtk_btif_send_cmd` -> `mtk_wcn_btif_write` of the H4 bytes; `:854` `bt_receive_data_cb` -> `btmtk_recv` |
| WMT is tunnelled in HCI: vendor command 0xFC6F, answer event 0xE4 | `btif/btmtk_mt66xx.c:89` `WMT_OVER_HCI_CMD_HDR {01 6F FC 00}`; `:747-797` `_send_wmt_power_cmd` (`waiting_event 0xE4`); `include/btif/btmtk_btif.h:283` `struct wmt_pkt_hdr` |
| For 6877: no BEIF, no ATF/SMC path, firmware copied by the AP (`SUPPORT_BEIF=0`, `SUPPORT_BIN2IMG=0`, `CFG_BT_ATF_SUPPORT=0`) | `connectivity/bt/mt66xx/btif/Kbuild:54-96` (6877 falls in the `else` branch) |
| BT register map (`bt@18000000` reg order: RGU 0x18000000, CFG 0x18001000, SYS 0x18050000, host CSR 0x18060000, BGF 0x18800000, BGF info 0x18812000, infracfg_ao 0x10001000) | `live.dts` `bt@18000000`; `include/btif/platform_base.h:154-160`; `btif/btmtk_btif_main.c:103` `bt_reg_init` |
| BTIF at 0x1100c000, IRQ SPI 202, AP-DMA TX/RX channels 0x10221200/0x10221280, clock `ifrao_btif` = infracfg_ao CG0 bit 31 (set 0x80, clr 0x84, sta 0x90) | `live.dts` `btif@1100c000`; `kdm/drivers/clk/mediatek/clk-mt6877.c:1901-1905,2018` |
| BTIF registers and init (normal mode, new handshake, FIFOs cleared, TX/RX trigger 8/1, no DMA, auto-reset) | `kdm/drivers/misc/mediatek/btif/common/plat_inc/btif_priv.h:28-113`; `btif_plat.c:488-532` `hal_btif_hw_init` -- same IP as HackiOS's MT6768 (`redwood_connsys.c` step 6) |
| BT interrupts (not used by milestone 1, which polls): SPI 464 BGF2AP BTIF wakeup, 479 BGF2AP SW IRQ, 465 BT CONN2AP SW IRQ | `live.dts` `bt@18000000` `interrupts` |
| CONNSYS memory (EMI): 0x780000 bytes, 1 MB aligned, anywhere in 0x50000000-0x90000000, "no-map" (Android put it at 0x51100000) | `live.dts` `consys-reserve-memory`; `iomem.txt` `51100000-51c77fff : reserved` |

### Firmware

`fwp_get_patch_names` (`platform_6877.h:1381`) composes `soc5_0_ram_mcu_1` / `soc5_0_ram_bt_1`
(`:175-176`) with `"_1_hdr.bin"`, or with a "flavor" letter when the `mediatek,bt` node has a
`flavor_bin` property (`platform_base.h:320-356`). The A34's `bt@18000000` has none, so:

| file (DYG1 `/vendor/firmware`) | bytes | built | EMI address in header -> offset |
| --- | --- | --- | --- |
| `soc5_0_ram_mcu_1_1_hdr.bin` (BGF MCU ROM patch) | 141618 | 2024-10-18 14:27:44 | `0xf0000000` -> 0x0 |
| `soc5_0_ram_bt_1_1_hdr.bin` (Bluetooth RAM code) | 2911132 | 2024-10-18 14:29:07 | `0xf005bc11` -> 0x5bc00 |

(`1c` variants of both exist for another sub-version.) Each starts with a 48-byte EMI header
(`btif/btmtk_mt66xx.c:46-55` `struct fw_patch_emi_hdr`: 16 bytes date, "ALPS", hw/sw version,
EMI address, subsystem, CRC); the image after it is copied to EMI base +
`(emi_addr[2] << 16 | emi_addr[1] << 8)` (`:422-533` `__download_patch_to_emi`). Both fit below
0x330000 of the 0x780000 region and do not overlap. `BT_FW.cfg` (coexistence, antenna) is all
zeros/commented on the A34; the antenna WMT command is not built for 6877
(`btif/Kbuild:98-118`).

### How Android switches Bluetooth on (`btmtk_set_power_on`, `btif/btmtk_mt66xx.c:1792-2071`)

1. `conninfra_pwr_on(CONNDRV_TYPE_BT)`: CONNSYS's common part (section 2).
2. `bgfsys_ccif_on`: 0x10003204 = 0 (tells the modem's coexistence BT is on, `platform_6877.h:204`).
3. `bt_hw_and_mcu_on` (`:633-700`):
   - `bgfsys_check_conninfra_ready` (`platform_6877.h:268-297`): host CSR 0x180601A8 bit 0
     (conn_infra forced awake for BT), poll 0x18001000 == 0x02060002 (10 x 1 ms);
   - `bgfsys_bt_patch_dl` (`:587`): both images into EMI (above);
   - `bgfsys_power_on` (`platform_6877.h:994-1220`): BGF CPU held (0x18000124 bit 0 = 0);
     0x18000020 = 0x42540080 | (old & ~0xffff0080); poll 0x18000414 bits 25:24; BT_FUNC_EN
     0x18001208 bit 0; poll 0x18000424 bit 24 then bit 0; clear the four GALS sleep protections
     (0x18001550 bits 4, 0 then 0x18001560 bits 4, 0; poll 0x18001554/0x18001564 bits 22, 23 clear);
     BGF IP version 0x18812010 == 0x02040200; 0x18800100 bit 31 cleared, 0x18800104 = 4; FW-own
     IRQs cleared (0x18060034, 0x1806003C); BGF CPU let go; poll 0x1880016C == 0x1D1E (ROM idle,
     100 x 5 ms); 0x1803F014 |= 1, |= 2; 0x180601A8 bit 0 cleared;
   - `btmtk_wcn_btif_open`: BTIF open (`hal_btif_hw_init`).
4. TX thread first does "driver own" (`btmtk_btif_main.c:415-490`): write 0x18060030 = 2
   (CLR_FW_OWN) every 16 tries, until bit 2 (OWNER_STATE_SYNC) of it reads 0, then 0x18060034 = 1.
5. `btmtk_intcmd_send_connfem_cmd` (skipped when the BT FEM has vid/pid 0), WMT
   `FUNC_CTRL` BT on: `01 6f fc 06 01 06 02 00 00 01`, answered `04 e4 05 02 06 01 00 <status>`
   (`:1037`, `:747`). Then UTC sync, screen-state, and the stack's own `HCI_Reset`.

## 2. CONNSYS's common part (conninfra)

Everything above needs `conninfra_pwr_on(CONNDRV_TYPE_BT)` first. The first call of a boot, with
the A34's settings, is reconstructed register by register (about 210 operations; the full table
with `file:line` for each was made from `connectivity/conninfra/conn_drv/connv2/` and lives in
`research/bluetooth/conninfra-pos.txt`, gitignored with the sources). Below, `POS` is
`platform/mt6877/mt6877_pos.c`, `PMIC` `platform/mt6877/mt6877_pmic.c`, `HW`
`platform/consys_hw.c`, `REG` `platform/mt6877/mt6877_consys_reg.c`.

What decides which code runs on this phone:

- **SRCLKEN_RC ("RC mode") is on**: the preloader prints `RC_CENTRAL_CFG1:0x10401425`
  (`recon/android-DYG1/expdb-after-xnu1.bin`); conninfra reads 0x1000F804 bit 0 (`POS:2075-2105`).
  So the LDOs are put under the resource centre's control and VCN33_1 is never switched on by
  software (`PMIC:376-380`).
- **26 MHz co-clock from the PMIC DCXO** (`/vendor/firmware/conninfra.cfg` = `co_clock_flag=1`,
  no `tcxo_support` in the DT; `platform/mt6877/mt6877.c:145-162`).
- **The power domain goes through Linux's SCPSYS** (`kdm/drivers/soc/mediatek/mtk-scpsys.c:607-702`
  with `mtk-scpsys-mt6877.c:62-73`), not conninfra's own SPM sequence (`POS:177-437`, which does not
  even compile: no `;` at `:436`); `conninfra.ko` has the strings of the former only.
- **A-die = MT6635** E2 or E3 (`mt6877.c:559-562` returns 0x6635; `POS:1057-1062` accepts only
  0x6635xxxx; the thermal constant 28 in `dmesg.txt:464` rules out E1, `mt6877.c:423-425`).
- `CONFIG_MTK_EMI_LEGACY` is on (`conninfra.ko` has "setting MPU for EMI share memory"), so Linux
  programs EMI MPU region 27 over CONNSYS's memory (`platform/mt6877/mt6877_emi.c:70-91`).

| phase | what (BT-only first power-on) | where |
| --- | --- | --- |
| 0 | EMI MPU region 27 = [base, base+0x77FFFF], domains 0 (AP), 2 (CONN), 3 (SCP) allowed, others forbidden: SiP SMC 0xC200050B, x1=0, x2=(start>>16)\|(27<<24), x3=(end>>16)\|(group<<24), x4=3 bits per domain; group 1 then group 0 | `mt6877_emi.c:70-91`; `kdm/drivers/memory/mediatek/emi_legacy/emi_legacy_v2/emi-mpu.c:382-450`; `kdm/include/linux/soc/mediatek/mtk_sip_svc.h:30` |
| 1 | PMIC (MT6359P via PWRAP 0x10026000): VCN18 OP_EN 0x1C36 bits 7..4, LP off, on (0x1C2E bit 0); VCN13 likewise (0x1C24, 0x1C1C), 1.3 V (0x202E [11:8]=4) | `PMIC:158-231`; `kdm/drivers/regulator/mt6359p-regulator.c:227-229,733-747` |
| 2 | GPIO196-206 to their CONN functions (0x10005480/0x10005490), IOCFG_RT 0x11EA0000 drive and pulls | `POS:469-523` |
| 3 | SPM CONN_PWR_CON 0x10006E04: PWR_ON, ack 0x10006EF0 bit 1, PWR_ON_2ND, ack 0x10006EF4, CLK_DIS off, ISO off, RST_B on; infracfg_ao bus protection released (0x100012A4 = 0x84000, then 0x42000; STA1 0x10001228 bits 13/19 clear) | `mtk-scpsys.c:607-702`; `mtk-scpsys-mt6877.c:62-73`; `POS:263-300` |
| 4 | first CONNSYS access: 0x18001000 == 0x02060002 (10 x 1 ms) | `POS:573-605` |
| 5 | EMI remap: 0x180601C4 [19:0] = base >> 16; peripheral remaps 0x180601D4/D8/DC = 0x01000 | `POS:63-92` |
| 6 | D-die: sysram 0x18050000 (16 KB) cleared, CONN2AP EMI path 0x1800E360/364, efuse to sysram, sysram power control, build mode | `POS:607-664` |
| 7 | sleep controller's A-die command table 0x18005004-0x1800509C (40 writes, BT-only values) | `POS:761-877` |
| 8 | A-die over the RF-SPI TOP channel (0x18004000, busy bit 5; HW semaphore 12 at 0x18072030/0x18072230; TOP clock 0x18005120): reset released (0x18001030 bit 0), chip id 0x6635xxxx, WRI_CTR2, SMCTK11, efuse into sysram 0x18052804.., thermal trim, XTAL_01/02, WF bandgaps; GPIO204 held high as a GPIO meanwhile | `POS:998-1154, 880-995, 1737-1918, 1668-1724` |
| 9-11 | AFE WBG calibration (0x18003000..), PLL enables (0x18003004/0C/F4), RADIO_STATUS bit 0 in sysram under semaphore 23 | `POS:1156-1271, 2044-2073` |
| 12 | BT voltage raise: VS2 0x18B4 = 0x35 (1.4625 V) voted (0x18AC bit 4), sleep 0x188E = 0x2E, VCN13 trim 0x202E [3:0] = 7 (1.37 V) | `PMIC:484-615` |
| 13 | A-die clock buffer (BT-only table), RC oscillators 0x18001380-0x180013C4, EMI/ddr_en timeout 0x18001400, bus DCM 0x1802E03C/40, bus timeouts 0x1800E300/31C and 0x1800F000, TOPRGU 0x10007200 bit 12 (key 0x88), bus/RF-SPI clocks 0x18009000.., sleep counter 0x18060380, coex/PTA clocks 0x18009A00; last: 0x18001200 bit 0 (conn_infra may sleep) | `POS:1273-1666, 687-759` |
| 14 | VCN18/VCN13 low-power bits | `PMIC:289-319` |
| 15 | VCN33_1 3.3 V for BT: OP_EN 0x1BEA bits 6, 5, LP off, VOSEL 0x1F98 [11:8] = 0xD; RC mode: no software enable | `PMIC:334-380` |

After phase 13 conn_infra may deep-sleep: an access needs the wakeup handshake (a request bit
in 0x180601A0.. -- Bluetooth uses its own, 0x180601A8 -- then the version poll), and reads while
asleep return 0xDEADFEED once phase 13's bus timeouts are set. Linux's readability guard
(`REG:570-626`): infracfg_ao 0x10001228 bits 13/19 clear, clock detect (0x18060000 bit 0 written,
bits 2:1 read 0b11), version 0x02060002, 0x180602D4 bit 0 set.

**Pre-calibration** (Android powers BT and Wi-Fi together once and keeps the calibration data) is
software orchestration only (`core/conninfra_core.c:596-744`) and can be skipped: conninfra itself
disables it unless both radios exist (`:2462-2466`). Cost, per the BT driver's own comment
(`btif/btmtk_mt66xx.c:1300-1306`): the calibration done by a BT-only function-on "is most likely to
be failed due to Wi-Fi is off" -- the radio works, possibly less well.


## 3. What iOS 16.7.16 needs

Checked on the 20H392 root and dyld cache (bluetoothd, BlueTool, MobileBluetooth; the dyld cache in
`research/metal/dsc` is byte-identical to the IPSW's), and against HackiOS's iOS 16.5 findings.
Addresses are unslid, in 16.7.16's `/usr/sbin/bluetoothd`.

- **Transport: H4BC, as on 16.5.** bluetoothd picks it (`0x100156160`) from the environment
  variable `BT_HCI_TRANSPORT`, else the registry service `marconi-bt` / `bluetooth`'s
  `transport-encoding`, else 3 (`0x100844880`). 0 USB, 1 H4, 2 H4DS, 3 H4BC, 4 H5, 5 BCSP, 6 APPLEBT,
  7 PCIe. The H4 family's only driver ("POSIX", open at `0x100066bdc`) connects a stream socket to the
  kernel control **`com.apple.uart.sk.bluetooth`**, sets option `0x800c5414` (12 bytes, {0x400, 63,
  63}), reads option `0x40105412` (the 16-byte UUID of a Skywalk channel) and `os_channel_create`s it.
  `/dev/cu.bluetooth` and `/dev/uart.bluetooth` are only tested for existence; H4BC opens
  `/dev/btwake` and only logs a failure. The job has the entitlement `skywalk.register-user-pipe`.
- **D22's device tree**: `/arm-io/uart1/bluetooth` (`compatible = bluetooth,n88`,
  `transport-encoding = 3`, `transport-speed = 3000000`, `local-mac-address`, `function-bt_wake`,
  `function-power_enable`, vendor 0x5ac, product 0x12a0, coex 2, calibration blobs). Our
  `tools/mkadt_a34.py` tree has none of it (only `/product bluetooth-le`).
- **Registry**: bluetoothd reads `local-mac-address`, `transport-encoding`, `transport-speed` of
  the service named `marconi-bt`, else `bluetooth`; without a non-zero address: "No bluetooth on this
  device." MobileBluetooth's `BTSessionAttachWithQueue` (`0x1b08fd88c` in the cache) needs
  MobileGestalt `bluetooth`, not a simulator, and that service, else error 9 (Settings' switch grey).
- **BlueTool** (`/usr/sbin/BlueTool -R`, user mobile, MachService `com.apple.BlueTool`, on demand):
  bluetoothd asks it to run its "boot"/"init" scripts. It carries 221 Broadcom firmware entries
  chosen by `ProductHash` + `WifiVendor` (D22: BCM4357B1 family) and talks to `com.apple.uart.bluetooth`
  on a UART phone. Nothing in it fits a MediaTek controller: HackiOS answers its service from inside
  the real process (`userland/src/inject.c`, `bluetool_serves`), and so will we.
- **bluetoothd** runs as mobile (`KeepAlive` on failure), so the kernel control must not be
  privileged. Its companions per HackiOS -- keybagd, securityd, tccd, wifid, configd -- exist under
  the same labels in 16.7.16.
- **Kernel**: our XNU (8796.141.3, DEVELOPMENT, SWEET) has Skywalk with the kernel-pipe nexus and
  the user channel syscalls (checked in the 0938 kernel's symbols); the Skywalk sources HackiOS's
  `redwood_btsk.c` uses are identical between its xnu-8796.121.2 and ours.
- **An untested shortcut**: `BT_HCI_TRANSPORT=H4BC:POSIX:<control>` makes bluetoothd use plain
  read/write on a kernel-control socket (options `0x40487413`/`0x80485414`, a termios) -- no Skywalk.


## 4. HackiOS: what carries over

HackiOS (`c36d664`) made iOS 16.5's real bluetoothd use an MT6631 in the MT6768's CONNSYS: apps saw
BLE devices, Settings showed "discoverable as iPhone", classic inquiry/connection/SDP worked, pairing
stopped at the numeric confirmation (`docs/bluetooth-x6826b.md`, `docs/bluetooth-mtk-porting.md`).

| HackiOS piece | on the A34 |
| --- | --- |
| `redwood_connsys.c` steps 1-11 (MT6768 WMT power-on, ROM patch over BTIF/STP, WMT init script) | **does not carry**: MT6877 is CONNAC 2 -- conninfra power sequence (section 2), firmware copied into EMI, BGF power-on; no STP, no WMT script. Rewritten: `a34_connsys.c`, `a34_bt.c` (milestone 1) |
| BTIF register programming (normal mode, new handshake, FIFOs, PIO, 16-byte bursts on TEMT) | **carries** (same IP, same offsets; clock gate also infracfg_ao CG0 bit 31) |
| `WMT_FUNC_CTRL` BT on | **carries in substance**: same payload `01 06 02 00 00 01`, but wrapped in HCI 0xFC6F and answered by event 0xE4 instead of an STP frame |
| `redwood_mtkbt_skel.c` (kernel control `com.apple.uart.sk.bluetooth`, `/dev/btwake` nodes, six-command probe, HCI quirks, bridge thread) | **carries** (APSL): milestone 2 fills its `struct mtkbt_platform` with milestone 1's power_on/function_on/send/receive. Quirks were found on an MT6631 and only fire on the exact answers it gave; MT6635/BGF firmware may need others |
| `redwood_btsk.c` (Skywalk kernel pipe) | **carries** unchanged (same Skywalk sources) |
| `RedwoodBluetooth.cpp` (registry service `bluetooth`, `local-mac-address`) | **carries**; the address source differs (below) |
| `userland/src/inject.c`: `bluetool_serves` (BlueTool's boot/init answered), `bt_entry_fix` (scan entries without a service list deliver), os_log capture, `_exit` hooks | **carries with new offsets** for 16.7.16: `bt_entry_fix` target `bluetoothd+0x58e944` (16.5: `+0x58e798`; all 23 hook prologues re-match at +0x1ac / +0x1d4), `_exit` sites `0x90778`/`0x907e8`. Loaded through our wrapper approach (`a34gl-bbd`-style), not HackiOS's `inject.conf`. `kern.redwood.host_keep` does not exist here: use the console |
| `net_test` ComputerName, `bt_test` | **carry** (MIT) |


## 5. Plan

Sizes are new lines of code; "phone tests" counts boots the user has to make.

| | milestone | kernel | user space | phone tests | main risks |
| --- | --- | --- | --- | --- | --- |
| **M1** | controller powered, firmware in, `WMT_FUNC_CTRL` answered, **HCI_Reset answered** in the kernel log; `a34bt probe` scans | `a34_connsys.c` (~880), `a34_bt.c` (~950), hooks in `sweet_ufs.c` -- **built** (0939) | `a34bt` (~600) + firmware installer -- **built**, CI green | 1-4 (`a34btstep=N` to bisect a hang) | a bus hang in the power sequence; RC-mode rails not lifted under XNU; EMI MPU/remap wrong (BGF MCU never idles); BTIF PIO RX overruns |
| **M2** | bluetoothd takes the controller: address in Settings, switch on | port HackiOS's skeleton + `redwood_btsk.c` + `RedwoodBluetooth.cpp` (~1100, mostly carried), non-privileged `com.apple.uart.sk.bluetooth`, BD address published | `a34bt-inject` in BlueTool (`bluetool_serves`) and bluetoothd (`bt_entry_fix` at +0x58e944, log capture), a wrapper per job, ComputerName (`net_test`), `bt_test` (~800, mostly carried) | 2-5 | which vendor commands bluetoothd 16.7.16 sends to this controller and what answers it needs (HackiOS needed 5 rewritten answers for the MT6631); MobileGestalt `bluetooth`; sandbox `BTServer` |
| **M3** | discovery / scan from Settings and apps | none expected (maybe more quirks) | `bt_test` scan; Settings | 1-2 | the scan-entry fix; RF quality without pre-calibration |
| **M4** | pairing + one profile (HID keyboard, or BLE GATT) | possibly ACL flow control / DMA RX if PIO is too slow | key storage needs the keybag (HackiOS: `aks_keys=1`, keybagd init) | 3+ | pairing confirmation UI path untested anywhere; A2DP/HFP need audio (`media` branch) and SCO routing (`mtk-btcvsd`) |

Not planned: sleep/low power (BGF "FW own", conn_infra sleep are left as Linux leaves them at
power-on, nothing is ever switched off), Wi-Fi coexistence beyond what the firmware does alone,
pre-calibration (needs Wi-Fi's subsystem up). The Wi-Fi work should reuse `a34_connsys.c` (its
phases are common; `bt_only` selects a few values) instead of powering CONNSYS a second way.

**The Bluetooth address.** Android's comes from NVRAM (`nvdata`), which this project never
touches. M2 will use what the controller reports (`Read_BD_ADDR`; M1 logs it) if it is a real
unicast address, else a random static address generated once and kept in `/a34/bt-address`, and
publish it as `local-mac-address`.


## 6. Milestone 1 as built

| file | what |
| --- | --- |
| `xnu/patches/0939-a34-bt-m1.patch` | against the reference tree at `543f537` (0938): new `bsd/dev/a34_connsys.c`, `bsd/dev/a34_bt.c`, two lines in `bsd/conf/files`, `a34_bt_init()` and `sweet_log_flush()` in `bsd/dev/sweet_ufs.c`. Nothing runs without `a34bt=1` |
| `userland/a34bt/` | `a34bt` (status, wait, probe, dump, cmd), `install-firmware.sh`, `INSTALL.md`; CI `.github/workflows/a34bt.yml` |

Checked: the kernel compiles and links with 0939 (`xnu-a34` run
[43677958340](https://github.com/VertexSelection/a34-ios/actions/runs/43677958340), branch
`bluetooth` = 0938 + 0939; warnings only, none of them errors; the artifact's kernel carries both
drivers); `a34bt` builds with `-Wall -Wextra -Werror` (`a34bt` workflow). Nothing has run on the phone.

Kernel steps (each logs `A34 BT: step N: ...` / `A34 CONNSYS: phN: ...`; the steps that can hang
a bus first write the log to disk and wait a second, `a34btsync=1`):

1. waits (up to `a34btwait`, 180 s) for the root and `/a34/firmware/soc5_0_ram_{mcu,bt}_1_1_hdr.bin`,
   checks their EMI headers;
2. `a34_connsys_on()`: phases 0-13 and 15 of section 2 (14 skipped unless `a34btlp=1`; VCN33_1 also
   switched on by software unless `a34btvcn33=0`; conn_infra's deep sleep enabled unless
   `a34btdeep=0`; EMI MPU SMC unless `a34btmpu=0`; CONNSYS memory at `a34btemi`, default
   0x60000000, outside XNU's RAM, which starts at 0x1_40000000);
3. conn_infra forced awake for Bluetooth (0x180601A8), version read;
4. both images copied into CONNSYS memory (uncached normal mapping, 32-bit stores, read back and
   summed);
5. `bgfsys_power_on` as in section 1, BGF IP version 0x02040200, MCU idle 0x1D1E;
6. BTIF clock and init, PIO, no interrupts (polled: spin while bytes come, 1-4 ms naps when idle);
7. driver own;
8. `WMT_FUNC_CTRL` BT on, status 0 expected;
9. HCI_Reset, Read_Local_Version_Information, Read_BD_ADDR, Read_Local_Supported_Features,
   Read_Buffer_Size, LE_Read_Buffer_Size, each Command Complete with status 0 (as HackiOS's probe;
   with fewer the raw channel still opens, for `a34bt cmd`/`dump`);
10. kernel control `com.a34.bt.hci` (root only; one H4 packet per datagram, both ways), state `up`
    in `sysctl kern.a34bt`.

`a34bt probe` (a one-shot launchd job) waits for `up`, repeats the reset and reads, then runs an
8-second passive LE scan and an 8-second inquiry and prints counts and the strongest RSSI (never
other devices' addresses).

### First phone test

1. Firmware into the root: `sudo userland/a34bt/install-firmware.sh <vendor>/firmware /mnt`
   (`vendor.img` of DYG1's `super.img`, mounted read-only).
2. `sudo python3 -I tools/root-add-job.py --copy /mnt /a34 a34` and
   `sudo python3 -I tools/root-add-job.py /mnt com.a34.btprobe /a34 a34 a34bt probe 8` with the
   CI artifact's `a34/`.
3. Kernel with 0939; boot-args as now plus **`a34bt=1`** (optionally `ufslogsecs=2`).
4. Success in `tools/pull-log.sh`: `A34 CONNSYS: up`, `A34 BT: step 5: BGF MCU idle`,
   `A34 BT: in  8: 04 e4 05 02 06 01 00 00`, `A34 BT: in  7: 04 0e 04 xx 03 0c 00`
   (HCI_Reset: **M1 reached**), `6 of 6 HCI commands`, then `a34bt: LE scan 8 s: N advertising
   reports ...` and `a34bt: PASS`.
5. A log ending right after an `A34 BT: step N:` / `A34 CONNSYS: phN:` line without its result =
   that step hung; next boot `a34btstep=N-1` (stops before it, the rest of the boot unaffected).


### 6b. Milestone 2 as built (0970, 0971)

| piece | what | where |
| --- | --- | --- |
| address | random static (two top bits 1, LAP not 0x9E8B00-3F), one per phone in `/a34/bt-address` (installer, or the kernel when missing); set with MediaTek's `01 1a fc 06 <addr, low byte first>` (answered `04 0e 04 01 1a fc 00`, `connectivity/bt/mt66xx/btmtk_buffer_mode.c:90-91`) after bring-up and before any HCI_Reset's completion is passed on; checked with Read_BD_ADDR | `a34_bt.c` (0970) |
| transport | kernel control `com.apple.uart.sk.bluetooth` (stream, unprivileged; option `0x800c5414` taken, `0x40105412` answers the channel UUID) and the Skywalk kernel pipe | `a34_bt.c`, `a34_btsk.c` (HackiOS `redwood_btsk.c`, APSL) |
| registry | service `bluetooth` under the platform expert, `local-mac-address` = the phone's address (no `transport-encoding`: bluetoothd's default 3 is H4BC) | `iokit/Kernel/arm/A34Bluetooth.cpp` |
| nodes | `/dev/btwake`, `/dev/btreset`, `/dev/btpoweroff` (H4BC opens btwake) | `a34_bt.c` |
| vendor commands | every OGF 0x3f command from bluetoothd answered in the kernel (Command Complete, status 0; 0xFD48 +1 byte, 0xFCE2 sub 0x15/0x36 +16, 0xFD1B +64 starting 0x51), never forwarded: MediaTek's own vendor commands (0xFC1A, 0xFC6F, ...) share the space | `a34_bt.c` `br_vsc` |
| event fixes | 0x0C5B unknown -> done; Read_Local_Supported_Commands LE octets added only if octet 25 is 0 (logged either way); unknown commands logged | `a34_bt.c` `br_quirks` |
| BlueTool | job `com.apple.BlueTool` wrapped to `/a34/a34bt-bluetool`, which serves the Mach service and answers every script with `result 0` (HackiOS's `bluetool_serves`, as a program instead of inside BlueTool) | `userland/a34bt/bluetool.c` |
| test | `a34bt scan 60`: LE scan of its own, or listening while bluetoothd has the controller; counts Apple adverts and AirPods-type proximity pairing (type 0x07) with model bytes; the kernel logs the same counts every 10 s | `userland/a34bt/a34bt.c`, `a34_bt.c` `br_le_*` |

Checked: main 0962 + 0970 + 0971 compiles and links (`xnu-a34` run
[43714808186](https://github.com/VertexSelection/a34-ios/actions/runs/43714808186): no new warnings in
`a34_btsk.c` or `A34Bluetooth.cpp`; the kernel carries the control, the pipe and the service);
`a34bt` and `a34bt-bluetool` build with `-Werror` (`a34bt` workflow run 43714804186).

Not done yet: HackiOS's `bt_entry_fix` in bluetoothd (only for scans of programs without a bundle;
16.7.16 target `+0x58e944`), bluetoothd's own log capture (its restarts are seen as `exec bluetoothd`
and the last `br>` before each), ComputerName, power-off.

## 7. Unknowns and risks

1. **Bus hang during power-on.** Any CONNSYS read before the power domain and bus protection are
   right stalls the AP bus with no timeout (bus timeouts are only set in phase 13). Mitigation:
   the order is Linux's, the AP-side state is printed before phase 3, the guard of `REG:570-626` runs
   before BGF registers are touched, and the log is flushed before each risky step.
2. **RC mode under XNU.** VCN33_1 and the LDOs' normal mode are switched by SRCLKEN_RC on Android.
   Its configuration is the preloader's, so it should still work; M1 nevertheless keeps the LDOs out
   of low-power mode and switches VCN33_1 on by software (boot-args undo both). The XO_WCN clock
   buffer is left in hardware mode as the preloader leaves it (`PMIC 0x788` is logged).
3. **EMI.** Region placement (0x60000000), the remap and EMI MPU region 27 by SMC are inferred to be
   enough; Linux's other EMI MPU region (31, CONN forbidden over all DRAM) is never programmed under
   XNU, so the MPU call may be redundant (`a34btmpu=0` drops it).
4. **No pre-calibration**: BT-only calibration may fail or be poor; RF range unknown until M1's scan.
5. **BTIF in PIO, polled.** Enough for commands and a scan; ACL throughput (audio) may need AP-DMA RX
   (channels 0x10221200/0x10221280) or the BTIF interrupt (SPI 202) later.
6. **Firmware expectations not replayed**: Linux also sends UTC sync, screen state, connfem (skipped
   when the BT FEM is 0/0) and sets "FW own" sleep; M1 sends none and keeps the controller awake.
7. **M2**: the vendor commands 16.7.16's bluetoothd sends to this firmware and the answers it needs;
   MobileGestalt `bluetooth`; whether our kernel enforces the `BTServer` sandbox; the BD address.
8. Nothing here writes a partition. `efs`, `sec_efs`, `nvram`, `nvdata`, `nvcfg`, `protect1/2`,
   `proinfo` and `param` are never read or written; the BT address will not come from NVRAM.
