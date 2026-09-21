# vulkan_remoting

Run Vulkan calls on a machine that owns a GPU, from a machine that does not.

The client is a real Vulkan ICD, so the stock Khronos loader drives it. An
unmodified `vulkaninfo` on a machine with no GPU lists the GPU of another
machine across the network, because every query it makes is forwarded over a
TCP socket and answered by the real driver at the far end.

```
  client machine                              server machine
  (no GPU needed)                             (has the GPU)

  vulkaninfo                                  server
      |                                          |
  Vulkan loader                               real ICD (RADV)
      |                                          |
  client/icd.cpp  <---- TCP, opcode+payload ---> server/server.cpp
```

## Layout

| path | what it is |
|---|---|
| `common/wire.{hpp,cpp}` | framing, bounds-checked reader/writer, connect helper |
| `common/generate_remoting.py` | derives opcodes from `vk.xml`, emits a command-set digest |
| `client/icd.cpp` | the Vulkan ICD the loader loads |
| `server/server.cpp` | runs where the GPU is; calls the real driver |
| `tools/probe.cpp` | links no Vulkan at all; times the round trips |
| `tests/test_wire.py` | failure-path tests, stdlib only |

Opcodes are not hand-numbered. `generate_remoting.py` sorts the command names
out of `vk.xml` and hashes the resulting list into `kCommandSetDigest`, which
both ends exchange at connect. Two builds from different headers therefore
fail at the handshake rather than silently invoking the wrong command.

## Build

```sh
cmake -B build && cmake --build build
```

Start the server on the machine with the GPU, then point the loader at the ICD:

```sh
./build/vulkan_remoting_server --port 24680                 # GPU machine
VK_DRIVER_FILES=$PWD/build/vulkan_remoting_icd.json vulkaninfo   # other machine
```

## What works

`vulkaninfo` completes instance creation, `vkEnumeratePhysicalDevices`, and the
physical-device property, queue-family, memory and feature queries, with no
unsupported-command hits in the server log. It stops at `vkCreateDevice`, which
is refused deliberately and with an explanatory message.

Cross-machine: a probe on `arch2.dorm`, which owns no Vulkan stack at all,
listed `water.n2n`'s RX 9070 XT and its Vega 3.

`tests/test_wire.py` covers the failure paths a local function call does not
have: short frames, truncated payloads, oversized length fields, a wrong
digest, an unknown opcode, and a peer that hangs up mid-message.

## What it measures, and what that means

| link | mean | worst | synchronous calls affordable per 16.7 ms frame |
|---|---|---|---|
| loopback | 15.8 us | 30.4 us | ~1059 |
| arch2.dorm -> water.n2n | 7,187.2 us | 17,752.7 us | **2** |
| apple.water -> water.n2n | 39,204.6 us | 61,812.2 us | **0** |

`arch2 -> water.n2n` costs almost exactly the 7.5 ms ICMP round trip, so the
time is network latency and nothing else.

Two synchronous calls per frame is the whole result. A frame of a real game is
thousands of Vulkan calls. The cost is **per call, not per byte**, so neither
compressing the payload nor a faster link changes the number meaningfully —
only removing round trips does, and the round trips are the API.

This is why API remoting is not how remote rendering is done in practice. The
working answer is to ship finished frames instead: Sunshine/Moonlight, or
`waypipe` for a Wayland application.

## The WSI problem, and the cheapest way out

`vkcube` never reaches `vkCreateDevice`. It fails earlier, at
`VK_KHR_surface`, because this ICD advertises no instance extensions. Window
system integration for a remote GPU has three known shapes:

1. **Host-side WSI with shared memory** — what Mesa's Venus (virtio-gpu) and
   gfxstream do. Swapchain images are resources the host compositor imports
   directly. It requires both ends to share physical memory, so it cannot
   cross a network. This is the wall the earlier gfxstream experiment hit.

2. **Client-side WSI with readback** — the ICD implements the surface and
   swapchain locally against Wayland shm or X, the server renders offscreen,
   and `vkQueuePresentKHR` copies the image to a buffer and sends the pixels
   back. 1920x1080x4 B is 8.3 MB a frame, 60 of those is about 4 Gbit/s
   uncompressed, so it needs a hardware encoder — at which point it is
   Sunshine, reimplemented inside a driver, with the per-call latency above
   still charged on top.

3. **Server-side WSI — the remote window.** Put the window on the machine that
   owns the GPU. `vkCreateSwapchainKHR` opens a real window there and creates
   a genuine swapchain on the real device; `vkQueuePresentKHR` becomes a
   single opcode with no payload, and no pixel ever crosses the socket. The
   price is that you have to be looking at the GPU machine's monitor.

Option 3 is the cheapest by a wide margin — roughly three opcodes — and is the
documented next step if this is continued.

It does not, however, remove the work that comes before it: `vkCreateDevice`,
queues, command pools and buffers, `vkAllocateMemory`, pipelines, and
`vkMapMemory`. Mapped memory is the genuinely hard one, because a mapped
pointer must be dereferenceable on the client: it needs a shadow allocation
plus an explicit flush at unmap and at submit. Vulkan's explicit memory model
is what makes that tractable at all.

## Status

A study, finished at the point where it had answered its question. The
measurements above are the answer.
