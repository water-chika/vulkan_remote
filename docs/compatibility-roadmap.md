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
| `vulkan_remoting_noncoherent_memory` | Two disjoint nonzero atom-aligned ranges, explicit flush/invalidate, GPU copies | Exact 128 synchronized bytes and checksum remotely; native may skip if every host-visible type is coherent |
| `vulkan_remoting_coherent_memory` | Experimental persistent coherent mappings, two no-flush/no-invalidate epochs | Exact 128 bytes and checksum when coherent negotiation is enabled |
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

Milestone 1 deterministic coverage is complete. Persistent-server acceptance now
runs without requiring an optional validation-layer installation and includes repeated
sequential clients, two explicitly overlapping initialized clients,
abrupt loss of one session while its peer completes, and a clean follow-up client.

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

The framework bootstrap records Vulkan-Samples source revision
`177edebf0cd7d4f669667e49f052cfb56b17e004`. `tests/run_vulkan_samples.py`
requires the selected source checkout's `HEAD` to match that revision; the external
executable path remains an explicit harness input. The runner
runs a selected sample natively and remotely with Release/Wayland/GLSL, fixed
dimensions, benchmark time, and a bounded frame count. All eight ordered targets
below have passed at 640x480 for bounded 20-frame runs, and each requires a clean
follow-up compute client. Process completion and validation/protocol-clean logs
prove framework and WSI liveness. The checked-in capture-gate patch plus
`--compare-rgba` holds a fixed frame in isolated 640x480 headless Sway compositors,
hides UI, and requires exact native/remoted RGBA equality. This passes for the six
framework-render-context targets from `texture_mipmap_generation` through
`timestamp_queries`. `hello_triangle` remains liveness-only because it is outside
the screenshot hook; `swapchain_recreation` intentionally uses a different app path
that does not trigger that hook.

Validated order:

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
- The default facade hides `HOST_COHERENT`; host-visible memory requires
  explicit atom-valid flush/invalidate and applications with no fallback may
  fail memory-type selection. When both peers set
  `VK_REMOTING_EXPERIMENTAL_COHERENT=1`, negotiation enables whole-mapping
  publication before submit and refresh after successful fence/idle completion.
  This is compatibility coverage for synchronized transfer paths, not a general
  strict-coherence claim: ordinary non-sparse storage descriptors,
  classic/render-pass-2 attachments including imageless, secondary command
  buffers, and GPU timeline signals are conservatively attributed, while sparse
  aliasing remains unsupported. Arbitrary concurrent pointer writes still
  require future OS dirty tracking or KMD-managed shared memory.
- No X11, `VK_KHR_display`, dmabuf/data-device/subsurface Wayland proxy support.
- The Windows local-display input bridge posts ordinary HWND messages; Raw Input,
  DirectInput, global keyboard-state APIs, true focus transfer, IME, and complete
  non-US layout fidelity are outside its supported contract.
