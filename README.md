# vulkan_remote

Run Vulkan on a machine that has no GPU, by forwarding the API to one that has.

The client is a real Vulkan ICD, so the stock Khronos loader drives it and
unmodified programs do not know a socket is involved. `vkcube` renders on
another machine's GPU through it, and its window appears on that machine's
compositor.

```
  client machine                                  server machine
  (no GPU needed)                                 (has the GPU)

  vkcube ──► Vulkan loader ──► client/icd  ──TCP──►  server ──► real ICD (RADV)
     └────► Wayland ──► wayland/proxy_client ──TCP──►  WaylandProxy ──► compositor
```

## Layout

| path | what it is |
|---|---|
| `common/wire.*` | framing, bounds-checked reader/writer |
| `common/marshal.*` | struct serialisation; arrays and handles, `pNext` not carried |
| `common/generate_remoting.py` | opcodes derived from `vk.xml`, plus a command-set digest |
| `client/` | the ICD: `icd.cpp` entry points, then one file per area |
| `server/` | `main.cpp`, `session.*` dispatch, `handlers_*.cpp` |
| `wayland/` | the protocol proxy and its own wire format |
| `tools/offscreen.cpp` | ordinary Vulkan triangle used as an offscreen regression test |
| `tools/texture_upload.cpp` | staging-uploaded checkerboard sampled and read back offscreen |
| `tools/compute.cpp` | deterministic storage-buffer compute regression |
| `tools/remoting_trace.py` | record, inspect, and exactly replay one headless wire session |
| `tests/run_matrix.py` | inventory-driven four-mode acceptance runner |
| `tools/probe.cpp` | links no Vulkan; times round trips |

Handlers register themselves by opcode at static initialisation, so adding a
command touches one file. They are compiled into the executable rather than a
static library, because a linker may drop an object nobody references and a
dropped registrar is indistinguishable from an unsupported command at runtime.

## Build and run

Both endpoints must be little-endian x86-64 and built from the same source
revision. In particular, use the repository-pinned `registry/vk.xml`: the
registry hash, wire schema, ABI identifier, and generated command-set digest are
checked during the handshake so incompatible peers fail before decoding calls.

### Windows application → Linux GPU quick start

In this mode the unmodified application and its CPU-side Vulkan logic run on
Windows. The remoting ICD forwards Vulkan calls to the Linux server, where the
real ICD owns the Vulkan objects and executes GPU work. A server-owned Wayland
window appears on the **Linux GPU machine**; pixels are not streamed back to the
Windows desktop.

Windows needs a native Visual Studio C++ environment, CMake, Ninja, Python, the
Vulkan SDK/loader, and OpenSSH. Linux needs CMake 3.21+, a C++17 compiler,
Python, Vulkan headers and loader development files, a working Vulkan ICD,
`pkg-config`, Wayland client/protocol development files, and access to an active
Wayland compositor.

Build and test the Linux server:

```sh
cmake -S . -B build
cmake --build build -j8
ctest --test-dir build --output-on-failure
```

Build the Windows client from a native Visual Studio command prompt:

```bat
cmake -S . -B build -G Ninja -Dvulkan_registry_xml=%CD%\registry\vk.xml
cmake --build build
```

On the Linux GPU machine, start the server from its graphical session. Check the
actual `WAYLAND_DISPLAY` instead of assuming its value:

```sh
XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=<wayland-display> \
  ./build/vulkan_remoting_server --validate
```

Keep the default `127.0.0.1:24680` binding. The protocol is unauthenticated; its
handshake establishes build compatibility, not peer identity. On Windows, first
make one interactive SSH connection to establish host-key trust and verify
key-based login, then leave this tunnel running in a separate terminal:

```bat
ssh user@<linux-gpu-host>
ssh -N -L 127.0.0.1:24680:127.0.0.1:24680 user@<linux-gpu-host>
```

Run `vkcube` from a normal interactive Windows desktop terminal:

```bat
python tools\remoting_run.py --no-tunnel ^
  --library build\vulkan_remoting_icd.dll ^
  127.0.0.1 -- C:\VulkanSDK\<version>\Bin\vkcube.exe --c 20
```

The helper writes an ICD manifest with an absolute DLL path, sets both
`VK_ICD_FILENAMES` and `VK_DRIVER_FILES`, and selects `127.0.0.1:24680`.
Windows OpenSSH lacks `ControlMaster`/`ControlPath`, so a separate tunnel is the
most predictable workflow and must be stopped manually afterward.

The helper can instead request the tunnel itself:

```bat
python tools\remoting_run.py user@<linux-gpu-host> -- ^
  C:\VulkanSDK\<version>\Bin\vkcube.exe --c 20
```

For manual loader setup, ensure the manifest's `library_path` is absolute and
set **both** loader variables; loader versions differ in which name they honor:

```bat
set VK_ICD_FILENAMES=C:\path\to\build\vulkan_remoting_icd.json
set VK_DRIVER_FILES=C:\path\to\build\vulkan_remoting_icd.json
set VK_REMOTING_HOST=127.0.0.1
set VK_REMOTING_PORT=24680
C:\VulkanSDK\<version>\Bin\vkcube.exe --c 20
```

The Windows Vulkan loader ignores environment-based driver selection for an
elevated process. A normal desktop terminal is preferred. If the helper is
already elevated, it temporarily registers its generated manifest under
`HKLM\SOFTWARE\Khronos\Vulkan\Drivers`, launches the child, and unregisters the
manifest on exit. A GUI process launched through Windows SSH still normally
lands in non-interactive session 0.

#### Launch from the Linux display host

`tools/run_windows_app.py` turns the preceding manual steps into one foreground
command run on the Linux machine where the window should appear:

```sh
python3 tools/run_windows_app.py -- \
  C:\\VulkanSDK\\<version>\\Bin\\vkcube.exe --c 20
```

It starts a loopback-only local server, opens an SSH **reverse** forward whose
`127.0.0.1` listener is on the Windows host, then creates a UUID-named scheduled
task with `/it /rl limited`. The task runs in the logged-in Windows user's
interactive desktop with normal integrity; plain Windows SSH runs in session 0
and is not suitable for `vkcube`'s Win32 window setup. The client HWND remains an
opaque token and the Linux server creates the visible Wayland window.

Defaults target `water-banana`, user `water`, deployment `C:\vulkan_remote`, and
`/mnt/worktrees/windows-debug-scripts/ssh/wsh`; each has a command-line override
shown by `--help`. The launcher prints concise startup phases, forwards newly
available application stdout/stderr to the local terminal, emits a 15-second
heartbeat while quiet, returns the Windows application's exit code, and on
completion, an explicitly requested timeout, or Ctrl-C removes only its UUID
task/artifacts and terminates only its owned Windows
PID tree, server, and SSH process groups. PID cleanup first verifies that the
command line contains the UUID run directory. Use `--keep-windows-artifacts` to
retain the remote log and status, or `--dry-run` to inspect commands without
contacting a host. Application runtime is unlimited by default; pass, for
example, `--timeout 30` when a bounded diagnostic run is wanted.

Windows→Linux does **not** require `--wayland`, the Wayland proxy, or TCP port
`24681`: a remote Win32 surface is translated to a server-owned Wayland window.
The `--wayland`/`24681` path is only for Linux→Linux when preserving the Linux
application's actual Wayland surface identity. Linux→Windows and
Windows→Windows likewise use server-owned native windows.

On a Linux server-owned window, requested FIFO presentation is attempted as
MAILBOX because RADV/Sway can otherwise block after resize. The server logs the
substitution and falls back to FIFO when MAILBOX is unavailable.

### Troubleshooting

| Symptom | Check |
|---|---|
| Connection refused | The server says it is listening on `127.0.0.1:24680` and the SSH tunnel terminal remains open. |
| SSH exits immediately | Establish host-key trust interactively and install a key; helpers use `BatchMode=yes`. |
| Handshake rejected | Rebuild both endpoints from the same commit and pinned `registry/vk.xml`. |
| Local/shipping GPU is used | Use an absolute manifest path and set both loader variables; avoid an elevated terminal unless using the helper's temporary registration. |
| No Linux window | Start the server with the active compositor's `XDG_RUNTIME_DIR` and `WAYLAND_DISPLAY`; verify the real ICD supports Wayland surfaces. |
| Tunnel remains after the app | Windows OpenSSH cannot use multiplexed close; stop the separate `ssh -N` process, or use `run_windows_app.py`, which owns its reverse tunnel. |
| Reverse forward fails | Check SSH server policy and whether the selected Windows loopback port is already occupied; override it with `--windows-port`. |
| Remote command exits without status | Re-run with `--keep-windows-artifacts` and inspect the UUID run directory under `C:\vulkan_remote\runs`. |

Cost, measured on loopback with `tools/offscreen` (a full instance/device setup,
render and readback): **19-22 ms direct against 25-27 ms tunnelled**, about
+5.7 ms. Over a real link that fixed SSH cost is dwarfed by the protocol's own
roughly two synchronous round trips per frame; the ceiling is protocol-bound,
not encryption-bound.

## Record and replay deterministic wire traffic

`tools/remoting_trace.py` records one complete, headless ICD connection through
a loopback proxy and can inspect or replay it later:

```sh
python3 tools/remoting_trace.py record run.vkrt \
  --listen-port 24682 --upstream-port 24680 \
  --protocol-file build/remoting_commands.inl --metadata sample=compute
python3 tools/remoting_trace.py inspect run.vkrt
python3 tools/remoting_trace.py replay run.vkrt --port 24680 \
  --protocol-file build/remoting_commands.inl
```

Point `VK_REMOTING_PORT` at the recorder's listening port while recording. The
container checks its schema, registry, ABI, record CRCs, and whole-file digest
before replay contacts a server. Replay uses a fresh server session and compares
responses exactly. It intentionally supports deterministic offscreen, texture,
and compute samples only. The default `outputs` verification checks protocol status and
mapped readback data while tolerating implementation-dependent query bytes; use
`--verify exact` when every response byte is expected to be stable. Traces retain
server-derived handles and memory/queue choices and are not portable across unrelated
GPU/driver configurations. Trace
payloads include mapped-memory contents and may therefore contain sensitive
application data; generated `.vkrt` files are test artifacts, not source files.

`tests/run_matrix.py print-example` emits a host-neutral inventory template for
the four live OS pairings. Commands are explicit argv arrays and hostnames are
kept outside the repository.

## What works

- `tools/offscreen` renders a triangle and `tools/texture_upload` uploads and
  samples a single-mip checkerboard; both read back deterministic images whose
  PPM output is **byte-identical** on the system driver and through this one.
  `tests/test_samples.py` verifies both paths with a fresh ephemeral server port.
- `vkcube` and `vkcubepp` each complete 20 frames through the server-owned
  Wayland-window path. On 2026-09-23 they completed in 0.5 s and exited cleanly.
- The Windows client works against the Linux server: a matched static DLL
  (`a62b3b0d...`) made `vulkaninfo.exe --summary` enumerate all three server GPUs,
  then `vkcube.exe --c 20`, `vkcubepp.exe --c 20`, and a repeated `vkcube` run
  completed in 0.84 s, 0.93 s, and 0.91 s respectively. The run used a headless
  Weston GL compositor; pixman is not a valid WSI control here because direct
  `vkcube` also fails against it.
- `VkSurfaceFormatKHR` is encoded field-by-field. Raw-copying this enum pair across
  Win32 and Linux corrupted the selected format (`VK_FORMAT_UNDEFINED` or random
  enum values), which made cube presentation hang or crash. Because that changes
  the wire schema, `common/generate_remoting.py` includes a schema revision in the
  handshake digest so mixed old/new peers are rejected before decoding messages.
- CTS results drift as the suite grows and commands get implemented, so the
  pass/fail count is not pinned here. `tests/cts_baseline.txt` is the known-
  passing set (`dEQP-VK.api.smoke.*` and `dEQP-VK.api.info.*` so far); run
  `python3 tests/run_cts.py --caselist '<pattern>'` and it reports only the
  delta against that baseline - regressions fail the run, new passes and the
  (expected, still large) set of unimplemented commands do not.
- Server-side validation is clean against the real driver.

## How the hard parts are solved

**Mapped memory.** `vkMapMemory` must return a pointer the caller can
dereference, which a socket cannot deliver. The client hands back a shadow
allocation and moves the bytes at the points where Vulkan says they become
visible: uploaded before a submit, downloaded at map and invalidate. Two
opcodes outside the Vulkan command set exist for that, and they are in the
handshake digest because both peers must agree on them. Coherent memory is
therefore not coherent in the strict sense — writes land at submit.

**Recording.** Every `vkCmd*` is sent without waiting for a reply, so a
40-call command buffer costs one round trip instead of forty. The server
counts errors from those calls and reports the total at the next call that
does wait.

**Presentation.** A remote GPU can only present to a window it owns, and
Wayland object ids are meaningless outside the connection that made them — so
no separate process can name the application's surface. `wayland/` replays the
application's protocol onto a connection owned by the server process itself,
which can then create a surface from it. Unlike waypipe it forwards **no
buffers**, because rendering already happens on that side.
`vkCreateWaylandSurfaceKHR` sends only `wl_proxy_get_id(surface)`, and the
server resolves it against the replayed object.

## What it measures, and what that means

| link | round trip | synchronous calls per 16.7 ms frame |
|---|---|---|
| loopback | 15.8 us mean | ~1059 |
| arch2.dorm → water.n2n | 7,187 us mean | **2** |
| higher-latency remote link | 39,205 us mean | **0** |

`vkcube` over the real link runs at **under about 7 fps**.

Two synchronous calls per frame is the result the project was built to get. A
frame of a real game is thousands of Vulkan calls, and the cost is **per call,
not per byte** — so neither compressing the payload nor a faster link changes
the number. Only removing round trips does, and the round trips are the API.

This is why remote rendering is done by shipping finished frames instead:
Sunshine/Moonlight, or `waypipe` for a Wayland application. The driver here is
a way to measure that conclusion rather than assume it.

## TODO / known limitations

- Add field-wise, schema-versioned encodings for the remaining raw-copied Vulkan
  structs before claiming cross-ABI support beyond the tested Win64/Linux-x86-64
  pair.
- Add client-side diagnostics that name a rejected or unsupported opcode; today
  the useful diagnostic is primarily in the server log.
- Add further command families only when a concrete sample or application requires them;
  transfer and compute have deterministic regression coverage.
- `pNext` chains are dropped by the marshaller.
- Shadow mappings are per-range; two mappings of overlapping memory are not
  reconciled.
- Roughly a third of cross-machine `vkcube` runs drop with
  `event on unknown object N` during registry binding in the proxy. Not
  isolated.
- Peer-supplied counts size allocations in some handlers before validation.
  One such site crashed the server under CTS and was fixed; the pattern
  deserves a sweep.
- No dmabuf, data devices or subsurfaces in the proxy. No `VK_KHR_display`,
  no X11 surfaces.
