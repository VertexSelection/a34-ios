# a34media: mediaserverd without audio hardware

mediaserverd is iOS's audio server, and also where MediaToolbox's player, video queue, routing,
system controller and decompression servers live. On the A34 it aborts every time a program starts
playing a movie (the keyboard's "slide to type" introduction the first time the keyboard opens,
Setup's "hello"). launchd restarts it, and whatever was waiting on it hangs for about 10 s.
This directory has HackiOS's two mediaserverd pieces, ported:

| file | what | from (HackiOS, branch `mtk`) |
| --- | --- | --- |
| `media.c` | `liba34media.dylib`, inside mediaserverd: gives VideoToolbox the display's alignments (the fix, below); logs every Objective-C exception thrown, with the throw site, and the fatal one; skips CMCapture's portrait-filter prewarm; one line on the audio set-up | `userland/src/inject_media.c` (`libredwood-media.dylib`), without the flashlight, the camera and Camera.app's probes |
| `audio_plugin.c`, `Info.plist` | `RedwoodAudio.driver`, a CoreAudio HAL plug-in (AudioServerPlugIn) with devices `Speaker`, `Hawking`, `Actuator`, `Codec`, so that VirtualAudio builds the speaker route. Output is counted and dropped, inputs are silent | `userland/src/audio_plugin.c`, `audio_plugin.plist` (there the samples go to `/dev/redwood-audio`, `/dev/redwood-haptic`, `/dev/redwood-mic`) |
| `../a34gl/inject.c` | `liba34gl-inject.dylib` opens `/a34/liba34media.dylib` in mediaserverd (`A34_LOAD`) | |
| `build.sh`, `../../.github/workflows/a34media.yml` | the build (macOS, Xcode; no Mesa). Artifact **`a34media`** | |

## The crash

Kernel log, 13 times in 12 boots (`build/ios-boot6.log` to `ios-boot49.log`), always the same, e.g. boot 49:

```
exec mediaparserd
exec com.apple.accessibility.mediaaccessibilityd
CRASH mediaserverd[28] sig 6 code 0x0 sub 0x0 slide 0x25ac8000 mh 0x104038000
CRASH mediaserverd bt 0x1b37244b8 0x1f75a6f54 0x1f7597a4c 0x1a5b9aafc 0x1f75a6424 0x1f75a63d4 0x1b3660794 ...
```

Unslid, and named by `ipsw dyld a2s` (cache 8f970452):

```
0x18dc5c4b8 abort+124
0x1d1adef54 (libc++abi abort_message)
0x1d1acfa4c demangling_terminate_handler+300   abort_message("terminating due to %s exception of type %s"): no what()
0x1800d2afc _objc_terminate+124                the @catch (id e) branch: uncaught_handler(e), then old_terminate
0x1d1ade424 std::__terminate
0x1d1ade3d4 std::terminate
0x18db98794 _dispatch_client_callout+36
0x18db736fc _dispatch_lane_serial_drain+600
0x18db741e4 _dispatch_lane_invoke+432
0x18db7cc68 _dispatch_root_queue_drain+328
```

What this establishes:

- An **Objective-C exception** (an NSException) that nobody caught. It is not a C++ exception: in
  `_objc_terminate` the return address 0x1800d2afc follows the call to `old_terminate` in the branch
  taken when the landing-pad selector is 2, which is the one that first calls the uncaught-exception
  handler with the object. The other branch returns to 0x1800d2b0c.
- It was thrown in a **block on a serial dispatch queue**, on a worker thread. The thrower is not in
  the backtrace and can't be: the unwinder has already gone back to `_dispatch_client_callout`, whose
  landing pad calls `std::terminate`. A deeper kernel backtrace only adds `_dispatch_worker_thread2`,
  `_pthread_wqthread` and `start_wqthread`. The crashing thread's FAR is libc++abi's `__crash_info`,
  where `abort_message` wrote its text.
- The trigger is **a program starting media playback**, not the daemon starting. Every crash comes
  right after `mediaparserd` and `mediaaccessibilityd` are started, which is what an AVPlayer item
  does. Boot 6 crashed the same way with no keyboard.

What it is **not**: HackiOS's prewarm crash. There, `+[BWCoreImageFilterRenderer
prewarmPortraitFilterVersion:semanticStyleFilters:]` (called from `FigPreloadShadersInternal`) makes an
OpenGL ES context with no driver behind it, and the daemon jumps to 0 (SIGSEGV) at every start. Our
root image runs ios-sweet's `sweet-root-tweaks.py`, which sets MediaToolbox's
`SubServerConfiguration.plist` `StartRecorderServer` to false (checked in `build/ios16-root.img`), so
CMCapture's recorder server never starts here. None of the 13 crashes is a SIGSEGV.

**The thrower** (boot 54, `build/ios-boot54.log`, logged by liba34media; cache slide 0x2bc68000):

```
throw 1 on queue "com.apple.coremedia.player.item.P/AV": NSInvalidArgumentException:
    -[__NSCFNumber objectForKey:]: unrecognized selector sent to instance 0xb504e0ce1c7af70e
#4  0x186e688fc CoreFoundation  _CF_forwarding_prep_0
#5  0x194f3b48c VideoToolbox    FigGetAlignmentForIOSurfaceOutputToDisplayService+0x7c
#6  0x1951cd9d8 MediaToolbox    FPSupport_CopyDestinationPixelBufferAttributesWithIOSurfaceSupport+0x70
... MediaToolbox (the player item), _dispatch_client_callout, _dispatch_lane_serial_drain
```

mediaserverd's player server lays out the frames of a movie that goes to the display.
`FigGetAlignmentForIOSurfaceOutputToDisplayService` takes the display service (`getH3clcdService`:
AppleCLCD) and reads `IORegistryEntrySearchCFProperty(display, "IOService",
"DisplayPipePlaneBaseAlignment")` as a dictionary, without checking: `CFDictionaryGetValue(it,
"PlaneBaseAlignmentLinear")`. Then it does the same with `"DisplayPipeStrideRequirements"` /
`"StrideLinearHorizontal"`. Without the property it uses 64. Our kernel (ios-sweet 0046, kept in 0061
and our 0907) publishes `setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32)`, a **number**, and
`CFDictionaryGetValue` on a CFNumber is `-[__NSCFNumber objectForKey:]`. QuartzCore reads the same key
the same way (`CA::WindowServer::AppleDisplay`), but asks for `CFDictionaryGetTypeID()` and so passes
the number over. That confirms the format. It is the same trigger (a movie after `mediaparserd` and
`mediaaccessibilityd`) and the same stack shape as the 13 earlier crashes.

The fix in liba34media: the function keeps both answers in statics of its own
(`_planeBaseRequirement.checked/.requirement`, `_strideRequirement.checked/.requirement`, VideoToolbox
`__DATA`). liba34media fills them in before anybody asks, with the kernel's number (64, the function's
own default too), after checking nine instruction words of the function. It leaves them alone when
AppleCLCD's property is a dictionary, so a kernel that publishes the right form wins. It does this in
every program it is loaded into (`A34_LOAD`). Other programs that send video to the display themselves
(an application's in-process player, assetsd: boot 46 has an assetsd abort with the same stack) need
either liba34media too or the kernel fixed. The kernel change is, in `AppleCLCD::start`
(`iokit/Kernel/arm/SweetFramebuffer.cpp`):

```cpp
	OSDictionary *alignment = OSDictionary::withCapacity(1);
	OSNumber *linear = OSNumber::withNumber(64ULL, 32);
	if (alignment != NULL && linear != NULL) {
		alignment->setObject("PlaneBaseAlignmentLinear", linear);
		setProperty("DisplayPipePlaneBaseAlignment", alignment);    /* was: a number (VideoToolbox reads a dictionary) */
	}
	OSSafeReleaseNULL(linear);
	OSSafeReleaseNULL(alignment);
```

**The audio side.** The same script moves `/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin` to
`HAL.disabled` (checked in the image), because without any audio device VirtualAudio faults at start
(ios-topaz's notes: `ApplyBeamFormerPreference` with no default virtual device). So mediaserverd ran
with no virtual audio device and no route at all. HackiOS ran the same iPhone X (D22) identity with
VirtualAudio and `RedwoodAudio.driver` under it, and that is what this restores. Boot 54 shows the
speaker route built and sounds played through it, and the keyboard no longer freezes.

**Diagnostics.** `liba34media` logs every throw in mediaserverd: `a34media: mediaserverd[PID]: throw
N on queue "LABEL" ...` with name, reason and the throw-site stack, then `UNCAUGHT exception (throw N)`
before an abort. Independently of that, `sweetscan=backboardd,SpringBoard,mediaserverd` makes the
kernel print mediaserverd's heap strings on an abort (ios-sweet 0051/0053).

## How it gets into mediaserverd

The kernel inserts one library into the programs `a34inject=` names (patch 0914). That library is
`liba34gl-inject.dylib`, and it now opens the libraries in `A34_LOAD` (paths separated by ':', from the
environment or `/a34/NAME.env`) before anything of its own. For mediaserverd the default is
`/a34/liba34media.dylib`. So the only change is the name on the boot-arg:

    a34inject=/a34/liba34gl-inject.dylib,backboardd,SpringBoard,mediaserverd

`A34_LOAD=` (empty) in `/a34/mediaserverd.env` turns it off without a reboot-arg change. In
mediaserverd liba34gl-inject is otherwise as in any program that is not the window server: Mesa only
at the first EAGLContext (`A34GL_MODE=off` in `/a34/mediaserverd.env` leaves even that out).

Without the new liba34gl-inject, the kernel can insert both libraries, because everything before the
first comma becomes `DYLD_INSERT_LIBRARIES` and dyld takes a ':' list. In a program that is not
mediaserverd, liba34media only fills in VideoToolbox's display alignments:

    a34inject=/a34/liba34gl-inject.dylib:/a34/liba34media.dylib,backboardd,SpringBoard,mediaserverd

`RedwoodAudio.driver` needs no injection. CoreAudio's HAL in mediaserverd loads every plug-in in
`/System/Library/Audio/Plug-Ins/HAL` and `/Library/Audio/Plug-Ins/HAL` (both paths are in the
cache's CoreAudio).

## Installing

[INSTALL.md](INSTALL.md), shipped in the artifact as well.

## What the log says

- `a34gl-inject: mediaserverd[PID]: A34_LOAD: /a34/liba34media.dylib loaded`
- `a34media: mediaserverd[PID]: loaded (cache slide 0x..., mediaserverd at 0x...): ...`
- `a34media: mediaserverd[PID]: display alignment: VideoToolbox's DisplayPipePlaneBaseAlignment set to 64 (AppleCLCD's is a number, where it reads a dictionary)`,
  and the same for `DisplayPipeStrideRequirements` (`not there`)
- `a34media: mediaserverd[PID]: RedwoodAudio.driver there, VirtualAudio.plugin there: the speaker route can be built`
- `a34media: RedwoodAudio[PID]: with the HAL: devices Speaker, Hawking, Actuator, Codec ...`
- `a34media: RedwoodAudio[PID]: the speaker's volume is 1.000 (0.0 dB)` (VirtualAudio took the speaker)
- with a sound (a keyboard click, the lock sound): `the speaker plays (into nowhere)` ... `the speaker stops after N s, M s of it not silence`
- `asked of object N (NAME) what it does not have: 'xxxx'`: properties a device lacks, the first 64.
  If VirtualAudio does not take a device, these say what it was looking for. `'cust'`
  (`kAudioObjectPropertyCustomPropertyInfoList`), `'clok'`, `'cstb'`, `'taps'`, `'accs'`, `'auto'` and
  the like are asked of every object and are optional; HackiOS answers none of them either.
- `throw N on queue ...` / `UNCAUGHT ...`: see above. Success is no `CRASH mediaserverd` line after
  the keyboard is first opened, no `UNCAUGHT`, and `the speaker plays` while the introduction runs.

Off: boot-arg `a34media=0` (liba34media says so and does nothing). Taking out `RedwoodAudio.driver`
and moving `VirtualAudio.plugin` back to `HAL.disabled` returns to the old state.

## Kernel crash lines

The kernel's backtrace (`sweet_crash_log`, ios-sweet 0043) stops at 10 frames. That is enough here (see
above). Where more is wanted, the change in that patch would be `i < 10` to `i < 32` in the
frame-pointer walk, with `line[200]` raised to 400 bytes or the line split every 10 frames. It is
not made here.

## Credits and licence

- **HackiOS** (OOSEX GANG, <https://github.com/oosexgang/hakios-xnu-kernel>, branch `mtk`):
  `inject_media.c` (the mediaserverd library and its prewarm skip) and `audio_plugin.c` /
  `audio_plugin.plist` (RedwoodAudio.driver: the four devices, their UIDs, streams, controls, clock).
  HackiOS's `userland/src/` is MIT, Copyright (c) 2025-2026 the HakiOS authors. ios-topaz
  (<https://github.com/MakrSas/ios-topaz>) carries the same `audio_plugin.c` and recorded the
  VirtualAudio fault without a device.
- Our changes are under the same licence.

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
