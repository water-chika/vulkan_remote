# Compatibility roadmap

This document separates implemented and tested behavior from planned coverage.
A command family is not considered supported merely because it appears in the
pinned Vulkan registry or the real server driver implements it.

## Current tested baseline

| Workload | Coverage | Oracle |
|---|---|---|
| `vulkan_remoting_offscreen` | Classic render pass, graphics pipeline, direct draw, image readback | Direct/remoted PPM bytes match |
| `vulkan_remoting_texture_upload` | Staging upload, image barriers, descriptors, sampler, textured draw | Direct/remoted PPM bytes match |
| `vulkan_remoting_indexed_draw` | Two vertex bindings at distinct nonzero offsets, uint16 index buffer, indexed draw | Direct/remoted PPM bytes match |
| `vulkan_remoting_compute` | Storage buffer, compute pipeline, dispatch, mapped readback | Exact checksum |
| `vulkan_remoting_push_descriptor_compute` | Two partial push writes, two sets, sparse bindings, three-element descriptor array with split updates | Exact words and checksum direct/remoted |
| `vulkan_remoting_spirv15_compute` | SPIR-V 1.5 Vulkan memory model, storage buffer compute | Exact words and checksum direct/remoted |
| `vulkan_remoting_buffer_ops` | Buffer fill, update, copy, transfer barrier, mapped readback | Exact checksum |
| `vulkan_remoting_multi_submit_reuse` | Multiple submit infos/command buffers, ordered consecutive submits, command-buffer re-record, fence status/reset/reuse | Exact 64-word checksum direct/remoted |
| `vulkan_remoting_offscreen_reuse` | Three reused graphics frames across each of two device epochs on one instance/connection | Exact per-frame pixels and aggregate checksum direct/remoted |
| `vulkan_remoting_mipmap` | Four mip levels, per-level barriers, same-image blits, multi-region readback | Exact 340-byte chain and checksum |
| `vulkan_remoting_noncoherent_memory` | Two disjoint nonzero atom-aligned ranges, explicit flush/invalidate, GPU copies | Exact 128 synchronized bytes and checksum, or matching capability skip |
| `vkcube`, `vkcubepp` | Acquire, submit, present, fences, server-owned native window | Bounded frame completion |
| `vulkaninfo --summary` | Instance/device enumeration and basic properties | Selected normalized fields |
| CTS smoke/info plus focused descriptor/push groups | Known passing cases in `tests/cts_baseline.txt` | Baseline regression delta with exact matched counts and preserved QPA/server logs |

The ICD reports Vulkan 1.2 after completing the registry-derived cumulative
command gates: 137/137 Vulkan 1.0, 28/28 Vulkan 1.1, and 13/13 Vulkan 1.2.
`VK_KHR_get_physical_device_properties2` remains available through KHR aliases.
Bounded feature/property `pNext` chains are supported. The dependency-closed
FSR device contract comprises `VK_KHR_multiview`, `VK_KHR_maintenance2`,
`VK_KHR_image_format_list`, `VK_KHR_create_renderpass2`,
`VK_KHR_fragment_shading_rate`, and `VK_KHR_imageless_framebuffer`; it is
advertised only when every member is supported by the real device. Device proc
addresses for create-renderpass2 and FSR are gated by enabled extensions.
`VK_KHR_dynamic_rendering` remains absent.

A registry-derived gate reports **137 of 137** Vulkan 1.0 core commands exposed
by the client, with matching server handlers or intentional local implementations.
The completed families include query pools, indirect and secondary execution,
resolve/clear/events/dynamic state, buffer views and texel descriptors, descriptor
pool operations, pipeline cache data/merge, render granularity, memory commitment,
and bounded sparse queries/binds.

## Acceptance rule for every milestone

1. Add a minimal deterministic repository-local sample.
2. Run it directly and through the remoting ICD against the same real driver.
3. Compare exact readback bytes/checksums, or explicit invariants for inherently
   nondeterministic results.
4. Add malformed/count-boundary protocol tests for new variable-size payloads.
5. Add reviewed CTS cases only after the standalone regression passes.
6. Run the cross-platform matrix where the required machines are available.

## Milestone 1 — broaden existing Vulkan 1.0 paths

These samples mostly use commands already remoted and should land before broader
API claims:

- Sequential sessions on one server and two simultaneous clients.

## Milestone 2 — Vulkan 1.0 command families (implemented)

All core command families are callable. Variable-size wire payloads are bounded,
non-dispatchable objects use per-session tables, and sparse feature bits remain
masked by the real-device feature intersection rather than being fabricated.

## Milestone 3 — bounded extension bridge (implemented capability contract)

The properties2 query family now has field-wise request/reply handling and a
bounded whitelist of supported input/output `pNext` structures. Renderpass2,
fragment-shading-rate, image/view, imageless-framebuffer, and device-feature
chains likewise use closed call-site allowlists. Unknown, duplicate, truncated,
or over-limit nodes are rejected rather than copied or silently dropped.

This extension bridge was retained through the Vulkan 1.2 promotion; promoted
core names are version-gated while extension aliases remain extension-gated.

## Milestone 3.5 — Vulkan 1.1 command surface (implemented and promoted)

The registry-derived gate covers all 28 Vulkan 1.1 commands. Core and promoted
KHR names share implementations. Physical-device groups are conservatively
represented as one-device groups, external handle properties expose no
transportable handle types, protected-memory behavior remains disabled, and
`vkEnumerateInstanceVersion` and the ICD manifest report Vulkan 1.2; both
physical-device property paths report `min(real device API, Vulkan 1.2)`. Bind-memory2,
memory-requirements2, device-group recording, queue2, command-pool trim,
descriptor update templates/layout support, and sampler YCbCr conversion have
bounded codecs and owned server object tables.

## Milestone 3.6 — Vulkan 1.2 public command surface

The registry-derived gate covers all 13 Vulkan 1.2 commands and the public
client contract is promoted atomically to Vulkan 1.2. Host query reset, timeline semaphore counter/wait/signal (including
semaphore-create and submit value chains), indirect-count recording, and the
existing renderpass2 family have bounded transports. Buffer/device-memory
address entry points are callable but return zero because remote addresses and
capture/replay tokens are not meaningful in the client process; their feature
bits remain false. Vulkan 1.2 aggregate and promoted individual query chains
share one policy that also masks unsupported descriptor-indexing semantics.
The server independently queries its loader version, requests at most Vulkan
1.2, and reports that version in the compatibility handshake; client objects
retain the negotiated internal version for core-versus-extension dispatch.
A checked-in SPIR-V 1.5 compute module uses `VulkanMemoryModel`, produces an
exact deterministic checksum directly and remotely, and has a trace word-preservation gate.

## Milestone 4 — Khronos Vulkan-Samples

After the framework bootstrap works, validate in this order:

1. `hello_triangle`
2. `texture_mipmap_generation`
3. `dynamic_uniform_buffers`
4. `separate_image_sampler`
5. `instancing`
6. `compute_nbody`
7. `timestamp_queries`
8. `swapchain_recreation`

Use benchmark/fixed-time mode, fixed dimensions, fixed frame count, and canonical
RGBA output. Modern extension samples—timeline semaphores, synchronization2,
dynamic rendering, buffer device address, ray tracing, mesh shading, descriptor
indexing, and shader objects—remain later milestones driven by concrete tests.

## Known limitations

- Little-endian x86-64 peers only.
- Client and server must use matching registry, schema, ABI, and command digest.
- Arbitrary `pNext` chains are unsupported; only the reviewed bounded
  call-site-specific allowlists are encoded.
- Only one active shadow mapping is supported per memory allocation.
- Coherent mapped writes become visible at remoting synchronization points rather
  than continuously, and GPU writes do not automatically refresh an existing
  coherent shadow after host waits.
- No X11, `VK_KHR_display`, dmabuf/data-device/subsurface Wayland proxy support.
- The Windows local-display input bridge posts ordinary HWND messages; Raw Input,
  DirectInput, global keyboard-state APIs, true focus transfer, IME, and complete
  non-US layout fidelity are outside its supported contract.
