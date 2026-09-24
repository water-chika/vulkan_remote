# vulkan_remoting

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

```sh
cmake -B build && cmake --build build -j8
ctest --test-dir build --output-on-failure

./build/vulkan_remoting_server --validate --wayland          # GPU machine
VK_DRIVER_FILES=$PWD/build/vulkan_remoting_icd.json \
VK_REMOTING_HOST=<gpu-machine> vkcube                        # other machine
```

### Windows builds and cross-platform modes

Both the ICD and GPU-side server build natively on Windows. Configure every
endpoint from the repository-pinned `registry/vk.xml`; its hash participates in
the command digest and handshake, so independently installed SDK registries
cannot silently assign different opcodes. The supported MVP wire ABI is
little-endian x86-64 (Win64 and Linux).

The four live modes are Linux→Linux, Windows→Linux, Linux→Windows, and
Windows→Windows. The application's surface opcode describes the **client** WSI,
while the server translates it to its own native surface: Wayland on Linux or an
owned HWND on Windows. Linux→Linux can additionally use the Wayland protocol
proxy, which preserves the application's actual surface identity; the other
modes use a server-owned native window.

One MVP limitation is explicit rather than silent: on a Linux server-owned
window, a requested FIFO swapchain is attempted as MAILBOX because RADV/Sway
otherwise blocks in the first post-resize `vkQueuePresentKHR`; the server logs
the substitution and falls back to FIFO if MAILBOX is unavailable. Proxy-backed
Linux surfaces and Windows surfaces preserve the requested presentation mode.

On Windows, build with a native Visual Studio environment and CMake, then point
both loader variables at the generated manifest:

```bat
cmake -S . -B build -G Ninja -Dvulkan_registry_xml=%CD%\registry\vk.xml
cmake --build build
set VK_ICD_FILENAMES=%CD%\build\vulkan_remoting_icd.json
set VK_DRIVER_FILES=%CD%\build\vulkan_remoting_icd.json
set VK_REMOTING_HOST=127.0.0.1
```

The loader ignores these variables for elevated processes. Run presentation in
a normal interactive desktop session; SSH-launched GUI processes normally land
in session 0. `tools/remoting_run.py` checks this and prepares an absolute-path
manifest. Keep the unauthenticated server on loopback and use an SSH tunnel for
cross-machine runs.

## Keeping the port off the network

The server binds `127.0.0.1` by default. This protocol has no authentication
- the handshake compares a command-set digest, which proves the peer was
built from the same generated table and nothing about who it is - and every
connection it accepts gets a detached thread with a path to the GPU. On
`0.0.0.0` that was offered to the whole LAN.

Remote access goes through an SSH tunnel instead, so authentication,
encryption, integrity and host verification all come from ssh and this
project carries no crypto of its own:

```sh
./build/vulkan_remoting_server --validate --wayland        # GPU machine, loopback only
python3 tools/remoting_run.py user@gpu-machine -- vkcube   # client machine, one command
```

`remoting_run.py` is not specific to vkcube, or to any program: the client is
an ICD, so anything that loads the Vulkan loader is remoted without knowing
it. Whatever follows `--` is launched with the environment already right - it
generates the ICD manifest with an absolute `library_path`, sets both
`VK_ICD_FILENAMES` and `VK_DRIVER_FILES`, opens the tunnel, and closes it
afterwards. On Windows it also refuses to run elevated, since the loader
would silently ignore the driver. Those are the three ways this setup
actually fails in practice, so the script removes them rather than
documenting them.

The steps it automates, if you would rather run them yourself:

```sh
./build/vulkan_remoting_server --validate --wayland        # GPU machine, loopback only
python3 tools/remoting_tunnel.py open user@gpu-machine     # client machine
VK_DRIVER_FILES=$PWD/build/vulkan_remoting_icd.json \
VK_REMOTING_HOST=127.0.0.1 vkcube
python3 tools/remoting_tunnel.py close user@gpu-machine
```

Both ends of the forward are pinned to loopback, key-based auth is required
(`BatchMode=yes`, so a host wanting a password fails instead of hanging),
host key checking stays on, and the connection is multiplexed with
`ControlPersist` so a second run does not pay another handshake. A LAN-only
run is still possible with `--address`, but it is now a deliberate choice
rather than the default.

Cost, measured on loopback with `tools/offscreen` (a full instance/device
setup, render and readback, so many round trips): **19-22 ms direct against
25-27 ms tunnelled**, about +5.7 ms. Loopback is where this looks worst,
because direct TCP there has almost no latency for ssh to hide behind; over a
real link with milliseconds of round-trip time that fixed cost is dwarfed by
the protocol's own ~2 synchronous round trips per frame. The ceiling stays
protocol-bound, not encryption-bound.

### On Windows

Windows 10 and later ship the OpenSSH client, so nothing needs installing.
What the machine does need:

- a key pair (`ssh-keygen -t ed25519`) with the public half in the GPU
  machine's `~/.ssh/authorized_keys`, since `BatchMode=yes` means a password
  prompt is a failure rather than a question;
- one interactive `ssh user@gpu-machine` first, to record the host key in
  `%USERPROFILE%\.ssh\known_hosts` - host key checking is not disabled here;
- the tunnel left running in its own window, because **Windows OpenSSH does
  not implement `ControlMaster`/`ControlPath`**. `tools/remoting_tunnel.py`
  detects this and omits those options, so `open` works, but there is no
  multiplexed connection to reuse or to close with `close`; stop the ssh
  process instead. The cost is one handshake per tunnel, which is once per
  session rather than once per frame.

```
ssh -N -L 127.0.0.1:24680:127.0.0.1:24680 user@gpu-machine
set VK_REMOTING_HOST=127.0.0.1
vkcube.exe
```

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
| apple.water → water.n2n | 39,205 us mean | **0** |

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
