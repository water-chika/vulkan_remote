# Compatibility roadmap

This document separates implemented and tested behavior from planned coverage.
A command family is not considered supported merely because it appears in the
pinned Vulkan registry or the real server driver implements it.

## Current tested baseline

| Workload | Coverage | Oracle |
|---|---|---|
| `vulkan_remoting_offscreen` | Classic render pass, graphics pipeline, direct draw, image readback | Direct/remoted PPM bytes match |
| `vulkan_remoting_texture_upload` | Staging upload, image barriers, descriptors, sampler, textured draw | Direct/remoted PPM bytes match |
| `vulkan_remoting_compute` | Storage buffer, compute pipeline, dispatch, mapped readback | Exact checksum |
| `vulkan_remoting_buffer_ops` | Buffer fill, update, copy, transfer barrier, mapped readback | Exact checksum |
| `vulkan_remoting_mipmap` | Four mip levels, per-level barriers, same-image blits, multi-region readback | Exact 340-byte chain and checksum |
| `vulkan_remoting_noncoherent_memory` | Two disjoint nonzero atom-aligned ranges, explicit flush/invalidate, GPU copies | Exact 128 synchronized bytes and checksum, or matching capability skip |
| `vkcube`, `vkcubepp` | Acquire, submit, present, fences, server-owned native window | Bounded frame completion |
| `vulkaninfo --summary` | Instance/device enumeration and basic properties | Selected normalized fields |
| CTS `api.smoke.*`, `api.info.*` | Known passing cases in `tests/cts_baseline.txt` | Baseline regression delta |

The ICD intentionally reports Vulkan 1.0. It no longer advertises
`VK_KHR_get_physical_device_properties2`; that extension returns only after its
full required entry-point family and bounded chain marshalling land.

A registry-derived gate currently reports **96 of 137** Vulkan 1.0 core commands
exposed by the client, leaving 41 command entry points. Every exposed RPC must
also have a server handler; the test fails if an implementation is added on only
one side. The exact missing list is printed by `tests/test_core_coverage.py` and
will shrink slice by slice until it becomes the 137/137 completion gate.

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

- Indexed drawing and multiple vertex bindings.
- Push constants and multiple descriptor bindings/array elements.
- Multiple command buffers/submits, fence reset/reuse, and queue ordering.
- Multi-frame offscreen resource reuse and device recreation.
- Sequential sessions on one server and two simultaneous clients.

## Milestone 2 — missing Vulkan 1.0 command families

Implement each family as one reviewed unit with a dedicated sample:

1. Query pools: create/destroy/reset, begin/end, timestamps, get/copy results.
2. Indirect execution: draw, indexed draw, and dispatch indirect.
3. Secondary command buffers: `vkCmdExecuteCommands`.
4. Resolve and clear operations: multisample resolve, depth/stencil clear,
   clear attachments, and multi-subpass rendering.
5. Dynamic state setters.
6. Buffer views and texel-buffer access.
7. Descriptor-set free/pool reset, pipeline-cache data/merge, and render-area
   granularity.
8. Sparse APIs only after a concrete sample requires them.

## Milestone 3 — truthful Vulkan 1.1/properties2 bridge

Before reporting Vulkan 1.1 or running the current Khronos Vulkan-Samples
framework, add field-wise request/reply handling for the query families it uses:

- instance-version enumeration;
- features2, properties2, memory-properties2, queue-family-properties2;
- format/image-format properties2 as required;
- a bounded whitelist of supported input/output `pNext` structures.

Do not copy arbitrary native `pNext` memory across the wire. Keep reporting
Vulkan 1.0 until every entry point needed for the advertised version is callable.

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
- Arbitrary `pNext` chains are dropped.
- Only one active shadow mapping is supported per memory allocation.
- Coherent mapped writes become visible at remoting synchronization points rather
  than continuously, and GPU writes do not automatically refresh an existing
  coherent shadow after host waits.
- No X11, `VK_KHR_display`, dmabuf/data-device/subsurface Wayland proxy support.
- The Windows local-display input bridge posts ordinary HWND messages; Raw Input,
  DirectInput, global keyboard-state APIs, true focus transfer, IME, and complete
  non-US layout fidelity are outside its supported contract.
