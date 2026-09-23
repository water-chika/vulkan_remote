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
| `tools/offscreen.cpp` | ordinary Vulkan program used as the regression test |
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

### Running the client on Windows

The client half builds for Windows; the server half does not, and is not
meant to. The split is the point: the server is where the GPU and the
compositor are, so it stays Linux/Wayland, and a Windows box is a *client*
needing no GPU of its own. Since the server owns the window, the Windows
client links no libwayland at all.

Cross-built from Linux, which is how the DLL below was produced:

```sh
mkdir -p /tmp/vkinc && ln -s /usr/include/vulkan /usr/include/vk_video /tmp/vkinc/
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared -static -I/tmp/vkinc \
    -Icommon -Iclient -Ibuild -o vulkan_remoting_icd.dll \
    client/*.cpp common/wire.cpp common/marshal.cpp -lws2_32
```

Pass only the Vulkan headers, not `-I/usr/include`: the latter puts glibc's
`stdlib.h` ahead of mingw's and the build dies on a redefined `div_t`.

`-static` is not optional, and is the fix for a real failure. Without it the
DLL imports `libstdc++-6.dll`, `libgcc_s_seh-1.dll` and
`libwinpthread-1.dll`, none of which ship with Windows, and `LoadLibrary`
then fails with **error 126**. That error names only the module it was asked
to load, never the dependency that was actually missing, so it reads as
"your ICD is broken" when the ICD is fine. Copying those three DLLs alongside
also works, but an ICD is loaded into another program's process, so the
search runs on the host's terms; a host that already loaded a different
`libstdc++-6.dll` gives an ABI mismatch instead of an honest failure.
`cmake` does this for you (see the `if(WIN32)` block); the statically linked
DLL imports only `KERNEL32`, `WS2_32` and the UCRT, and is 2.7 MB against
438 KB.

Installing it on the Windows machine is three steps, because the Vulkan
loader finds a driver differently there than on Linux:

1. Put `vulkan_remoting_icd.dll` anywhere readable, say `C:\vulkan_remoting\`.
2. Write an ICD manifest next to it, `C:\vulkan_remoting\icd.json`, whose
   `library_path` is an **absolute** path to the DLL:

   ```json
   {"file_format_version": "1.0.0",
    "ICD": {"library_path": "C:\\vulkan_remoting\\vulkan_remoting_icd.dll",
            "api_version": "1.0.0"}}
   ```

   The spec says a relative `library_path` resolves against the manifest's
   own directory, and this document used to claim so, but in practice on
   Windows a relative path was not found and an absolute one was needed. Note
   that this ICD cannot paper over that itself: `library_path` is read by the
   loader in order to decide what to load, so our code does not exist yet at
   the moment the path is resolved. Whoever writes the manifest has to get it
   right.

3. Point the loader at that manifest. Set **both** variables, to the absolute
   path of the JSON - `VK_ICD_FILENAMES` is the older name and some loaders
   still honour only it:

   ```
   set VK_ICD_FILENAMES=C:\vulkan_remoting\icd.json
   set VK_DRIVER_FILES=C:\vulkan_remoting\icd.json
   ```

   The loader **ignores both for elevated processes**, and a plain `ssh`
   session on Windows lands elevated in session 0, where they are silently
   dropped. The run that proved this client worked used a non-elevated
   session-1 launch; if the driver appears to be ignored, check this first.

   There is also a registry route - a `REG_DWORD` named for the manifest's
   full path, data 0, under `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` or the
   `HKCU` equivalent - which avoids the environment entirely. It is
   *untested* here: the working run used the variables above, not the
   registry.

Then point it at the Linux server, which must already be running:

```
set VK_REMOTING_HOST=<gpu-machine>
vkcube.exe
```

Proven on a Windows client against a Linux server: the DLL loads, connects
over TCP, and `vulkaninfo.exe --summary` enumerates the server's real GPUs by
their exact names - which is also a struct-ABI check, since those names
arrive as real values rather than garbage. Both peers blit whole Vulkan
structs over the wire, so they must agree on that ABI; Windows and Linux on
x86-64 do, but a 32-bit or ARM peer would misread every struct with no
handshake failure to warn it. Not yet exercised from Windows: presenting a
window, and so any frame rate figure.

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

## What works

- `tools/offscreen` renders a triangle and reads it back: output is
  **byte-identical** whether run on the system driver or through this one.
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
- Add the minimum command families needed by broader samples, in evidence-driven
  order: transfer (`vkCmdCopyImage`/`vkCmdBlitImage`/`vkCmdFillBuffer`/
  `vkCmdUpdateBuffer`), then compute (`vkCreateComputePipelines`/`vkCmdDispatch`).
- Run pinned triangle, texture-upload, and compute samples after those APIs exist;
  keep unsupported modern Vulkan and arbitrary `pNext` use explicitly out of scope.
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
