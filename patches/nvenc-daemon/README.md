# nvenc-daemon — the SCM_RIGHTS transport, as a genuinely separate process

A standalone daemon for encoding a plain (non-Venus) dma-buf via NVENC — the transport
`VCMD_ENCODE_RESOURCE` (see [`../virglrenderer/README.md`](../virglrenderer/README.md)) fundamentally
can't reach, since it needs a Venus resource id and a real Surface-sourced capture buffer (the path
any screen-recording app, `scrcpy` included, actually uses) was never allocated through this
project's own Venus path in the first place. See [`../codec2/README.md`](../codec2/README.md) for
the guest side that connects to this.

## Why this is its own process, not a thread inside `virgl_test_server`

It used to be exactly that — a `pthread_create()`-started listener thread inside `virgl_test_server`
itself (see `patches/virglrenderer/`'s own git history for the removed `nvenc_scm_listener.c`).
**Confirmed as a real regression, unrelated to encoding at all**: `virgl_test_server` `fork()`s
internally, once per Venus context, to spawn its own `virgl_render_server` child process. A
long-lived background thread in that same process is a textbook hazard for exactly that reason — if
the thread happens to be holding any libc-internal lock (malloc's arena lock, the dynamic linker's
own lock from a `dlopen()`) at the instant another part of the process calls `fork()`, the child
inherits that lock already held, forever, since only the forking thread survives into the fork.
Any later call in the child needing that same lock — essentially any `malloc()` — deadlocks or
misbehaves silently.

Found on real hardware, not in a code review: ordinary rendering (nothing to do with this daemon at
all) went from occasionally crashing to consistently rendering a solid white screen the same day
this thread was added — confirmed by a clean A/B test, swapping between this project's own modified
`virgl_test_server` and the untouched, pristine binary with zero other changes. Moving the listener
out to its own process here removed the hazard entirely (no shared address space, no shared locks) —
`virgl_test_server` itself is back to completely unmodified.

This also happens to be the right structural choice for a separate reason: it makes this project's
own encode path shaped exactly like redroid-hwenc's own VA-API daemon (a real, working precedent for
unifying the two projects' approach eventually — see the main `README.md`'s discussion of a possible
shared, vendor-agnostic Codec2 component with the vendor-specific work living entirely on the host).

## Build

Plain `gcc`, no big build system — matches this project's own `tests/tier7_*.c` spikes:

```sh
gcc -O2 -Wall -o nvenc_scm_daemon nvenc_scm_daemon.c vtest_gpu_encode.c -lpthread -ldl
```

`vtest_gpu_encode.c`/`.h` here are the exact same files as
[`../virglrenderer/vtest_gpu_encode.c`](../virglrenderer/vtest_gpu_encode.c) — kept in sync manually
(vendored twice rather than shared via a build-system dependency, matching this project's existing
convention of vendoring whole files rather than introducing cross-directory build coupling). Needs
FFmpeg's `nv-codec-headers` installed to `/usr/local/include`, same as the rest of Tier 7.

## Running

```sh
./nvenc_scm_daemon /path/to/venus-sock/nvenc-scm.sock
```

The socket path should be the same directory bind-mounted into the redroid container as `/dev/venus`
(the same directory `venus.sock` itself lives in) — the guest side connects to the fixed path
`/dev/venus/nvenc-scm.sock`, which resolves to whatever real path this daemon was given on the host.
Run alongside a real Venus-capable `virgl_test_server` (pristine or otherwise) for actual rendering —
this daemon only handles the encode side, nothing about rendering.
