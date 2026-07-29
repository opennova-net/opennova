"""Mesh building: the part hierarchy, per-LOD mesh creation with smoothing
groups and skin capture, and the additional render LODs.

Mixin part of BlenderSceneBuilder -- instance state lives in core.__init__.
"""

from __future__ import annotations

import bpy
from mathutils import Vector

from blender.math_utils import render_space
from blender.mesh_primitives import create_cube_mesh

from .helpers import _mtrx_to_center_rotation


def _compute_smoothing_groups(faces, normals, epsilon=1e-4):
    """Compute smoothing group bitmasks from per-loop normals.

    Compares normals at shared edges to classify them as smooth or sharp,
    then flood-fills connected smooth regions.  Uses greedy graph coloring
    on the component adjacency graph so that adjacent components never
    share a smoothing-group bit (prevents false smoothing from bit
    collisions).
    """
    from collections import defaultdict, deque

    # Build edge → face adjacency
    edge_faces = defaultdict(list)
    for fi, (v0, v1, v2) in enumerate(faces):
        for a, b in ((v0, v1), (v1, v2), (v2, v0)):
            edge_faces[(min(a, b), max(a, b))].append(fi)

    # Classify edges as smooth or sharp by comparing per-loop normals
    eps_sq = epsilon * epsilon
    smooth_adj = defaultdict(set)
    for (ea, eb), flist in edge_faces.items():
        if len(flist) != 2:
            continue
        fi_a, fi_b = flist[0], flist[1]
        fa, fb = faces[fi_a], faces[fi_b]
        smooth = True
        for sv in (ea, eb):
            na = normals[fi_a * 3 + list(fa).index(sv)]
            nb = normals[fi_b * 3 + list(fb).index(sv)]
            dx = na[0] - nb[0]
            dy = na[1] - nb[1]
            dz = na[2] - nb[2]
            if dx * dx + dy * dy + dz * dz > eps_sq:
                smooth = False
                break
        if smooth:
            smooth_adj[fi_a].add(fi_b)
            smooth_adj[fi_b].add(fi_a)

    # Flood-fill smooth-connected components
    comp = [-1] * len(faces)
    cid = 0
    for fi in range(len(faces)):
        if comp[fi] >= 0:
            continue
        queue = deque([fi])
        while queue:
            f = queue.popleft()
            if comp[f] >= 0:
                continue
            comp[f] = cid
            for adj in smooth_adj.get(f, ()):
                if comp[adj] < 0:
                    queue.append(adj)
        cid += 1

    # Detect flat components: all per-loop normals within the component
    # are identical.  Flat components get SG=0 (no smoothing) because the
    # original model likely used SG=0 for co-planar faces — smoothing has
    # no effect on normals there, but SG=0 gives per-face tangent/bitangent
    # in compute_smoothed_vectors, which matters for vertex dedup.
    comp_varies = [False] * cid
    comp_ref = [None] * cid
    for fi in range(len(faces)):
        c = comp[fi]
        if c < 0 or comp_varies[c]:
            continue
        for j in range(3):
            n = normals[fi * 3 + j]
            if comp_ref[c] is None:
                comp_ref[c] = n
            else:
                ref = comp_ref[c]
                dx = n[0] - ref[0]
                dy = n[1] - ref[1]
                dz = n[2] - ref[2]
                if dx * dx + dy * dy + dz * dz > eps_sq:
                    comp_varies[c] = True
                    break
    flat_comps = set(c for c in range(cid) if not comp_varies[c])

    # Build component adjacency graph (sharp edges between components)
    comp_adj = defaultdict(set)
    for (ea, eb), flist in edge_faces.items():
        if len(flist) != 2:
            continue
        fi_a, fi_b = flist[0], flist[1]
        ca, cb = comp[fi_a], comp[fi_b]
        if ca != cb and ca >= 0 and cb >= 0:
            comp_adj[ca].add(cb)
            comp_adj[cb].add(ca)

    # Greedy graph coloring: assign each component a bit such that no
    # two adjacent components share the same bit (up to 31 distinct bits).
    comp_color = {}
    for c in range(cid):
        if c in flat_comps:
            comp_color[c] = -1  # will map to SG=0
            continue
        used = set()
        for neighbor in comp_adj.get(c, ()):
            if neighbor in comp_color and comp_color[neighbor] >= 0:
                used.add(comp_color[neighbor])
        color = 0
        while color in used:
            color += 1
        comp_color[c] = color

    result = []
    for fi in range(len(faces)):
        c = comp[fi]
        if c < 0:
            result.append(0)
        else:
            color = comp_color.get(c, 0)
            result.append(0 if color < 0 else (1 << (color % 31)))
    return result


class MeshesMixin:
    def build_part_hierarchy(self, base_name: str):
        """Create a hierarchy of Empty objects for the part tree."""
        if self.ir.lod_count == 0:
            return

        lod0 = self.ir.lods[0]
        num_parts = int(lod0.part_count)

        root = bpy.data.objects.new(base_name, None)
        root.empty_display_type = 'PLAIN_AXES'
        root.empty_display_size = 0.1
        bpy.context.collection.objects.link(root)
        root["_lod_index"] = 0
        self.root_object = root

        for i in range(num_parts):
            part_name = f"PN{i + 1:02d}"
            part_obj = bpy.data.objects.new(part_name, None)
            part_obj.empty_display_type = 'PLAIN_AXES'
            part_obj.empty_display_size = 0.05
            bpy.context.collection.objects.link(part_obj)
            self.part_nodes[i] = part_obj

        # Parent and position parts
        for i in range(num_parts):
            part = lod0.parts[i]
            part_obj = self.part_nodes[i]

            abs_pos = render_space(Vector(part.abs_position))
            rel_pos = render_space(Vector(part.rel_position))

            parent = root
            if (part.parent_index >= 0 and
                part.parent_index < num_parts and
                part.parent_index != i):
                parent = self.part_nodes[part.parent_index]

            part_obj.parent = parent
            if parent == root:
                part_obj.location = abs_pos
            else:
                part_obj.location = rel_pos

        return root

    def create_basic_meshes(self, base_name: str) -> list:
        """Create meshes from the IR (LOD 0).

        Delegates to _create_meshes_for_lod() which creates one mesh object
        per (part, material) pair.
        """
        if self.ir.lod_count == 0:
            return []

        mesh_objects = self._create_meshes_for_lod(
            self.ir.lods[0], self.part_nodes, track_bone_data=True)
        self.mesh_objects = mesh_objects
        return mesh_objects

    def _create_meshes_for_lod(self, lod, part_nodes, track_bone_data=False):
        """Create meshes for a single LOD level.

        Creates one mesh object per (part, material) pair.  Primitives that
        share the same part and material are merged into a single mesh.
        Each resulting mesh has exactly one material slot.

        Args:
            lod: IR LOD data (self.ir.lods[N])
            part_nodes: dict mapping part index -> Blender Empty object
            track_bone_data: if True, store bone data in self._mesh_bone_data
                             (only needed for LOD 0 armature binding)
        """
        mesh_objects = []
        num_parts = int(lod.part_count)
        num_primitives = int(lod.primitive_count)

        is_skinned = (int(self.ir.mesh_type) == 3)  # THREEDI_IR_MESH_SKINNED

        # Group primitives by part_index.
        # Creating one mesh per part (not per part+material) ensures that
        # normal smoothing crosses material boundaries within a part,
        # matching the reference tool's per-subobject smoothing behaviour.
        from collections import defaultdict, OrderedDict
        part_prims = defaultdict(list)
        for prim_idx in range(num_primitives):
            prim = lod.primitives[prim_idx]
            part_idx = int(prim.part_index)
            if part_idx < 0 or part_idx >= num_parts:
                continue
            if part_idx not in part_nodes:
                continue
            part_prims[part_idx].append(prim_idx)

        # Create one mesh per part (all materials merged)
        for part_idx in sorted(part_prims.keys()):
            prim_indices = part_prims[part_idx]
            part = lod.parts[part_idx]
            part_abs = Vector(part.abs_position)

            all_vertices = []
            all_faces = []
            all_uvs = []
            all_uvs1 = []
            all_normals = []
            all_bone_data = []
            all_face_mat_indices = []   # per-face material index
            vert_map = {}

            def _bone_entries(v, prim, is_skinned, part_idx):
                if is_skinned:
                    entries = []
                    for bi in range(4):
                        w = v.bone_weights[bi]
                        if w > 0:
                            local_idx = v.bone_indices[bi]
                            if prim.bone_table_length > 0 and local_idx < prim.bone_table_length:
                                skel_idx = prim.bone_table[local_idx]
                            else:
                                skel_idx = 0
                            entries.append((skel_idx, w))
                    if not entries:
                        entries.append((part_idx, 1.0))
                    return entries
                else:
                    return [(part_idx, 1.0)]

            def _bone_key(entries):
                """Build a stable dedup key for skinned per-vertex weights."""
                if not is_skinned:
                    return ()
                return tuple(
                    (int(bone_idx), round(float(weight), 6))
                    for bone_idx, weight in entries
                )

            def get_or_add_vert(pos, bone_data_entry, source_index=None):
                if is_skinned and source_index is not None:
                    # Preserve original vertex indexing for skinned meshes.
                    # Position-only dedup collapses distinct source vertices and
                    # breaks CDTA/COBJ parity on models like US01.
                    key = ("src", int(source_index))
                else:
                    key = (round(pos.x, 6), round(pos.y, 6), round(pos.z, 6),
                           _bone_key(bone_data_entry))
                if key in vert_map:
                    return vert_map[key]
                idx = len(all_vertices)
                all_vertices.append(pos)
                all_bone_data.append(bone_data_entry)
                vert_map[key] = idx
                return idx

            # Collect unique material indices for this part and ensure
            # materials are created.
            mat_idx_set = OrderedDict()
            for prim_idx in prim_indices:
                mi = int(lod.primitives[prim_idx].material_index)
                if mi not in mat_idx_set:
                    mat_idx_set[mi] = len(mat_idx_set)
                if mi not in self.material_dict:
                    if 0 <= mi < int(self.ir.material_count):
                        self.material_dict[mi] = self._create_material(self.ir.materials[mi])

            for prim_idx in prim_indices:
                prim = lod.primitives[prim_idx]
                idx_offset = int(prim.index_offset)
                idx_count = int(prim.index_count)
                vert_offset = int(prim.vertex_offset)
                prim_mat_idx = int(prim.material_index)

                if idx_count < 3:
                    continue

                for j in range(0, idx_count, 3):
                    i0 = int(lod.indices[idx_offset + j + 0]) + vert_offset
                    i1 = int(lod.indices[idx_offset + j + 2]) + vert_offset
                    i2 = int(lod.indices[idx_offset + j + 1]) + vert_offset

                    if i0 >= int(lod.vertex_count) or i1 >= int(lod.vertex_count) or i2 >= int(lod.vertex_count):
                        continue

                    v0 = lod.vertices[i0]
                    v1 = lod.vertices[i1]
                    v2 = lod.vertices[i2]

                    pos0 = render_space(Vector(v0.position) - part_abs)
                    pos1 = render_space(Vector(v1.position) - part_abs)
                    pos2 = render_space(Vector(v2.position) - part_abs)

                    bd0 = _bone_entries(v0, prim, is_skinned, part_idx)
                    bd1 = _bone_entries(v1, prim, is_skinned, part_idx)
                    bd2 = _bone_entries(v2, prim, is_skinned, part_idx)

                    vi0 = get_or_add_vert(pos0, bd0, i0)
                    vi1 = get_or_add_vert(pos1, bd1, i1)
                    vi2 = get_or_add_vert(pos2, bd2, i2)

                    # Skip degenerate faces created by vertex merging
                    if vi0 == vi1 or vi1 == vi2 or vi0 == vi2:
                        continue

                    all_faces.append((vi0, vi1, vi2))
                    all_face_mat_indices.append(prim_mat_idx)
                    # UV V-flip: Blender V=0 at bottom, game/DDS V=0 at top
                    all_uvs.extend([
                        (v0.uv0[0], 1.0 - v0.uv0[1]),
                        (v1.uv0[0], 1.0 - v1.uv0[1]),
                        (v2.uv0[0], 1.0 - v2.uv0[1]),
                    ])
                    all_normals.extend([
                        render_space(Vector(v0.normal)),
                        render_space(Vector(v1.normal)),
                        render_space(Vector(v2.normal)),
                    ])
                    all_uvs1.extend([
                        (v0.uv1[0], 1.0 - v0.uv1[1]),
                        (v1.uv1[0], 1.0 - v1.uv1[1]),
                        (v2.uv1[0], 1.0 - v2.uv1[1]),
                    ])

            if not all_vertices or not all_faces:
                continue

            mesh_name = f"{part_idx + 1:02d} Mesh0"
            mesh_data = bpy.data.meshes.new(mesh_name)

            mesh_data.from_pydata(all_vertices, [], all_faces)
            mesh_data.update()

            # Compute smoothing groups from per-loop normals so that
            # the vertex builder reproduces the correct normal splits.
            if all_normals and len(all_normals) == len(all_faces) * 3:
                sg_values = _compute_smoothing_groups(all_faces, all_normals)
            else:
                sg_values = [1] * len(all_faces)

            sg_attr = mesh_data.attributes.new('smoothing_group', 'INT', 'FACE')
            for fi in range(len(mesh_data.polygons)):
                sg_attr.data[fi].value = sg_values[fi] if fi < len(sg_values) else 1
                mesh_data.polygons[fi].use_smooth = True
            if all_normals and len(all_normals) == len(mesh_data.loops):
                mesh_data.normals_split_custom_set(all_normals)

            if all_uvs:
                uv_layer = mesh_data.uv_layers.new(name="UVMap")
                for loop_idx in range(len(mesh_data.loops)):
                    if loop_idx < len(all_uvs):
                        uv_layer.data[loop_idx].uv = all_uvs[loop_idx]

            if all_uvs1:
                uv_layer1 = mesh_data.uv_layers.new(name="UVMap_Lightmap")
                for loop_idx in range(len(mesh_data.loops)):
                    if loop_idx < len(all_uvs1):
                        uv_layer1.data[loop_idx].uv = all_uvs1[loop_idx]

            # Add all materials used by this part as mesh material slots.
            # mat_slot_map maps global mat_idx -> local slot index.
            mat_slot_map = {}
            for mi in mat_idx_set:
                slot_idx = len(mesh_data.materials)
                mat_slot_map[mi] = slot_idx
                if mi in self.material_dict:
                    mesh_data.materials.append(self.material_dict[mi])
                else:
                    mesh_data.materials.append(None)

            # Assign per-face material index
            for fi in range(len(mesh_data.polygons)):
                if fi < len(all_face_mat_indices):
                    global_mi = all_face_mat_indices[fi]
                    mesh_data.polygons[fi].material_index = mat_slot_map.get(global_mi, 0)

            mesh_obj = bpy.data.objects.new(mesh_name, mesh_data)
            bpy.context.collection.objects.link(mesh_obj)

            mesh_obj.parent = part_nodes[part_idx]

            mesh_obj["_part_index"] = part_idx
            if track_bone_data:
                self._mesh_bone_data[mesh_obj.name] = all_bone_data

            mesh_objects.append(mesh_obj)

        # Create empty mesh objects for parts with no geometry.
        # The original ASE always had mesh objects for all subobjects, even
        # empty ones.  This preserves subobjectCount in ConvertToInternal.
        for part_idx in sorted(part_nodes.keys()):
            if part_idx not in part_prims:
                mesh_name = f"{part_idx + 1:02d} Mesh0"
                mesh_data = bpy.data.meshes.new(mesh_name)
                mesh_data.from_pydata([], [], [])
                mesh_data.update()
                mesh_obj = bpy.data.objects.new(mesh_name, mesh_data)
                bpy.context.collection.objects.link(mesh_obj)
                mesh_obj.parent = part_nodes[part_idx]
                mesh_obj["_part_index"] = part_idx
                mesh_objects.append(mesh_obj)

        return mesh_objects

    def _build_additional_lods(self, name: str):
        """Import LODs 1+ as separate root hierarchies, hidden by default."""
        if self.ir.lod_count <= 1:
            return

        # Build material name manifest for _material_names property
        mat_names = []
        for i in range(int(self.ir.material_count)):
            mat = self.ir.materials[i]
            shader = mat.shader_name.decode("utf-8", errors="replace").rstrip("\x00")
            if not shader:
                shader = "FF_ST_OP"
            mat_names.append(f"Material_{i}_{shader}")
        mat_names_str = ";".join(mat_names)

        # Store on LOD 0 root too
        if self.root_object:
            self.root_object["_material_names"] = mat_names_str

        for lod_idx in range(1, int(self.ir.lod_count)):
            lod = self.ir.lods[lod_idx]
            num_parts = int(lod.part_count)
            if num_parts == 0:
                continue

            # Create LOD root empty
            lod_root_name = f"{name}_LOD{lod_idx}"
            lod_root = bpy.data.objects.new(lod_root_name, None)
            lod_root.empty_display_type = 'PLAIN_AXES'
            lod_root.empty_display_size = 0.1
            bpy.context.collection.objects.link(lod_root)
            lod_root["_lod_index"] = lod_idx
            lod_root["_material_names"] = mat_names_str
            lod_root.hide_viewport = True

            # Build part hierarchy for this LOD
            lod_part_nodes = {}
            for i in range(num_parts):
                part_name = f"PN{i + 1:02d}"
                part_obj = bpy.data.objects.new(part_name, None)
                part_obj.empty_display_type = 'PLAIN_AXES'
                part_obj.empty_display_size = 0.05
                bpy.context.collection.objects.link(part_obj)
                lod_part_nodes[i] = part_obj

            for i in range(num_parts):
                part = lod.parts[i]
                part_obj = lod_part_nodes[i]

                abs_pos = render_space(Vector(part.abs_position))
                rel_pos = render_space(Vector(part.rel_position))

                parent = lod_root
                if (part.parent_index >= 0 and
                    part.parent_index < num_parts and
                    part.parent_index != i):
                    parent = lod_part_nodes[part.parent_index]

                part_obj.parent = parent
                if parent == lod_root:
                    part_obj.location = abs_pos
                else:
                    part_obj.location = rel_pos

            # Create meshes for this LOD (shared material_dict)
            lod_meshes = self._create_meshes_for_lod(
                lod, lod_part_nodes, track_bone_data=False)

            # Create center markers for each part (mirrors create_scene_markers
            # for LOD 0).  The original OED tool derives per-LOD center axes
            # from each LOD's own _XX center objects.
            for i in range(num_parts):
                if i not in lod_part_nodes:
                    continue
                center_mesh = bpy.data.meshes.new(f"_{i + 1:02d} center")
                center_obj = bpy.data.objects.new(f"_{i + 1:02d} center", center_mesh)
                create_cube_mesh(center_mesh, 0.015)
                bpy.context.collection.objects.link(center_obj)
                center_obj.parent = lod_part_nodes[i]

                # Apply rotation from MTRX if available.
                if (i < int(lod.part_animation_count) and
                        int(self.ir.matrix_count) > 0):
                    mi = lod.part_animations[i].matrix_index
                    if mi != 0xFF and mi < int(self.ir.matrix_count):
                        rot = _mtrx_to_center_rotation(self.ir.matrices[mi].m)
                        if rot is not None:
                            center_obj.matrix_local = rot
                    else:
                        # Preserve "no matrix" centers so export can emit a
                        # PANM matrix index of 0xFF.
                        center_obj["opennova_zero_axis"] = True

            # Create attach point markers (~XXx attach) for child parts
            pi_counter: dict[int, int] = {}
            for i in range(num_parts):
                if i not in lod_part_nodes:
                    continue
                part = lod.parts[i]
                pi = int(part.parent_index)
                if i == 0 or pi < 0:
                    continue
                count = pi_counter.get(pi, 0)
                pi_counter[pi] = count + 1
                suffix = chr(ord('a') + (count % 26))
                attach_name = f"~{pi + 1:02d}{suffix} attach"
                attach_mesh = bpy.data.meshes.new(attach_name)
                attach_obj = bpy.data.objects.new(attach_name, attach_mesh)
                create_cube_mesh(attach_mesh, 0.012)
                bpy.context.collection.objects.link(attach_obj)
                attach_obj.parent = lod_part_nodes[i]

            print(f"[LOD] {lod_root_name}: {num_parts} parts, "
                  f"{len(lod_meshes)} meshes")
