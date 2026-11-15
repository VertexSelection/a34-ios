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
