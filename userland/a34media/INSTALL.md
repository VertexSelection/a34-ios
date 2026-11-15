# a34media: installing

The artifact is laid out by where each file goes on the iOS root:

| artifact | on the root | what |
| --- | --- | --- |
| `a34/liba34media.dylib` | `/a34/liba34media.dylib` | inside mediaserverd (`media.c`) |
| `a34/liba34gl-inject.dylib` | `/a34/liba34gl-inject.dylib` | the kernel's `a34inject` library, now with `A34_LOAD` (replaces a34gl's; otherwise the same) |
| `System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver/` | the same path | the HAL's null devices (`audio_plugin.c`, `Info.plist`) |

Every Mach-O is ad-hoc signed and named by its absolute path. Do not change a load command afterwards.

With the root mounted at `/mnt`, as root on the PC, from the repository's top directory, with the
artifact unpacked into `a34media/`:

    python3 -I tools/root-add-job.py --copy /mnt /a34 a34media/a34
    python3 -I tools/root-add-job.py --copy /mnt /System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver \
        a34media/System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver

and put VirtualAudio back where the HAL and MediaExperience look for it (ios-sweet's
`sweet-root-tweaks.py` moved it out because, with no device at all, it faults at start):

    mkdir -p /mnt/Library/Audio/Plug-Ins/HAL
    mv /mnt/Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin /mnt/Library/Audio/Plug-Ins/HAL/

Boot-args: add `mediaserverd` to `a34inject`, and (diagnostics, optional) let the kernel print
mediaserverd's heap strings if it still aborts:

    a34inject=/a34/liba34gl-inject.dylib,backboardd,SpringBoard,mediaserverd
    sweetscan=backboardd,SpringBoard,mediaserverd

Undo: `a34inject` without `mediaserverd`, or boot-arg `a34media=0`; for the audio, move
`VirtualAudio.plugin` back to `HAL.disabled` and delete `RedwoodAudio.driver`. If mediaserverd
crash-loops with SIGSEGV in VirtualAudio, VirtualAudio did not take the devices of RedwoodAudio.driver.
Check the `a34media: RedwoodAudio[...]` lines.
