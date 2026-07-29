"""Scene overlays: center/attach markers, user points, lights, and the
collision / occlusion visualization meshes.

Mixin part of BlenderSceneBuilder -- instance state lives in core.__init__.
"""

from __future__ import annotations

import bpy
from mathutils import Vector, Matrix

from blender.math_utils import (
    render_space,
    collision_space,
    compute_polyhedron_controlled,
)
from blender.mesh_primitives import create_cube_mesh
from pyopennova.scene_naming import (
    CollisionType,
    OcclusionType,
    build_occlusion_name as _build_occlusion_name,
    build_volume_name as _build_volume_name,
    format_duplicate_suffix as _format_duplicate_suffix,
)

from .helpers import _mtrx_to_center_rotation


class OverlaysMixin:
    def create_scene_markers(self):
        """Create center and attachment point markers, parented to part nodes."""
        if self.ir.lod_count == 0:
            return
        lod0 = self.ir.lods[0]

        for i in range(int(lod0.part_count)):
            if i not in self.part_nodes:
                continue
            part_node = self.part_nodes[i]

            # Center point (child of part node, at origin = part center)
            center_mesh = bpy.data.meshes.new(f"_{i + 1:02d} center")
            center_obj = bpy.data.objects.new(f"_{i + 1:02d} center", center_mesh)
            create_cube_mesh(center_mesh, 0.015)
            center_obj.data.materials.append(self._create_marker_material("center_magenta", (1.0, 0.0, 1.0)))
            bpy.context.collection.objects.link(center_obj)
            center_obj.parent = part_node

            # Apply rotation from MTRX if available (preserves center axis
            # through the Blender → ASE → convert_internal roundtrip).
            if (i < int(lod0.part_animation_count) and
                    int(self.ir.matrix_count) > 0):
                mi = lod0.part_animations[i].matrix_index
                if mi != 0xFF and mi < int(self.ir.matrix_count):
                    rot = _mtrx_to_center_rotation(self.ir.matrices[mi].m)
                    if rot is not None:
                        center_obj.matrix_local = rot
                else:
                    # Preserve "no matrix" centers so export can emit a
                    # PANM matrix index of 0xFF.
                    center_obj["opennova_zero_axis"] = True

        # Create tilde attachment point objects (~01a, ~01b, etc.)
        num_parts = int(lod0.part_count)
        pi_counter: dict[int, int] = {}
        for i in range(num_parts):
            if i not in self.part_nodes:
                continue
            part = lod0.parts[i]
            pi = int(part.parent_index)

            # Skip root parts (no parent to attach to)
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
            attach_obj.parent = self.part_nodes[i]

    def create_user_points(self):
        """Create user point markers, localized to parent part space."""
        if self.ir.lod_count == 0:
            return
        lod0 = self.ir.lods[0]

        # Ensure world matrices are computed before reading parent positions.
        bpy.context.view_layer.update()

        for i in range(int(self.ir.userpoint_count)):
            up = self.ir.userpoints[i]
            name = up.name.decode("utf-8", errors="replace").rstrip("\x00")

            # Build display name from type code
            display_idx = up.part_index if up.part_index >= 0 else 0
            # Build display name: always use "UP{type_char}" so classify_name
            # recognizes it as objType=6.
            type_code = up.type_code
            if type_code and 32 <= type_code <= 126:
                prefix = f"UP{chr(type_code)}"
            else:
                prefix = "USR"
            marker_name = f"{prefix}{display_idx + 1:02d}"
            if name:
                marker_name += f" {name}"

            marker_mesh = bpy.data.meshes.new(marker_name)
            marker_obj = bpy.data.objects.new(marker_name, marker_mesh)
            create_cube_mesh(marker_mesh, 0.015)
            marker_obj.data.materials.append(self._create_marker_material("user_point_white", (1.0, 1.0, 1.0)))
            bpy.context.collection.objects.link(marker_obj)

            # Position: IR stores (3di.y, 3di.z, 3di.x) in internal/swizzled
            # space.  Map to Blender via internal→render→render_space:
            # blender = (ir[0], -ir[2], ir[1]).
            raw = Vector(up.position)
            up_pos = Vector((raw[0], -raw[2], raw[1]))
            if up.part_index >= 0 and up.part_index < len(self.part_nodes):
                parent_node = self.part_nodes[up.part_index]
                up_pos = up_pos - Vector(parent_node.matrix_world.translation)

            marker_obj.location = up_pos

            # Direction: same coordinate mapping as position.
            raw_dir = Vector(up.direction)
            z_axis = Vector((raw_dir[0], -raw_dir[2], raw_dir[1])).normalized()
            if z_axis.length_squared > 1e-6:
                # Construct orthonormal basis from Z-axis direction
                world_up = Vector((0.0, 0.0, 1.0))
                if abs(z_axis.dot(world_up)) > 0.999:
                    world_up = Vector((0.0, 1.0, 0.0))
                x_axis = world_up.cross(z_axis).normalized()
                y_axis = z_axis.cross(x_axis).normalized()
                rot_mat = Matrix((
                    (x_axis.x, y_axis.x, z_axis.x),
                    (x_axis.y, y_axis.y, z_axis.y),
                    (x_axis.z, y_axis.z, z_axis.z),
                )).to_3x3()
                marker_obj.rotation_euler = rot_mat.to_euler()

            parent_node = self.root_object
            if up.part_index >= 0 and up.part_index < len(self.part_nodes):
                parent_node = self.part_nodes[up.part_index]
            marker_obj.parent = parent_node

            marker_obj["nl_ase_name"] = marker_name  # original before Blender dedup
            marker_obj["nl_raw_position"] = tuple(up.position)
            marker_obj["nl_raw_direction"] = tuple(up.direction)
            marker_obj["nl_parent_index"] = int(up.part_index)
            marker_obj["nl_type_code"] = int(up.type_code)

    def create_scene_lights(self):
        """Create light objects from the IR's light data."""
        for i in range(int(self.ir.light_count)):
            light = self.ir.lights[i]
            light_idx = light.part_index if light.part_index >= 0 else i
            light_name = f"LP{light_idx + 1:02d}"

            light_data = bpy.data.lights.new(name=f"{light_name}_data", type="POINT")
            light_obj = bpy.data.objects.new(light_name, light_data)

            light_data.color = (light.color_start[0], light.color_start[1], light.color_start[2])
            light_data.energy = 1.0
            if light.attenuation_end > 0:
                light_data.use_custom_distance = True
                light_data.cutoff_distance = light.attenuation_end
            light_data.use_shadow = False

            bpy.context.collection.objects.link(light_obj)

            # Convert position to Blender space
            light_obj.location = render_space(Vector(light.offset))

            # Store light fields as custom properties for round-trip export
            if light.attenuation_start > 0:
                light_obj["atten_start"] = float(light.attenuation_start)
            if light.falloff > 0:
                light_obj["falloff"] = float(light.falloff)
            rot_bl = render_space(Vector((light.rotation[0],
                                        light.rotation[1],
                                        light.rotation[2])))
            light_obj["tm_row2"] = [rot_bl.x, rot_bl.y, rot_bl.z]
            if light.light_type != 0:
                light_obj["light_type"] = int(light.light_type)

            # Store light colorgen fields as custom properties for 3dp round-trip
            if light.style != 0:
                light_obj["colorgen_style"] = int(light.style)
                light_obj["colorgen_rate"] = float(light.rate) / 256.0
                light_obj["colorgen_phase"] = float(light.phase) / 256.0
            # color_end (color_start is already stored in light_data.color)
            ce = light.color_end
            if ce[0] != 0.0 or ce[1] != 0.0 or ce[2] != 0.0:
                light_obj["colorgen_end"] = [
                    min(255, int(ce[0] * 255.0)),
                    min(255, int(ce[1] * 255.0)),
                    min(255, int(ce[2] * 255.0)),
                ]
            # Control register for colorgen (style > 0x70 means phase is reg index)
            if light.style > 0x70:
                creg = self._resolve_ctrl_reg(int(light.phase))
                if creg:
                    light_obj["colorgen_ctrlreg"] = creg
            # Light disable flags
            flags = int(light.flags)
            if flags & 0x01:
                light_obj["disable_corona"] = 1
            if flags & 0x02:
                light_obj["disable_lightterrain"] = 1
            if flags & 0x04:
                light_obj["disable_lightobjects"] = 1

            parent_node = self.root_object
            if light.part_index >= 0 and light.part_index in self.part_nodes:
                parent_node = self.part_nodes[light.part_index]
            light_obj.parent = parent_node

    def create_collision_visualization(self):
        coll = self.ir.collision
        if not coll:
            return

        lod0 = self.ir.lods[0] if self.ir.lod_count > 0 else None
        part_count = int(lod0.part_count) if lod0 else 0
        name_counters = {}

        for vol_idx in range(int(coll.contents.volume_count)):
            vol = coll.contents.volumes[vol_idx]
            try:
                color = CollisionType(vol.type).color
            except ValueError:
                color = (1.0, 1.0, 1.0)

            # Determine parent node and parent position
            parent = self.root_object
            parent_pos = Vector((0, 0, 0))
            parent_set = False
            if vol.object_index >= 0 and vol.object_index < part_count:
                parent = self.part_nodes.get(vol.object_index, self.root_object)
                part = lod0.parts[vol.object_index]
                parent_pos = render_space(Vector(part.abs_position))
                parent_set = True
            if not parent_set and vol.part_index >= 0 and vol.part_index < part_count:
                parent = self.part_nodes.get(vol.part_index, self.root_object)
                part = lod0.parts[vol.part_index]
                parent_pos = render_space(Vector(part.abs_position))

            # Compute world center and local offset
            world_min = collision_space(Vector(vol.min))
            world_max = collision_space(Vector(vol.max))
            world_center = (world_min + world_max) * 0.5
            local_center = world_center - parent_pos

            # Build name: type + object_index + duplicate suffix
            name_index = vol.object_index if vol.object_index >= 0 else vol.part_index
            name_key = name_index if name_index >= 0 else -1
            counter_key = (vol.type, name_key)
            name_counters[counter_key] = name_counters.get(counter_key, 0) + 1
            occurrence = name_counters[counter_key]
            mesh_name = _build_volume_name(vol.type, vol.flags, name_index, occurrence) + "-colonly"

            # Gather planes (respecting plane_start/plane_count bounds)
            hs_list = []
            if (vol.plane_count > 0 and vol.plane_start >= 0 and
                    vol.plane_start + vol.plane_count <= int(coll.contents.plane_count)):
                for p_idx in range(vol.plane_count):
                    gp_idx = vol.plane_start + p_idx
                    plane = coll.contents.planes[gp_idx]
                    n = collision_space(Vector(plane.normal))
                    hs_list.append([n.x, n.y, n.z, plane.distance])

            min_pt = collision_space(Vector(vol.min))
            max_pt = collision_space(Vector(vol.max))

            try:
                hull_ok = False
                if hs_list:
                    vertices, faces = compute_polyhedron_controlled(hs_list, min_pt, max_pt)
                    if vertices is not None and len(vertices) >= 4:
                        vertices = [v - world_center for v in vertices]
                        self._create_collision_mesh(
                            mesh_name, vertices, faces, color,
                            parent=parent, location=local_center,
                        )
                        hull_ok = True

                if not hull_ok:
                    # Fallback box from min/max bounds
                    half = (abs((world_max.x - world_min.x) * 0.5),
                            abs((world_max.y - world_min.y) * 0.5),
                            abs((world_max.z - world_min.z) * 0.5))
                    # Skip degenerate volumes where all dimensions are zero
                    if half[0] <= 0.0 and half[1] <= 0.0 and half[2] <= 0.0:
                        continue
                    # Box vertices: iterate s0 in (-1,1), s1 in (-1,1), s2 in (-1,1)
                    # Index layout: 0=(-,-,-) 1=(-,-,+) 2=(-,+,-) 3=(-,+,+)
                    #               4=(+,-,-) 5=(+,-,+) 6=(+,+,-) 7=(+,+,+)
                    box_pts = [
                        (s0 * half[0], s1 * half[1], s2 * half[2])
                        for s0 in (-1, 1) for s1 in (-1, 1) for s2 in (-1, 1)
                    ]
                    # Hardcoded 6 quad faces, fan-triangulated
                    box_faces = [
                        [0, 2, 3, 1],  # -X face
                        [4, 5, 7, 6],  # +X face
                        [0, 1, 5, 4],  # -Y face
                        [2, 6, 7, 3],  # +Y face
                        [0, 4, 6, 2],  # -Z face
                        [1, 3, 7, 5],  # +Z face
                    ]
                    self._create_collision_mesh(
                        mesh_name, box_pts, box_faces, color,
                        parent=parent, location=local_center,
                    )
            except Exception as e:
                print(f"Failed to create collision mesh {mesh_name}: {e}")

    def _create_collision_mesh(self, name, vertices, polygon_faces, color,
                               parent=None, location=None):
        """Create a collision mesh with shared vertices.

        polygon_faces: list of polygon faces (each a list of vertex indices).
        Fan-triangulated using the shared vertex indices directly.
        """
        mesh_data = bpy.data.meshes.new(name)
        mesh_obj = bpy.data.objects.new(name, mesh_data)
        # Build shared vertex list
        verts = [tuple(vertices[i]) for i in range(len(vertices))]
        # Fan-triangulate each polygon face using shared vertex indices
        tri_faces = []
        for face in polygon_faces:
            for i in range(1, len(face) - 1):
                tri_faces.append((face[0], face[i], face[i + 1]))
        mesh_data.from_pydata(verts, [], tri_faces)
        mesh_data.update()

        # Store constant smoothing group for ASE export (matches reference)
        sg_attr = mesh_data.attributes.new('smoothing_group', 'INT', 'FACE')
        for j in range(len(tri_faces)):
            sg_attr.data[j].value = 1

        bpy.context.collection.objects.link(mesh_obj)

        if parent is not None:
            mesh_obj.parent = parent
        if location is not None:
            mesh_obj.location = location

        mat_name = f"Collision_{name}"
        mat = bpy.data.materials.get(mat_name)
        if mat is None:
            mat = bpy.data.materials.new(mat_name)
            mat.diffuse_color = (*color, 0.5)
        mesh_data.materials.append(mat)
        mesh_obj.display_type = "SOLID"
        mesh_obj.color = (*color, 0.7)

    def create_occlusion_visualization(self):
        """Create occlusion meshes from the IR's occlusion data."""
        occ = self.ir.occlusion
        if not occ or int(occ.contents.object_count) == 0:
            return

        if self.ir.lod_count == 0:
            return
        lod0 = self.ir.lods[0]

        occ_counters = {}
        for i in range(int(occ.contents.object_count)):
            occ_obj = occ.contents.objects[i]
            if occ_obj.num_vertices <= 0 or occ_obj.face_count <= 0:
                continue

            vert_start = occ_obj.vertex_start
            face_start = occ_obj.face_start
            vert_count = occ_obj.num_vertices
            face_count_val = occ_obj.face_count

            if vert_start + vert_count > int(occ.contents.vertex_count):
                continue
            if face_start + face_count_val > int(occ.contents.face_count):
                continue

            # Compute parent abs position for localization
            occ_parent = occ_obj.parent_subobject_index
            part_abs = Vector((0, 0, 0))
            if occ_parent >= 0 and occ_parent < int(lod0.part_count):
                parent_part = lod0.parts[occ_parent]
                part_abs = render_space(Vector(parent_part.abs_position))

            # Vertices: transform_position then subtract parent abs
            vertices = []
            for j in range(vert_count):
                v = occ.contents.vertices[vert_start + j]
                pos = render_space(Vector(v.position))
                vertices.append(pos - part_abs)

            # Faces: unpack raw_indices as 3 x uint8
            faces = []
            face_flags = []
            for j in range(face_count_val):
                face = occ.contents.faces[face_start + j]
                raw = face.raw_indices
                v1 = raw & 0xFF
                v2 = (raw >> 8) & 0xFF
                v3 = (raw >> 16) & 0xFF
                if v1 < len(vertices) and v2 < len(vertices) and v3 < len(vertices):
                    faces.append([v1, v2, v3])
                    face_flags.append(((raw >> 24) & 0xFF) + 1)

            if not vertices or not faces:
                continue

            base_name = _build_occlusion_name(occ_obj.type, occ_obj.parent_subobject_index, occ_obj.connecting_subobject)
            key = (occ_obj.type, int(occ_obj.parent_subobject_index), int(occ_obj.connecting_subobject))
            occ_counters[key] = occ_counters.get(key, 0) + 1
            mesh_name = base_name + _format_duplicate_suffix(occ_counters[key]) + "-occonly"
            mesh_data = bpy.data.meshes.new(mesh_name)
            bl_obj = bpy.data.objects.new(mesh_name, mesh_data)

            mesh_data.from_pydata(vertices, [], faces)
            mesh_data.update()

            # Store face_flag as smoothing group for ASE export
            sg_attr = mesh_data.attributes.new('smoothing_group', 'INT', 'FACE')
            for j, sg_val in enumerate(face_flags):
                sg_attr.data[j].value = sg_val

            bpy.context.collection.objects.link(bl_obj)

            try:
                occ_color = OcclusionType(occ_obj.type).color
            except ValueError:
                occ_color = (1.0, 0.0, 0.0)

            mat_name = f"Occlusion_{mesh_name}"
            mat = bpy.data.materials.get(mat_name)
            if mat is None:
                mat = bpy.data.materials.new(mat_name)
                mat.diffuse_color = (*occ_color, 0.25)
            mesh_data.materials.append(mat)
            bl_obj.display_type = "SOLID"
            bl_obj.color = (*occ_color, 0.25)

            parent_node = self.root_object
            if occ_parent >= 0 and occ_parent in self.part_nodes:
                parent_node = self.part_nodes[occ_parent]
            bl_obj.parent = parent_node
