# Skinned Mesh BLAS Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make skinned/animated meshes render correctly under ray tracing by rebuilding their BLAS each frame from the GPU-skinned vertex buffer.

**Architecture:** Godot's skeleton compute shader (`skeleton.glsl`) already outputs post-skin vertex positions to `MeshInstance::Surface::vertex_buffer[current_buffer]` every frame. We detect skinned mesh instances during TLAS build, create their BLAS from the animated buffer instead of the static one, and mark them dirty each frame so the BLAS is rebuilt with fresh vertex data.

**Tech Stack:** C++ (Godot engine), Vulkan acceleration structures, godot-cpp patterns

**Branch:** `gpu-driven-tlas-optimization` in `/storage/projects/code/godot/nvidia-rtx-godot/`

---

## Background

### Current Pipeline (Static Only)

```
Static Vertex Buffer → blas_create() → BLAS (built once) → TLAS instance
```

### Target Pipeline (Skinned Support)

```
Skeleton Compute Shader → Animated Vertex Buffer[current] → blas_create() → BLAS (rebuilt per frame) → TLAS instance
```

### Key Files

| File | Role |
|------|------|
| `servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp` | TLAS build, BLAS management |
| `servers/rendering/renderer_rd/forward_clustered/render_raytracing.h` | RTSurfaceData, cache structs |
| `servers/rendering/renderer_rd/storage_rd/mesh_storage.h` | MeshInstance::Surface, skeleton check |
| `servers/rendering/renderer_rd/storage_rd/mesh_storage.cpp` | `update_mesh_instances()`, animated buffer lifecycle |
| `servers/rendering/rendering_device.cpp` | `blas_create()` public API |
| `drivers/vulkan/rendering_device_driver_vulkan.cpp` | Vulkan BLAS creation |

### Key Data Structures

```cpp
// mesh_storage.h:187-210
struct MeshInstance {
    Mesh *mesh = nullptr;
    RID skeleton;                    // Check .is_valid() for skinned mesh
    struct Surface {
        RID vertex_buffer[2];        // Double-buffered post-skin output
        uint32_t current_buffer = 0; // Which buffer has this frame's data
    };
    LocalVector<Surface> surfaces;
    uint64_t skeleton_version = 0;   // Incremented when skeleton updates
};

// render_raytracing.h — BLAS cache entry
struct RTSurfaceData {
    RID blas;
    // ... material, SBT offset, etc.
};
```

---

## Task 1: Add Skinned Mesh Detection to process_surface()

**Files:**
- Modify: `servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp:240-340` (`process_surface`)
- Modify: `servers/rendering/renderer_rd/forward_clustered/render_raytracing.h` (`RTSurfaceData`)

**Step 1: Add `is_skinned` flag and `skeleton_version` to RTSurfaceData**

In `render_raytracing.h`, find the `RTSurfaceData` struct and add:

```cpp
// In RTSurfaceData struct
bool is_skinned = false;
uint64_t last_skeleton_version = 0;
```

**Step 2: Detect skinned mesh in process_surface()**

In `render_raytracing.cpp`, in the `process_surface()` function, after the existing vertex buffer retrieval (around line 316), add detection:

```cpp
// After getting mesh_instance and surface data
bool is_skinned = false;
RID mesh_instance_rid; // Need to pass this through

// Check if this geometry instance has a skeleton
// The GeometryInstanceForwardClustered has mesh_instance RID
// from the scene cull data
if (p_mesh_instance_rid.is_valid()) {
    MeshStorage *mesh_storage = MeshStorage::get_singleton();
    // mesh_instance_rid is valid = skeleton compute shader ran
    is_skinned = true;
}
```

The key question is how `process_surface()` gets the mesh instance RID. Currently it receives `p_surf` (surface cache) and `p_mesh_surface` (mesh surface). We need to thread the mesh_instance RID through from `build_tlas()`.

**Step 3: Thread mesh_instance RID through to process_surface()**

In `render_raytracing.h`, update the `process_surface` signature:

```cpp
RTSurfaceData *process_surface(
    const void *p_surf,
    void *p_mesh_surface,
    uint16_t p_surface_invalidation_counter,
    const Transform3D &p_transform,
    LocalVector<RID> &r_dirty_blas_list,
    RID p_mesh_instance = RID());  // NEW: mesh instance for skinned detection
```

In `render_raytracing.cpp` `build_tlas()`, update the call site (around line 1232) to pass the mesh instance RID from the geometry instance.

**Step 4: Store skinned flag on cache entry**

```cpp
surf_data->is_skinned = is_skinned;
```

**Step 5: Commit**

```bash
git add servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp \
       servers/rendering/renderer_rd/forward_clustered/render_raytracing.h
git commit -m "feat(rt): detect skinned meshes in BLAS pipeline"
```

---

## Task 2: Use Animated Vertex Buffer for Skinned BLAS

**Files:**
- Modify: `servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp:316-340` (vertex buffer selection in `process_surface`)

**Step 1: Branch vertex buffer source based on skinned status**

In `process_surface()`, where the vertex buffer is retrieved (around lines 316-330), add the skinned path:

```cpp
RID vertex_array_rd;
RD::VertexFormatID vertex_format;

if (is_skinned && p_mesh_instance.is_valid()) {
    // Use post-skeleton-compute animated vertex buffer
    MeshStorage::get_singleton()->mesh_instance_surface_get_vertex_arrays_and_format(
        p_mesh_instance, surface_index,
        input_mask,
        false,  // p_input_motion_vectors
        false,  // p_point_size_emulated
        vertex_array_rd, vertex_format);
} else {
    // Static mesh path (existing code)
    MeshStorage::get_singleton()->mesh_surface_get_vertex_arrays_and_format(
        p_mesh_surface, input_mask, false, vertex_array_rd, vertex_format);
}
```

**Step 2: Verify the input_mask is correct**

The RT pipeline needs position data. Check what `input_mask` the existing code uses — it should include at minimum the position attribute. The mask used for RT BLAS only needs positions (and optionally normals for any-hit shading).

**Step 3: Commit**

```bash
git add servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp
git commit -m "feat(rt): use animated vertex buffer for skinned mesh BLAS"
```

---

## Task 3: Force BLAS Rebuild Each Frame for Skinned Meshes

**Files:**
- Modify: `servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp` (`process_surface` and `build_tlas`)

**Step 1: Track skeleton version and mark BLAS dirty when it changes**

In `process_surface()`, after the cache hit logic, add skeleton version checking:

```cpp
if (surf_data->is_skinned) {
    MeshStorage *mesh_storage = MeshStorage::get_singleton();
    uint64_t current_skeleton_version = mesh_storage->mesh_instance_get_skeleton_version(p_mesh_instance);

    if (current_skeleton_version != surf_data->last_skeleton_version) {
        // Skeleton updated — need to rebuild BLAS with new vertex positions
        surf_data->last_skeleton_version = current_skeleton_version;

        // Free old BLAS and create new one from animated buffer
        if (surf_data->blas.is_valid()) {
            RD::get_singleton()->free(surf_data->blas);
        }
        surf_data->blas = RD::get_singleton()->blas_create(vertex_array_rd, index_array_rd);
        r_dirty_blas_list.push_back(surf_data->blas);
    }
}
```

**Step 2: Expose skeleton version from MeshStorage if not already public**

Check if `mesh_instance_get_skeleton_version()` exists. If not, add to `mesh_storage.h`:

```cpp
_FORCE_INLINE_ uint64_t mesh_instance_get_skeleton_version(RID p_mesh_instance) {
    MeshInstance *mi = mesh_instance_owner.get_or_null(p_mesh_instance);
    ERR_FAIL_NULL_V(mi, 0);
    return mi->skeleton_version;
}
```

**Step 3: Commit**

```bash
git add servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp \
       servers/rendering/renderer_rd/storage_rd/mesh_storage.h
git commit -m "feat(rt): rebuild BLAS each frame for skinned meshes"
```

---

## Task 4: Ensure Skeleton Compute Runs Before TLAS Build

**Files:**
- Modify: `servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp` (render pipeline ordering)

**Step 1: Verify execution order**

The skeleton compute shader (`update_mesh_instances()`) must run BEFORE `build_tlas()`. Check the render pipeline in `_render_scene()`:

```
Expected order:
1. update_mesh_instances()  — skeleton compute shader, writes animated verts
2. build_tlas()             — reads animated verts, builds BLAS
3. RT rendering             — traces rays against TLAS/BLAS
```

In `render_forward_clustered.cpp`, find `_render_scene()` and verify `update_mesh_instances()` is called before `build_acceleration_structures()`. If not, reorder.

**Step 2: Add a pipeline barrier if needed**

The skeleton compute shader writes to vertex buffers, and BLAS build reads them. There may need to be a memory barrier between them. Check if Godot's draw graph handles this automatically via resource tracking, or if we need an explicit barrier:

```cpp
// If needed after skeleton compute, before BLAS build:
RD::get_singleton()->barrier(
    RD::BARRIER_MASK_COMPUTE,
    RD::BARRIER_MASK_ACCELERATION_STRUCTURE_BUILD);
```

**Step 3: Commit**

```bash
git add servers/rendering/renderer_rd/forward_clustered/render_forward_clustered.cpp
git commit -m "feat(rt): ensure skeleton compute runs before BLAS build"
```

---

## Task 5: Test with Bistro Demo (Animated Characters)

**Step 1: Build optimized**

```bash
cd /storage/projects/code/godot/nvidia-rtx-godot
scons -j$(nproc) platform=linuxbsd target=editor dev_build=no vulkan=yes
```

**Step 2: Test with GI demo (regression — no skinned meshes)**

```bash
DISPLAY=:0 ./bin/godot.linuxbsd.editor.x86_64 \
    --path /storage/projects/code/godot/godot-demo-projects/3d/global_illumination \
    res://test.tscn
```

Expected: RT renders correctly, no regression on static meshes.

**Step 3: Test with Bistro (has animated characters)**

```bash
DISPLAY=:0 ./bin/godot.linuxbsd.editor.x86_64 \
    --path /storage/projects/code/godot/Bistro-Demo-Tweaked \
    res://MainScene.tscn
```

Expected: Animated characters now visible and animating under RT.

**Step 4: Test with planetoid (skinned player)**

```bash
DISPLAY=:0 ./bin/godot.linuxbsd.editor.x86_64 \
    --path /storage/projects/code/godot/hyper-tps \
    res://scenes/test/dc_planetoid_rtx_test.tscn
```

Expected: Player model animates correctly under RT + DLSS-RR.

**Step 5: Performance check**

Press P in each scene to check FPS. Skinned BLAS rebuild adds per-frame cost. Acceptable if:
- < 1ms additional frame time for a few skinned meshes
- No regression on static-only scenes

**Step 6: Commit**

```bash
git commit -m "test(rt): verify skinned mesh BLAS on Bistro, GI demo, planetoid"
```

---

## Task 6: Optimize — BLAS Refit Instead of Rebuild (Optional)

**Files:**
- Modify: `servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp`
- Modify: `drivers/vulkan/rendering_device_driver_vulkan.cpp` (if BLAS needs ALLOW_UPDATE flag)

**Step 1: Create skinned BLAS with ALLOW_UPDATE flag**

When creating BLAS for skinned meshes, add the update flag so we can refit instead of full rebuild:

```cpp
// In process_surface(), when creating BLAS for skinned mesh:
surf_data->blas = RD::get_singleton()->blas_create(
    vertex_array_rd, index_array_rd,
    RD::ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT,  // geometry bits
    0);  // position attribute location

// Mark it as supporting updates (may need driver-level change)
```

This requires the Vulkan BLAS to be created with `VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR`. Check if the driver's `blas_create()` supports this flag; if not, add a parameter.

**Step 2: Use refit instead of rebuild on subsequent frames**

```cpp
if (surf_data->is_skinned && surf_data->last_skeleton_version != current_skeleton_version) {
    surf_data->last_skeleton_version = current_skeleton_version;
    // Update vertex data reference (if buffer RID changed due to double-buffer swap)
    // Then refit instead of rebuild:
    RD::get_singleton()->acceleration_structure_update(surf_data->blas);
    // Don't add to dirty list — refit doesn't need full rebuild
}
```

**Note:** NVIDIA recommends rebuild over refit for BLAS when vertices change significantly (skinning can cause large deformations). Benchmark both approaches. Refit is faster but produces lower-quality BVH that degrades ray traversal performance.

**Step 3: Benchmark refit vs rebuild**

Run the Bistro benchmark with both approaches, compare:
- BLAS build time (should be lower with refit)
- RT render time (may be higher with refit due to BVH quality)
- Net frame time

**Step 4: Commit**

```bash
git add servers/rendering/renderer_rd/forward_clustered/render_raytracing.cpp \
       drivers/vulkan/rendering_device_driver_vulkan.cpp
git commit -m "perf(rt): BLAS refit for skinned meshes (optional optimization)"
```

---

## Risk Notes

1. **Vertex format mismatch**: The animated vertex buffer from skeleton compute may have a different stride/format than what `blas_create()` expects. The skeleton shader outputs positions + normals + tangents; BLAS only needs positions. Verify the vertex format mask extracts position correctly.

2. **Double-buffer timing**: The animated buffer swaps each frame (`current_buffer ^= 1`). The BLAS must use the SAME buffer index that the skeleton compute shader just wrote to. If the swap happens between skeleton compute and BLAS build, we'd read stale data.

3. **Morph targets / blend shapes**: The skeleton compute shader also handles blend shapes. Our approach covers this automatically since we use the post-compute output buffer.

4. **Performance**: Rebuilding BLAS each frame for every skinned mesh is expensive with many animated characters. For production, consider LOD-based culling (only rebuild BLAS for nearby skinned meshes, use rasterizer for distant ones).

5. **Memory**: Each skinned mesh BLAS needs scratch memory for rebuilds. With many animated characters, this could spike VRAM usage.
