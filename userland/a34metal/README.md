# a34metal: a Metal device for the Mali-G68

The Metal plug-in for iOS 16.7.16 on the Galaxy A34: the bundle `Metal.framework` loads as its
GPU driver. **Milestone 1** of [`docs/metal-plan.md`](../../docs/metal-plan.md): Metal finds the
plug-in, loads it, registers its device, and the device says what it is. It makes no buffer,
texture, queue or pipeline yet (milestones 2 to 5). Nothing here has run on the phone so far.

Built by `.github/workflows/a34metal.yml` (branch `metal`, artifact **`a34metal`**), or on a Mac
with `userland/a34metal/build.sh`.

| file | what |
| --- | --- |
| `A34Metal.m`, `Info.plist` | `A34Metal.bundle`: class `A34MetalDevice`, a subclass of Metal's private `_MTLDevice` |
| `metal-probe.m` | `metal-probe`: asks Metal for its device and prints what it is; `PASS` / `FAIL` |
| `a34metal_iokit.h` | the IOKit calls used, for SDKs without IOKit headers |
| `hosttest.m` | build-time check of the plug-in against the build machine's Metal (not for the phone) |
| `build.sh` | builds both for arm64 iOS 16, signs ad hoc, checks, packages |

## How Metal loads it

Read off Metal 306.6.6 in our dyld cache (the plan's section 1 has the addresses):
`MTLCreateSystemDefaultDevice` looks up every `IOAcceleratorES` service, reads its properties
`MetalPluginName` and `MetalPluginClassName`, loads
`/System/Library/Extensions/<MetalPluginName>.bundle`, takes the class of that name (else the
principal class), requires a subclass of `_MTLDevice`, sends `-initWithAcceleratorPort:`, and
gives the result to `MTLAddDevice`, which asserts the protocols `MTLDevice` and `MTLDeviceSPI`
and has `_MTLDevice` fill its limits from the subclass's `-featureProfile` and
`-maxBufferLength`. No user client of `IOAcceleratorES` is opened by anything in that path;
the GPU is reached through `RedwoodMali`, as Mesa reaches it.

So the kernel's `IOAcceleratorES` (patch 0907, without properties today) has to name the
plug-in: `MetalPluginName` = `A34Metal`, `MetalPluginClassName` = `A34MetalDevice`, behind
boot-arg `a34metal` (the snippet is in the plan). Until a kernel has that,
`metal-probe --direct` loads the bundle itself and registers the device through Metal's own
`MTLAddDevice`, which runs the same assertions and limits.

## Who gets the device

Once the kernel names the plug-in, every process that asks Metal loads it: no injection is
needed, which is how apps get a device (`liba34gl` reaches backboardd only). A device that
cannot draw must not be given to programs that would then draw with it, so the plug-in
answers per program, and "no" unless:

| | |
| --- | --- |
| `A34METAL=1` / `A34METAL=0` in the environment | yes / no, whatever else says |
| the program is `metal-probe` or `a34metal-*` | yes |
| `/a34/A34Metal.clients` has a line with the program's name, or `*` | yes |
| boot-arg `a34metal=2` | yes, except backboardd |
| boot-arg `a34metal=3` | yes |

`A34METAL_PROFILE=N` or boot-arg `a34metal_profile=N` sets `-featureProfile` (0, 1, 4...9;
default 1), the number `_MTLDevice` derives the limits and GPU families from. The plug-in
writes to stderr as `a34metal[program pid]: ...`; a message of `MTLDevice`/`MTLDeviceSPI` that
is not implemented is answered with zero and named once.

## On the phone

With the root mounted at `/mnt` (TWRP, or the image on the host) and the artifact unpacked in
`a34metal/`:

```
tools/root-add-job.py --copy /mnt /System/Library/Extensions/A34Metal.bundle a34metal/A34Metal.bundle
tools/root-add-job.py /mnt works.kry.a34metal.probe /a34 a34metal/a34 metal-probe --direct --next
```

The job runs once at boot with its output in the kernel log (`tools/pull-log.sh`). Expected
with today's kernel: the registry lines (`IOAcceleratorES ... has no MetalPluginName`),
`MTLCreateSystemDefaultDevice() = 0x0`, the `direct:` lines, the device's class, name
(`Mali-G68 (A34Metal)` with `a34mali=1`), the GPU families `-featureProfile` 1 claims, and
`PASS`. With a kernel that names the plug-in (`a34metal=1`), plain `metal-probe` passes
without `--direct`. Exit status: 0 `PASS`, 1 no device, 2 another class's device, 3 a device
Metal would not take. The plug-in is new code in every process that asks Metal: leave
`a34metal` at 1 until milestone 4.

## Credits

The scheme (a direct `_MTLDevice` subclass, `MTLDeviceSPI` stated at run time, the methods the
registration needs, refusals named once, the device shown to chosen programs) was worked out
by **HackiOS** (OOSEX GANG): `hakios-metal/plugin/HakiosMetal.m` in
<https://github.com/oosexgang/hakios-xnu-kernel>, and `userland/src/redwoodmetal.m` on its
branch `mtk` (the Mali-G52 port), both under the MIT License. The code here is written for
this port; the facts about `targetDeviceInfo`, the feature profiles and the object that must
not be released are theirs and are marked in `A34Metal.m`.

```
MIT License

Copyright (c) 2026 OOSEX GANG
Copyright (c) 2025-2026 the HakiOS authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
