# a34gl: installing

`a34/` is everything that goes into **`/a34`** on the iOS root (the userdata partition), as it
is: every library is named `/a34/<file>` (dyld refuses `@rpath` and other `@` paths for our
binaries), nothing has an `LC_RPATH`, everything is ad-hoc signed. Do not change a load command
afterwards (`install_name_tool`, `macho-abs-paths.py`): the signature breaks and launchd panics
("unexpected exec of non-platform binary").

| file | what |
| --- | --- |
| `libEGL.1.dylib`, `libGLESv2.2.dylib`, `libgallium-26.3.0.dylib`, `libdrm.2.dylib` | Mesa 26.3 panfrost + redwood-drm (`userland/mesa`), with the IOSurface import |
| `liba34gl.dylib` | the EAGLContext behind CoreAnimation, on Mesa (`eagl.c`, `gl_egl.c`) |
| `liba34gl-inject.dylib` | the loader inserted into backboardd (`inject.c`) |
| `a34gl-bbd` | backboardd's stand-in in its launchd job: sets `DYLD_INSERT_LIBRARIES`, execs the real one (`bbd.c`) |
| `a34gl-surftest` | test of the IOSurface path without CoreAnimation (`surftest.c`) |
| `mali-tri` | Mesa's own test (one triangle in a pbuffer) |
| `backboardd.env.example` | environment for backboardd (copy to `/a34/backboardd.env`) |

## 1. Files, and the tests first

With the root mounted at `/mnt` (as root, on the PC; `tools/root-add-job.py` from the repository):

    python3 -I tools/root-add-job.py --copy /mnt /a34 a34

Then run the tests as one-shot launchd jobs (output on the console, `tools/pull-log.sh`):

    python3 -I tools/root-add-job.py /mnt com.a34.malitri /a34 a34 mali-tri
    python3 -I tools/root-add-job.py /mnt com.a34.surftest /a34 a34 a34gl-surftest

`a34gl-surftest` must print `PASS` (exit 0) before CoreAnimation is tried: it needs the
kernel's RedwoodMali selector `0x7e` (IOSurface -> GEM handle). Exit 3 means the surface could
not be imported (kernel without `0x7e`, or a surface the GPU cannot use); 1 wrong pixels (cache
maintenance of surface memory?); 2 no EGL/GLES at all.

## 2. backboardd

Either the kernel (patch 0914) inserts the loader -- boot-arg
`a34inject=/a34/liba34gl-inject.dylib,backboardd`, nothing else to do; environment for
backboardd then goes into `/a34/backboardd.env` -- or the launchd job runs a wrapper:

    python3 -I tools/root-add-job.py --wrap /mnt com.apple.backboardd /a34/a34gl-bbd

The first makes backboardd's job (`System/Library/xpc/launchd.plist`) run `/a34/a34gl-bbd`, with
the real program in `A34GL_REAL`; the wrapper adds `/a34/liba34gl-inject.dylib` to
`DYLD_INSERT_LIBRARIES` and execs `/usr/libexec/backboardd` (same pid, same Mach services).
launchd itself drops `DYLD_*` variables from a job, which is why there is a wrapper.

**The kernel must let dyld honour `DYLD_INSERT_LIBRARIES`**: dyld asks the MAC policy named
`AMFI` (`__mac_syscall("AMFI", 0x5a, ...)`, `amfi_check_dyld_policy_self`) and, without an
answer, ignores every `DYLD_*` variable -- the same cause as "security policy does not allow @
path expansion". See `userland/a34gl/README.md`, "Injection".

Off again: boot-arg `a34gl=0`, or `A34GL_OFF=1` in `/a34/backboardd.env`, or

    python3 -I tools/root-add-job.py --unwrap /mnt com.apple.backboardd

The log (stderr: the kernel copies backboardd's into its own log as `|backboardd| ...`; or the
file `A34GL_LOG` names) says what was loaded, which IOSurfaces became texture memory, and a
line for the first 40 frames and every 600th after. Each message is rate-limited (32 times,
then once in 10 s).
