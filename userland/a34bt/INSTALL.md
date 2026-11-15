# a34bt: Bluetooth milestone 1 on the phone

Milestone 1 of `docs/bluetooth-plan.md`: the kernel powers the MT6877's CONNSYS and its Bluetooth
subsystem, loads MediaTek's Bluetooth firmware, switches Bluetooth on and asks the controller the
first HCI commands (`xnu/patches/0939-a34-bt-m1.patch`, boot-arg `a34bt=1`). `a34bt` is the user-space
side of the test: it talks to the controller through the kernel's raw HCI channel and makes it scan.
bluetoothd is not involved yet (milestone 2).

| file | what |
| --- | --- |
| `a34/a34bt` | the tool, for `/a34` (ad-hoc signed) |
| `install-firmware.sh` | copies the two firmware files from the phone's own vendor partition into the iOS root |

## 1. Firmware (once)

The kernel reads `/a34/firmware/soc5_0_ram_mcu_1_1_hdr.bin` and `/a34/firmware/soc5_0_ram_bt_1_1_hdr.bin`
from the iOS root. They are MediaTek's and are not in this repository: take them from the A34's
vendor partition of the firmware the phone runs (One UI 7, DYG1). With `vendor.img` from that
firmware's `super.img` mounted read-only at `/mnt/vendor` and the iOS root at `/mnt`:

    sudo userland/a34bt/install-firmware.sh /mnt/vendor/firmware /mnt

It prints each file's size, build time and EMI address (expected: the MCU patch 141618 bytes at EMI
`0xf0000000`, the Bluetooth RAM code 2911132 bytes at `0xf005bc11`).

## 2. The tool and its test job

    sudo python3 -I tools/root-add-job.py --copy /mnt /a34 a34
    sudo python3 -I tools/root-add-job.py /mnt com.a34.btprobe /a34 a34 a34bt probe 8

The second line makes a one-shot launchd job that waits (up to 120 s) for the kernel to say `up`,
then resets the controller, reads its version, address, name, features and buffer sizes, runs an
8-second passive LE scan and an 8-second classic inquiry, and prints one line per result on the
console (so in the kernel log). Other devices' addresses are not printed, only counts and the
strongest signal.

## 3. Boot

Add `a34bt=1` to the boot-args. Optional: `a34btstep=N` stops after step N (1 firmware, 2 CONNSYS
power, 3 conn_infra awake, 4 firmware copy, 5 BGF power, 6 BTIF, 7 driver own, 8 WMT function on,
9 HCI probe, 10 raw channel), `a34btsync=0` skips the one-second log flush before each risky step,
`a34btlog=N` packets of each direction in the log (default 48).

## 4. What success looks like (`tools/pull-log.sh`, then grep `A34 BT\|a34bt`)

    A34 BT: step 1: soc5_0_ram_mcu_1_1_hdr.bin: 141618 bytes, built 20241018142744, platform ALPS, ...
    A34 BT: step 2: ...                       (CONNSYS power, a34_connsys.c)
    A34 BT: step 3: conn_infra 0x2060002 awake for Bluetooth
    A34 BT: step 4: soc5_0_ram_bt_1_1_hdr.bin: 2911084 bytes at EMI ... read back ... (same sum)
    A34 BT: step 5: BGF powered, IP version 0x2040200 (as expected)
    A34 BT: step 5: BGF MCU idle after N ms: sw_dbg_ctl 0x1d1e, ...
    A34 BT: step 7: driver own after N tries
    A34 BT: out 10: 01 6f fc 06 01 06 02 00 00 01
    A34 BT: in  8: 04 e4 05 02 06 01 00 00
    A34 BT: step 8: Bluetooth's function on: status 0
    A34 BT: out 4: 01 03 0c 00
    A34 BT: in  7: 04 0e 04 01 03 0c 00                     <- HCI_Reset answered (byte 4 may differ): milestone 1
    A34 BT: step 9: controller: HCI .. rev ..., LMP .., manufacturer 0x0046 (MediaTek, expected)
    A34 BT: step 9: 6 of 6 HCI commands answered as a controller answers
    A34 BT: UP in N ms: ...
    a34bt: kernel: up: ...
    a34bt: LE scan 8 s: N advertising reports from M devices, strongest -NN dBm
    a34bt: PASS: ...

A line `A34 BT: FAILED at step N: ...` says where it stopped; a log that ends right after
`A34 BT: step N: ...` (without its result) means that step hung the machine.

`a34bt status`, `a34bt cmd OPCODE [BYTE...]` and `a34bt dump [SECS]` are for a shell, if there is one.

## 5. Bluetooth for iOS (kernel 0970 + 0971, `a34bt=2`)

What changes with 0970: the phone gets its own address. The firmware's is MediaTek's default
(00:00:46:67:61:01, the same on every A34); a random static address is kept in `/a34/bt-address`
(`install-firmware.sh` makes it if the file is missing, else the kernel does at the next boot) and set
with MediaTek's command 0xFC1A after bring-up and after every HCI_Reset. The raw channel now takes
commands (0939 refused them, which is why `a34bt probe` said nothing in boot 0961-radio-a), and every
`a34bt` line goes to `/dev/console`.

What 0971 adds with `a34bt=2`: the controller handed to iOS's `bluetoothd` (kernel control
`com.apple.uart.sk.bluetooth`, the Skywalk channel, the registry's service `bluetooth` with
`local-mac-address`, `/dev/btwake`). bluetoothd first asks BlueTool to run its "boot" script, which
on an iPhone loads Broadcom firmware: BlueTool's job runs `a34bt-bluetool` instead, which answers.

With the root at `/mnt` and the CI artifact's `a34/`:

    sudo userland/a34bt/install-firmware.sh /mnt/vendor/firmware /mnt     # (once; also makes /a34/bt-address)
    sudo python3 -I tools/root-add-job.py --copy /mnt /a34 a34
    sudo python3 -I tools/root-add-job.py --wrap /mnt com.apple.BlueTool /a34/a34bt-bluetool
    sudo python3 -I tools/root-add-job.py /mnt com.a34.btprobe /a34 a34 a34bt scan 60

(`--unwrap /mnt com.apple.BlueTool` gives BlueTool back.) The scan job listens for 60 s: before
bluetoothd connects it scans on its own; after, it counts what bluetoothd's scans bring in. Keep the
AirPods Pro 2 in their case, lid open, next to the phone during the first minute.

Boot-args: as now, with **`a34bt=2`** instead of `a34bt=1`.

What to look for (`grep -a 'A34 BT\|a34bt' log`):

    A34 BT: address C?:..:..:..:..:.. made now (random static), kept in /a34/bt-address (0)   (first boot)
    A34 BT: address (at bring-up): C?:...  set
    A34 BT: br: bridge to bluetoothd up: waiting for bluetoothd
    A34 BT: the service "bluetooth" is in the registry, local-mac-address ...
    a34bt-bluetool[N]: asked { ... "script" => "boot" ... }
    A34 BT: br: com.apple.uart.sk.bluetooth connected by bluetoothd[N]
    A34 BT: br: getsockopt 0x40105412 ...: the channel's UUID
    A34 BT: channel: connected: bluetoothd's packets now go to the controller and back
    A34 BT: br> ... / br< ... / br= ...       (bluetoothd's commands, the answers; br= answered here)
    A34 BT: LE heard in 10 s: N reports from M devices ...; proximity pairing (AirPods/Beats): K, ... model bytes 14 20
    a34bt: Apple: ... AirPods/Beats proximity pairing: K adverts

Then Settings > Bluetooth: the switch should be on and show the phone as discoverable. If bluetoothd
keeps restarting (`exec bluetoothd` again and again), the last `br>` before each restart is the command it
gave up on: that is the next thing to answer.

## 6. bluetoothd past BlueTool (0972, `a34bt-btd`)

Boot 0980-btmetal-a: everything of 0971 was there, but bluetoothd never opened it. Its start
(bluetoothd 20H392, read in the binary): address and registry checks pass; then it has BlueTool boot
the chip -- `{kMsgId: RunBuiltinScript, kMsgArgs: {script: boot, btaddress}}` to `com.apple.BlueTool`,
synchronously (`+0x272034`). BlueTool's job was never started (no `exec` of it or of
`a34bt-bluetool`), the reply was an XPC error, which bluetoothd counts as "Init failed, still in high
power result=1", and it `_exit`ed (`+0x3a6128`) and was restarted: 15 times.

Now BlueTool is answered inside bluetoothd: its job runs `a34bt-btd`, which execs the real
bluetoothd with `liba34bt-btd.dylib` inserted (every message to `com.apple.BlueTool` answered there:
result 0) and with its stderr and os_log on the console, so the next wall, if any, is in the log.

    sudo python3 -I tools/root-add-job.py --copy /mnt /a34 a34
    sudo python3 -I tools/root-add-job.py --wrap /mnt com.apple.bluetoothd /a34/a34bt-btd

(`--unwrap /mnt com.apple.bluetoothd` undoes it. The BlueTool wrap of section 5 may stay. The
files `/a34/bluetoothd.quiet` (no os_log on the console) and `/a34/bluetoothd.nolib` (no library) are
switches.) Kernel: main with 0972 (LE counts every 10 s fixed; Apple advert types counted), `a34bt=2`.

In the log:

    a34bt-btd[N]: exec /usr/sbin/bluetoothd (os_log on stderr: yes, /a34/liba34bt-btd.dylib)
    a34bt-btd: bluetoothd[N]: liba34bt-btd loaded: BlueTool answered here, exits said
    ...Bluetooth Super Server Robot Destroyer...            (bluetoothd's own stderr)
    a34bt-btd: bluetoothd[N]: connects to com.apple.BlueTool ...
    a34bt-btd: bluetoothd[N]: com.apple.BlueTool asked { ... boot ... }: answered here
    HCI Transport is set to H4BC                            (its os_log)
    A34 BT: br: com.apple.uart.sk.bluetooth connected by bluetoothd[N]
    A34 BT: br> / br< / br= ...
    a34bt-btd: bluetoothd[N]: _exit(1) from bluetoothd+0x... (if it still restarts: where from)
    A34 BT: LE Apple types (type:count, 07 = proximity pairing): 07:.. 10:.. 12:..

## 7. bluetoothd's keychain (boot 0988-all-a)

bluetoothd passed BlueTool's place in 0988-all-a and then `_exit(1)` from `bluetoothd+0x90778 <-
+0x487edc <- +0x5993e4 <- +0x42a5d4 <- +0x42b174 <- +0x429084 <- main`: its stack's init
(`+0x599048`) reads its root keys (`readRootKeys`, `+0x594fac`) -- LE identity root, encryption root,
cloud keys, static random address -- from the system keychain (generic passwords of service
`MobileBluetooth`) and stores new ones when there are none. securityd cannot store an item on this
kernel (AppleKeyStore selector 10, wrap key: `SWEET AKS: securityd sel 10 ... -> 0xe00002c7`), so
"Failed to set new identity root!" and a restart. (The only other way past it is
`/product/boot-ios-diagnostics` in the device tree, which turns the whole system into iOS Diagnostics.)

`liba34bt-btd` now keeps bluetoothd's `MobileBluetooth` items itself, in
`/var/mobile/Library/Bluetooth/com.a34.keychain.plist` (not encrypted); every other keychain use is
unchanged. And it formats bluetoothd's os_log onto the console (`bt: ...`), since
`OS_ACTIVITY_DT_MODE` showed nothing. Only `/a34` changes (same kernel, same wraps):

    sudo python3 -I tools/root-add-job.py --copy /mnt /a34 a34

Expected: `a34bt-btd: bluetoothd[N]: keychain: copy Identity Root ...: 0 found`, `keychain: add ...: 0`,
then `bt: ...` lines of its start, `com.apple.BlueTool asked ... boot`, `bt: HCI Transport is set to
H4BC`, and the kernel's `A34 BT: br: com.apple.uart.sk.bluetooth connected by bluetoothd`.
