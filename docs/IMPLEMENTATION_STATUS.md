# OrcaVulkanSlicer implementation status

This is a local development fork of OrcaSlicer. Its purpose is to make two
experiments independently reviewable:

- importing QIDI Studio profile bundles without renaming their files;
- building a deterministic Vulkan compute path for expensive geometry work.

It is not yet a production replacement for OrcaSlicer.

## Implemented in this fork

### QIDI Studio profile import

`Import Configs` accepts the QIDI Studio profile extensions `.qdscfg` and
`.qdsflmt`. They are ZIP containers, so they use the same validated extraction
and JSON-preset import path as Orca's `.orca_bundle` files.

The importer retains Orca's existing profile validation, version parsing,
overwrite prompt, substitution reporting, and user-preset save path. It does
not silently rename a QIDI profile to `.zip`.

### Vulkan capability and exact-geometry contract

The CMake option below is disabled by default and leaves the normal CPU build
unchanged.

```text
-DSLIC3R_ENABLE_VULKAN_SLICER=ON
```

When enabled, `VulkanSlicerBackend` enumerates devices and only marks a device
as suitable for deterministic tiled geometry when it exposes `shaderInt64`.
The current GPU shader is a perimeter/infill candidate-stage prototype; it is
not wired into the print pipeline and does not replace generated wall, infill,
support, or G-code output.

## Accuracy policy

The target representation is signed 64-bit fixed point with 1,000,000 units
per mm. Geometry is translated to a 4 mm tile before a GPU dispatch. A
host-side 128-bit predicate rejects a tile that would violate the GPU range.
Every emitted wall or infill item needs a stable source ID, so output order
does not depend on Vulkan workgroup scheduling.

GPU output may be used only after it exactly matches the fixed-point reference
IR for the same model, profile, layer range, and seed. A missing Vulkan
feature, unsupported geometry, validation mismatch, or tile overflow must
fall back to the current CPU stage.

## Remaining work

1. Import the supplied Q2 `.qdscfg` fixture and catalogue all fields,
   inherited QIDI presets, substitutions, and unsupported keys.
2. Implement a QIDI inheritance resolver for exports that reference QIDI
   system-only parents absent from Orca.
3. Add compiled SPIR-V and a dispatch path for the exact candidate/clip stages.
4. Integrate differential tests for walls, sparse/solid infill, supports, and
   the ordered G-code intermediate representation.
5. Add an opt-in UI setting with a per-stage CPU fallback report.

## Validation status

No build or runtime test has run in this workspace. CMake, a C++ compiler, and
the Vulkan SDK are not installed or discoverable here. This status is
intentional: no unverified GPU speed-up or print-safety claim is made.
