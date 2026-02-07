"""
Blender ASE Exporter - ASCII Scene Export
Based on 3ds Max ASCII Scene Export format

This module provides functionality to export Blender scenes to ASE format,
maintaining compatibility with the 3ds Max ASE specification.
"""

from __future__ import annotations

import bpy
import bmesh
import os
import time
import traceback
from mathutils import Vector
from . import ase_tokens as tokens


class MaterialKeeper:
    """Material manager similar to 3ds Max MtlKeeper (flat list of materials)."""

    def __init__(self):
        self.materials = []
        self.material_map = {}
        self.object_material_map = {}

    def add_material(self, material):
        """Register a material if it's not already present. Returns material index."""
        if material is None:
            return -1
        if material in self.material_map:
            return self.material_map[material]
        idx = len(self.materials)
        self.materials.append(material)
        self.material_map[material] = idx
        return idx

    def add_object_materials(self, obj):
        """Register materials for a mesh object and store its material ref."""
        if not obj.data.materials:
            self.object_material_map[obj.name] = -1
            return -1

        # Add all slot materials to the global list
        for mat in obj.data.materials:
            if mat is not None:
                self.add_material(mat)

        # Use the active material (or first non-None) as the object's material ref
        mat = obj.active_material
        if mat is None:
            for slot in obj.data.materials:
                if slot is not None:
                    mat = slot
                    break

        idx = self.add_material(mat) if mat is not None else -1
        self.object_material_map[obj.name] = idx
        return idx

    def get_object_material_id(self, obj):
        return self.object_material_map.get(obj.name, -1)

    def count(self):
        return len(self.materials)

    def get_material(self, idx):
        if 0 <= idx < len(self.materials):
            return self.materials[idx]
        return None


class AseExporter:
    """Main ASE exporter class"""

    def __init__(self):
        # Export options
        self.include_mesh = True
        self.include_materials = True
        self.include_animations = False
        self.include_normals = True
        self.include_uvs = True
        self.include_vertex_colors = False
        self.include_bone_weights = True
        self.export_textures = True
        self.precision = 4
        self.scale = 1.0

        # Internal state
        self.file_stream = None
        self.material_keeper = MaterialKeeper()
        self.float_format = f"%.{self.precision}f"
        self.scene = None
        self._texture_queue = {}  # bitmap_filename -> bpy.types.Image
        self._used_tex_names = {}  # lowercase short name -> original short name (dedup)

        # Node processing
        self.total_node_count = 0

    def set_options(self, **options):
        """Set export options"""
        for key, value in options.items():
            if hasattr(self, key):
                setattr(self, key, value)

        # Update float format
        self.float_format = f"%.{self.precision}f"

    def export_scene(self, scene, filepath):
        """Export scene to ASE file"""
        self.scene = scene

        print(f"ASE Export: Starting export to {filepath}")

        try:
            # Validate filepath
            if not filepath:
                raise ValueError("No output filepath specified")

            # Check if directory exists
            output_dir = os.path.dirname(filepath)
            if output_dir and not os.path.exists(output_dir):
                print(f"ASE Export: Creating directory {output_dir}")
                os.makedirs(output_dir, exist_ok=True)

            print(f"ASE Export: Opening file for writing")
            with open(filepath, 'w', encoding='utf-8') as f:
                self.file_stream = f

                print(f"ASE Export: Preprocessing scene")
                # Count nodes for progress
                self.preprocess_scene()
                print(f"ASE Export: Found {self.total_node_count} exportable objects")

                print(f"ASE Export: Exporting global info")
                # Export all components
                self.export_global_info()

                if self.include_materials:
                    print(f"ASE Export: Exporting {self.material_keeper.count()} materials")
                    self.export_material_list()

                print(f"ASE Export: Exporting scene objects")
                # Export scene objects
                self.export_scene_objects()

            # Save texture files after the ASE is written
            if self.export_textures and self._texture_queue:
                self._save_textures(os.path.dirname(filepath) or '.')

            print(f"ASE Export: Export completed successfully")
            return True

        except PermissionError as e:
            error_msg = f"Permission denied writing to {filepath}: {str(e)}"
            print(f"ASE Export Error: {error_msg}")
            raise Exception(error_msg)

        except OSError as e:
            error_msg = f"File system error: {str(e)}"
            print(f"ASE Export Error: {error_msg}")
            raise Exception(error_msg)

        except Exception as e:
            error_msg = f"ASE Export failed: {str(e)}"
            print(f"ASE Export Error: {error_msg}")
            print("Full traceback:")
            traceback.print_exc()
            raise Exception(error_msg)

        finally:
            self.file_stream = None

    def preprocess_scene(self):
        """Count nodes and prepare materials"""
        self.total_node_count = 0

        # Count exportable objects
        for obj in self.scene.objects:
            if self.is_exportable_object(obj):
                self.total_node_count += 1

                # Register object materials (creates multi/sub-object groups)
                if obj.type == 'MESH' and obj.data.materials:
                    self.material_keeper.add_object_materials(obj)

    def is_exportable_object(self, obj):
        """Check if object should be exported"""
        if obj.type == 'MESH':
            return True
        elif obj.type == 'CAMERA':
            return True
        elif obj.type == 'LIGHT':
            return True
        elif obj.type == 'CURVE':
            return True
        elif obj.type == 'EMPTY':  # Helper objects
            return True
        return False

    def export_global_info(self):
        """Export global scene information"""
        # File header
        self.write_line(f"{tokens.ID_FILEID}\t{tokens.VERSION}")

        # Comment with timestamp (match 3ds Max AsciiExport format)
        timestamp = time.strftime("%a %b %d %H:%M:%S %Y")
        version_num = tokens.VERSION / 100.0
        self.write_line(f'{tokens.ID_COMMENT} "AsciiExport Version  {version_num:.2f} - {timestamp}"')

        # Scene block
        self.write_line(f"{tokens.ID_SCENE} {{")

        # Scene filename
        scene_filename = os.path.basename(bpy.data.filepath) if bpy.data.filepath else "untitled.blend"
        self.write_line(f'\t{tokens.ID_FILENAME} "{self.fixup_name(scene_filename)}"')

        # Frame range
        frame_start = self.scene.frame_start
        frame_end = self.scene.frame_end
        frame_rate = self.scene.render.fps

        self.write_line(f"\t{tokens.ID_FIRSTFRAME} {frame_start}")
        self.write_line(f"\t{tokens.ID_LASTFRAME} {frame_end}")
        self.write_line(f"\t{tokens.ID_FRAMESPEED} {frame_rate}")
        self.write_line(f"\t{tokens.ID_TICKSPERFRAME} {160}")  # 3ds Max standard

        # Background color (static for now)
        world = self.scene.world
        if world and hasattr(world, 'color'):
            bg_color = world.color
            self.write_line(f"\t{tokens.ID_STATICBGCOLOR} {self.format_color(bg_color)}")
        else:
            self.write_line(f"\t{tokens.ID_STATICBGCOLOR} {self.format_point3((0.0, 0.0, 0.0))}")

        # Ambient light (static for now)
        self.write_line(f"\t{tokens.ID_STATICAMBIENT} {self.format_point3((0.0, 0.0, 0.0))}")

        self.write_line("}")  # End scene

    def export_material_list(self):
        """Export material list"""
        material_count = self.material_keeper.count()

        if material_count == 0:
            return

        self.write_line(f"{tokens.ID_MATERIAL_LIST} {{")
        self.write_line(f"\t*MATERIAL_COUNT {material_count}")

        for i in range(material_count):
            material = self.material_keeper.get_material(i)
            self.export_material(material, i, 1)

        self.write_line("}")  # End material list

    def export_material(self, material, material_id, indent_level):
        """Export a Standard material (matches 3ds Max output)"""
        indent = self.get_indent(indent_level)

        self.write_line(f"{indent}{tokens.ID_MATERIAL} {material_id} {{")

        mat_name = material.get("ase_material_name", material.name) if material else f"Material_{material_id}"
        self.write_line(f"{indent}\t{tokens.ID_MATERIAL_NAME} \"{mat_name}\"")
        self.write_line(f"{indent}\t{tokens.ID_MATERIAL_CLASS} \"Standard (Legacy)\"")

        if material:
            self._export_standard_material_properties(material, indent_level)

        self.write_line(f"{indent}}}")  # End material
    def _export_standard_material_properties(self, material, indent_level):
        """Export Standard (Legacy) material properties to match 3ds Max."""
        indent = self.get_indent(indent_level)

        # Default values based on 3ds Max Standard material defaults
        ambient = (0.588, 0.588, 0.588)
        diffuse = (1.0, 1.0, 1.0)
        if hasattr(material, 'diffuse_color'):
            diffuse = material.diffuse_color[:3]
        specular = (0.9, 0.9, 0.9)
        shine = 0.1
        shine_strength = 0.0

        transparency = 0.0
        if hasattr(material, 'diffuse_color') and len(material.diffuse_color) > 3:
            transparency = max(0.0, min(1.0, 1.0 - material.diffuse_color[3]))

        self.write_line(f"{indent}\t{tokens.ID_MATERIAL_AMBIENT} {self.format_color(ambient)}")
        self.write_line(f"{indent}\t{tokens.ID_MATERIAL_DIFFUSE} {self.format_color(diffuse)}")
        self.write_line(f"{indent}\t{tokens.ID_MATERIAL_SPECULAR} {self.format_color(specular)}")
        self.write_line(f"{indent}\t{tokens.ID_MATERIAL_SHINE} {self.format_float(shine)}")
        self.write_line(f"{indent}\t{tokens.ID_MATERIAL_SHINESTRENGTH} {self.format_float(shine_strength)}")
        self.write_line(f"{indent}\t{tokens.ID_MATERIAL_TRANSPARENCY} {self.format_float(transparency)}")
        self.write_line(f"{indent}\t{tokens.ID_MATERIAL_WIRESIZE} {self.format_float(1.0)}")
        self.write_line(f"{indent}\t{tokens.ID_MATERIAL_SHADING} {tokens.SHADING_BLINN}")
        self.write_line(f"{indent}\t{tokens.ID_MATERIAL_XP_FALLOFF} {self.format_float(0.0)}")
        self.write_line(f"{indent}\t{tokens.ID_MATERIAL_SELFILLUM} {self.format_float(0.0)}")
        self.write_line(f"{indent}\t{tokens.ID_MATERIAL_FALLOFF} In")
        self.write_line(f"{indent}\t{tokens.ID_MATERIAL_XP_TYPE} Filter")

        # Export texture maps
        self.export_material_textures(material, indent_level + 1)

    # OED MaterialTexSlot.path is char[16] (15 usable chars + null).
    # copy_str(dst, 16, src) uses strncpy(dst, src, 15) so texture filenames
    # (including extension) must be at most 15 characters to survive the
    # OED's ensure_bucket -> 3DI export pipeline without truncation.
    MAX_TEX_FILENAME = 15

    def _fit_texture_name(self, name):
        """Shorten a texture filename to fit OED's 15-char limit.

        Truncates the stem, preserving the extension.  Appends a digit
        suffix when truncation would create a duplicate.

        Returns the (possibly shortened) filename.
        """
        if not name:
            return name
        if len(name) <= self.MAX_TEX_FILENAME:
            self._used_tex_names[name.lower()] = name.lower()
            return name

        root, ext = os.path.splitext(name)
        max_stem = self.MAX_TEX_FILENAME - len(ext)

        candidate = root[:max_stem] + ext
        key = candidate.lower()

        # Deduplicate: if this short name is already taken by a DIFFERENT
        # original name, shorten further and append a digit.
        if key in self._used_tex_names and self._used_tex_names[key] != name.lower():
            for i in range(2, 100):
                suffix = str(i)
                candidate = root[:max_stem - len(suffix)] + suffix + ext
                key = candidate.lower()
                if key not in self._used_tex_names:
                    break

        self._used_tex_names[key] = name.lower()
        if candidate != name:
            print(f"ASE Export: Texture name shortened: '{name}' -> '{candidate}'")
        return candidate

    def export_material_textures(self, material, indent_level):
        """Export material texture maps.

        Uses stored custom properties (ase_diffuse_bitmap, ase_opacity_bitmap)
        from the importer for round-trip fidelity.  Falls back to inspecting
        the Blender node tree when those properties are absent.
        """
        indent = self.get_indent(indent_level)

        # --- Resolve diffuse bitmap path and image object ---
        diffuse_bitmap = material.get("ase_diffuse_bitmap", "")
        diffuse_image = None

        # If no stored name, try to extract from Blender node tree
        if not diffuse_bitmap and material.use_nodes and material.node_tree:
            diffuse_bitmap, diffuse_image = self._find_bitmap_from_nodes(material)

        # Fix texture extensions for df4oed compatibility.
        # df4oed only supports TGA/PCX/MDT in ASE files.
        # DDS textures are auto-loaded as fallback, so we map .dds → .tga
        def fix_ext(name):
            if not name:
                return name
            name = os.path.basename(name)
            root, ext = os.path.splitext(name)
            ext_upper = ext.upper()
            if ext_upper in ('.TGA', '.PCX', '.MDT'):
                return root + ext_upper
            return root + '.TGA'

        diffuse_bitmap = fix_ext(diffuse_bitmap)

        # Fit to OED's 15-char filename limit
        diffuse_bitmap = self._fit_texture_name(diffuse_bitmap)

        # Queue the image for TGA export
        if diffuse_bitmap and diffuse_image:
            self._texture_queue[diffuse_bitmap] = diffuse_image

        def write_map_block(map_token, bitmap_name, subno, amount):
            if not bitmap_name:
                return
            self.write_line(f"{indent}{map_token} {{")
            self.write_line(f"{indent}\t{tokens.ID_MAP_NAME} \"{bitmap_name.lower()}\"")
            self.write_line(f"{indent}\t{tokens.ID_MAP_CLASS} \"Bitmap\"")
            self.write_line(f"{indent}\t{tokens.ID_MAP_SUBNO} {subno}")
            self.write_line(f"{indent}\t{tokens.ID_MAP_AMOUNT} {self.format_float(amount)}")
            self.write_line(f"{indent}\t{tokens.ID_BITMAP} \"{bitmap_name}\"")
            self.write_line(f"{indent}\t{tokens.ID_MAP_TYPE} Screen")
            self.write_line(f"{indent}\t{tokens.ID_UVW_U_OFFSET} {self.format_float(0.0)}")
            self.write_line(f"{indent}\t{tokens.ID_UVW_V_OFFSET} {self.format_float(0.0)}")
            self.write_line(f"{indent}\t{tokens.ID_UVW_U_TILING} {self.format_float(1.0)}")
            self.write_line(f"{indent}\t{tokens.ID_UVW_V_TILING} {self.format_float(1.0)}")
            self.write_line(f"{indent}\t{tokens.ID_UVW_ANGLE} {self.format_float(0.0)}")
            self.write_line(f"{indent}\t{tokens.ID_UVW_BLUR} {self.format_float(1.0)}")
            self.write_line(f"{indent}\t{tokens.ID_UVW_BLUR_OFFSET} {self.format_float(0.0)}")
            self.write_line(f"{indent}\t{tokens.ID_UVW_NOUSE_AMT} {self.format_float(1.0)}")
            self.write_line(f"{indent}\t{tokens.ID_UVW_NOISE_SIZE} {self.format_float(1.0)}")
            self.write_line(f"{indent}\t{tokens.ID_UVW_NOISE_LEVEL} 1")
            self.write_line(f"{indent}\t{tokens.ID_UVW_NOISE_PHASE} {self.format_float(0.0)}")
            self.write_line(f"{indent}\t{tokens.ID_BITMAP_FILTER} Pyramidal")
            self.write_line(f"{indent}}}")

        write_map_block(tokens.ID_MAP_DIFFUSE, diffuse_bitmap, 1, 1.0)
        # NOTE: Do NOT export ase_opacity_bitmap as MAP_OPACITY.
        # The "opacity" slot is actually DETAIL/lightmap textures (slot 2 in 3DI),
        # not true alpha masks. Exporting MAP_OPACITY triggers a crash in df4oed's
        # material preview dialog (sub_403750) due to an uninitialized alpha buffer
        # pointer when alpha_mode == 2 but the TGA loading path isn't taken.

    def _find_bitmap_from_nodes(self, material):
        """Extract a bitmap path and Image object from the Blender shader node tree.

        Returns (path_string, bpy.types.Image or None).
        """
        def find_image_from_socket(socket, visited=None):
            if not socket or not socket.is_linked:
                return None
            return find_image_from_node(socket.links[0].from_node, visited)

        def find_image_from_node(node, visited=None):
            if visited is None:
                visited = set()
            if id(node) in visited:
                return None
            visited.add(id(node))
            if node.type == 'TEX_IMAGE' and node.image:
                return node.image
            if node.type == 'NORMAL_MAP':
                return find_image_from_socket(node.inputs.get('Color'), visited)
            for inp in node.inputs:
                if inp.is_linked:
                    img = find_image_from_socket(inp, visited)
                    if img:
                        return img
            return None

        principled = None
        for node in material.node_tree.nodes:
            if node.type == 'BSDF_PRINCIPLED':
                principled = node
                break

        image = None
        if principled:
            image = find_image_from_socket(principled.inputs.get('Base Color'))
        if image is None:
            for node in material.node_tree.nodes:
                if node.type == 'TEX_IMAGE' and node.image:
                    image = node.image
                    break

        if image:
            path = image.filepath or image.filepath_raw or image.name or ""
            if path:
                path = bpy.path.abspath(path)
            return path, image
        return "", None

    def _save_textures(self, output_dir):
        """Save queued texture images as TGA files alongside the ASE."""
        for filename, image in self._texture_queue.items():
            dest = os.path.join(output_dir, filename)
            if os.path.exists(dest):
                print(f"ASE Export: Texture already exists, skipping: {filename}")
                continue

            try:
                # Save via a temporary scene settings override to force TGA output.
                # We work on a copy so the original image's settings are untouched.
                orig_path = image.filepath_raw
                orig_format = image.file_format

                image.filepath_raw = dest
                image.file_format = 'TARGA'
                image.save()

                # Restore original values
                image.filepath_raw = orig_path
                image.file_format = orig_format

                print(f"ASE Export: Saved texture {filename}")
            except Exception as e:
                print(f"ASE Export: Failed to save texture {filename}: {e}")

    def export_scene_objects(self):
        """Export all scene objects"""
        # Export armature helpers and bones first (matches 3ds Max bone output)
        for obj in self.scene.objects:
            if obj.type == 'ARMATURE':
                self.export_helper_object(obj)
                self.export_armature_bones(obj)

        for obj in self.scene.objects:
            if self.is_exportable_object(obj):
                if obj.type == 'MESH':
                    self.export_geometry_object(obj)
                elif obj.type == 'CAMERA':
                    self.export_camera_object(obj)
                elif obj.type == 'LIGHT':
                    self.export_light_object(obj)
                elif obj.type == 'CURVE':
                    self.export_shape_object(obj)
                elif obj.type == 'EMPTY':
                    self.export_helper_object(obj)
                else:
                    print(f"ASE Export: Skipping object '{obj.name}' of type '{obj.type}'")

    def export_geometry_object(self, obj):
        """Export geometry object (mesh) as a single GEOMOBJECT."""
        print(f"ASE Export: Exporting geometry object '{obj.name}'")

        try:
            self.write_line(f"{tokens.ID_GEOMETRY} {{")

            self.export_node_header(obj, 1)
            self.export_node_transform(obj, 1)

            if self.include_mesh:
                self.export_mesh(obj, 1)

            self.write_line(f"\t{tokens.ID_PROP_MOTIONBLUR} 0")
            self.write_line(f"\t{tokens.ID_PROP_CASTSHADOW} 1")
            self.write_line(f"\t{tokens.ID_PROP_RECVSHADOW} 1")

            if self.include_materials and obj.data.materials:
                material_id = self.material_keeper.get_object_material_id(obj)
                if material_id >= 0:
                    self.write_line(f"\t*MATERIAL_REF {material_id}")
            else:
                wire_color = obj.color[:3] if hasattr(obj, 'color') else (0.6, 0.6, 0.6)
                self.write_line(f"\t{tokens.ID_WIREFRAME_COLOR} {self.format_color(wire_color)}")

            self.write_line("}")

        except Exception as e:
            print(f"ASE Export Error: Failed to export geometry object '{obj.name}': {e}")
            raise

    def _prepare_triangulated_mesh(self, obj):
        """Prepare a triangulated temp mesh. Returns (mesh_eval, bm, temp_mesh, neg_scale, orig_edge_keys)."""
        mesh = obj.data

        depsgraph = bpy.context.evaluated_depsgraph_get()
        obj_eval = obj.evaluated_get(depsgraph)
        mesh_eval = obj_eval.data

        bm = bmesh.new()
        bm.from_mesh(mesh_eval)
        bmesh.ops.triangulate(bm, faces=bm.faces)

        temp_mesh = bpy.data.meshes.new("temp_export")
        bm.to_mesh(temp_mesh)
        temp_mesh.calc_loop_triangles()
        self._ensure_split_normals(temp_mesh)

        neg_scale = obj.matrix_world.determinant() < 0
        orig_edge_keys = {tuple(sorted(edge.vertices)) for edge in mesh_eval.edges}

        return mesh_eval, bm, temp_mesh, neg_scale, orig_edge_keys

    def _export_name(self, name):
        """Map Blender object name to ASE node name.

        PN## helpers (part nodes) must be exported as bare numbers ("01", "02", ...)
        so that df4oed's classification loop recognises the first digit character and
        assigns them to the correct numbered group.
        """
        if name.startswith("PN") and len(name) >= 4 and name[2:4].isdigit():
            return name[2:]  # "PN01" -> "01", "PN40" -> "40"
        return name

    def export_node_header(self, obj, indent_level):
        """Export node header information"""
        indent = self.get_indent(indent_level)

        # Node name
        export_name = self._export_name(obj.name)
        self.write_line(f'{indent}{tokens.ID_NODE_NAME} "{self.fixup_name(export_name)}"')

        # Parent node — export for any linked object (matches 3ds Max behavior)
        if obj.parent:
            parent_name = self._export_name(obj.parent.name)
            self.write_line(f'{indent}{tokens.ID_NODE_PARENT} "{self.fixup_name(parent_name)}"')

        # Check for bone numbering (nodes starting with "BN")
        if obj.name.startswith("BN") and len(obj.name) >= 4:
            try:
                bone_number = int(obj.name[2:4]) - 1  # BN01 -> 0, BN02 -> 1, etc.
                self.write_line(f"{indent}{tokens.ID_NODE_BONENUMBER} {bone_number}")
            except ValueError:
                pass

    def export_node_transform(self, obj, indent_level):
        """Export node transformation matrix (world space, like 3ds Max's GetNodeTM)"""
        self._write_node_tm(self._export_name(obj.name), obj.matrix_world, indent_level)

    def _write_node_tm(self, name, matrix, indent_level):
        """Write a NODE_TM block from a matrix."""
        indent = self.get_indent(indent_level)

        self.write_line(f"{indent}{tokens.ID_NODE_TM} {{")
        self.write_line(f'{indent}\t{tokens.ID_NODE_NAME} "{self.fixup_name(name)}"')

        # Inherit flags (matches reference exporter sub_10007080)
        self.write_line(f"{indent}\t*INHERIT_POS 0 0 0")
        self.write_line(f"{indent}\t*INHERIT_ROT 0 0 0")
        self.write_line(f"{indent}\t*INHERIT_SCL 0 0 0")

        rot = matrix.to_3x3().transposed()
        self.write_line(f"{indent}\t{tokens.ID_TM_ROW0} {self.format_point3(rot[0][:3])}")
        self.write_line(f"{indent}\t{tokens.ID_TM_ROW1} {self.format_point3(rot[1][:3])}")
        self.write_line(f"{indent}\t{tokens.ID_TM_ROW2} {self.format_point3(rot[2][:3])}")
        row3 = [matrix.translation[i] * self.scale for i in range(3)]
        self.write_line(f"{indent}\t{tokens.ID_TM_ROW3} {self.format_point3(row3)}")

        pos = [matrix.translation[i] * self.scale for i in range(3)]
        self.write_line(f"{indent}\t{tokens.ID_TM_POS} {self.format_point3(pos)}")
        loc, rot, scale = matrix.decompose()
        axis, angle = rot.to_axis_angle()
        if abs(angle) < 1e-6:
            axis = Vector((0, 0, 0))
            angle = 0.0
        self.write_line(f"{indent}\t{tokens.ID_TM_ROTAXIS} {self.format_point3(axis)}")
        self.write_line(f"{indent}\t{tokens.ID_TM_ROTANGLE} {self.format_float(angle)}")
        self.write_line(f"{indent}\t{tokens.ID_TM_SCALE} {self.format_point3(scale)}")
        self.write_line(f"{indent}\t{tokens.ID_TM_SCALEAXIS} {self.format_point3((0, 0, 0))}")
        self.write_line(f"{indent}\t{tokens.ID_TM_SCALEAXISANG} {self.format_float(0.0)}")

        self.write_line(f"{indent}}}")

    def export_mesh(self, obj, indent_level):
        """Export mesh geometry (single-material path with own triangulation)."""
        print(f"ASE Export: Processing mesh '{obj.name}'")

        mesh_eval, bm, temp_mesh, neg_scale, orig_edge_keys = self._prepare_triangulated_mesh(obj)
        print(f"ASE Export: Triangulated mesh has {len(temp_mesh.vertices)} vertices, {len(temp_mesh.loop_triangles)} triangles (neg_scale={neg_scale})")

        self._export_full_mesh(obj, temp_mesh, neg_scale, orig_edge_keys, indent_level)

        # Cleanup
        bm.free()
        bpy.data.meshes.remove(temp_mesh)

    def _export_full_mesh(self, obj, temp_mesh, neg_scale, orig_edge_keys, indent_level):
        """Export a full mesh block (all triangles, no subsetting)."""
        indent = self.get_indent(indent_level)

        self.write_line(f"{indent}{tokens.ID_MESH} {{")
        self.write_line(f"{indent}\t{tokens.ID_TIMEVALUE} 0")

        vertex_count = len(temp_mesh.vertices)
        self.write_line(f"{indent}\t{tokens.ID_MESH_NUMVERTEX} {vertex_count}")

        face_count = len(temp_mesh.loop_triangles)
        self.write_line(f"{indent}\t{tokens.ID_MESH_NUMFACES} {face_count}")

        self.export_mesh_vertices(temp_mesh, obj, indent_level + 1)
        self.export_mesh_faces(temp_mesh, obj, indent_level + 1, neg_scale, orig_edge_keys)

        if self.include_uvs and temp_mesh.uv_layers:
            self.export_mesh_uvs(temp_mesh, indent_level + 1, neg_scale)

        if self.include_vertex_colors and (hasattr(temp_mesh, 'color_attributes') and temp_mesh.color_attributes):
            self.export_mesh_vertex_colors(temp_mesh, indent_level + 1, neg_scale)
        else:
            self.write_line(f"{indent}\t{tokens.ID_MESH_NUMCVERTEX} 0")

        if self.include_bone_weights:
            self.export_mesh_weights(obj, temp_mesh, indent_level)

        if self.include_normals:
            self.export_mesh_normals(temp_mesh, indent_level + 1, neg_scale)

        self.write_line(f"{indent}}}")

    def export_mesh_vertices(self, mesh, obj, indent_level):
        """Export mesh vertices"""
        indent = self.get_indent(indent_level)

        self.write_line(f"{indent}{tokens.ID_MESH_VERTEX_LIST} {{")

        # Export vertices with object transform applied (like 3ds Max does)
        # In 3ds Max: mesh positioned at absolute_location, vertices relative to that
        # We need to recreate the same final vertex positions
        transform_matrix = obj.matrix_world

        for i, vertex in enumerate(mesh.vertices):
            # Apply object transformation then scale (matches reference exporter)
            world_co = transform_matrix @ vertex.co
            scaled_co = [world_co[j] * self.scale for j in range(3)]
            self.write_line(f"{indent}\t{tokens.ID_MESH_VERTEX} {i:4d}\t{self.format_point3(scaled_co)}")

        self.write_line(f"{indent}}}")  # End vertex list

    def export_mesh_faces(self, mesh, obj, indent_level, neg_scale=False, orig_edge_keys=None):
        """Export mesh faces"""
        indent = self.get_indent(indent_level)

        self.write_line(f"{indent}{tokens.ID_MESH_FACE_LIST} {{")

        for i, tri in enumerate(mesh.loop_triangles):
            # Get vertex indices; reverse winding for negative scale (like 3ds Max TMNegParity)
            if neg_scale:
                v1, v2, v3 = tri.vertices[2], tri.vertices[1], tri.vertices[0]
            else:
                v1, v2, v3 = tri.vertices

            # Get sub-material ID (slot index within multi/sub-object material)
            material_id = tri.material_index

            # Edge visibility: visible if the edge existed before triangulation
            if orig_edge_keys:
                def edge_vis(a, b):
                    return 1 if tuple(sorted((a, b))) in orig_edge_keys else 0
                ab = edge_vis(v1, v2)
                bc = edge_vis(v2, v3)
                ca = edge_vis(v3, v1)
            else:
                ab, bc, ca = 1, 1, 1

            # Check for custom smoothing_group attribute first (set by scene_builder
            # for collision/occlusion meshes), fall back to Blender's use_smooth.
            sg_attr = mesh.attributes.get('smoothing_group') if hasattr(mesh, 'attributes') else None
            if sg_attr and sg_attr.domain == 'FACE':
                smoothing_group = sg_attr.data[i].value
            else:
                smoothing_group = 1 if tri.use_smooth else 0

            # Build face line matching reference format (sub_10007450)
            line = f"{indent}\t{tokens.ID_MESH_FACE} {i:4d}:    A: {v1:4d} B: {v2:4d} C: {v3:4d} AB: {ab:4d} BC: {bc:4d} CA: {ca:4d}"
            # Smoothing groups: tab, space, token, space, then comma-separated bit positions
            line += f"\t {tokens.ID_MESH_SMOOTHING} "
            smooth_bits = []
            for bit in range(32):
                if smoothing_group & (1 << bit):
                    smooth_bits.append(str(bit))  # 0-indexed, matching reference binary
            if smooth_bits:
                # All but last get trailing comma, last gets trailing space
                if len(smooth_bits) > 1:
                    line += ",".join(smooth_bits[:-1]) + ","
                line += smooth_bits[-1] + " "
            # MTLID
            line += f"\t{tokens.ID_MESH_MTLID} {material_id}"
            self.write_line(line)

        self.write_line(f"{indent}}}")  # End face list

    def export_mesh_uvs(self, mesh, indent_level, neg_scale=False):
        """Export mesh UV coordinates"""
        indent = self.get_indent(indent_level)

        # Blender 4.5 API - check for UV layers
        if not mesh.uv_layers or len(mesh.uv_layers) == 0:
            print(f"ASE Export: No UV layers found")
            # 3ds Max exporter outputs NUMTVERTEX 0 even when no UVs
            self.write_line(f"{indent}{tokens.ID_MESH_NUMTVERTEX} 0")
            return

        uv_layer = mesh.uv_layers.active
        if not uv_layer:
            print(f"ASE Export: No active UV layer")
            self.write_line(f"{indent}{tokens.ID_MESH_NUMTVERTEX} 0")
            return

        # Deduplicate UV vertices by (vertex_index, uv_coord) pair, matching 3dsMax behavior.
        # Each unique (vertex, UV) combination gets one TVERT entry; TFACEs index into those.
        uv_coords = []
        uv_faces = []
        uv_map = {}  # (vertex_index, rounded_u, rounded_v) -> tvert index

        for tri in mesh.loop_triangles:
            face_uvs = []
            # Reverse loop order for negative scale to match winding reversal
            loops = list(reversed(tri.loops)) if neg_scale else list(tri.loops)
            verts = list(reversed(tri.vertices)) if neg_scale else list(tri.vertices)
            for loop_index, vert_index in zip(loops, verts):
                uv = uv_layer.data[loop_index].uv
                key = (vert_index, round(uv.x, 6), round(uv.y, 6))
                if key not in uv_map:
                    uv_map[key] = len(uv_coords)
                    uv_coords.append(uv)
                face_uvs.append(uv_map[key])

            uv_faces.append(face_uvs)

        # Export UV vertices
        self.write_line(f"{indent}{tokens.ID_MESH_NUMTVERTEX} {len(uv_coords)}")
        self.write_line(f"{indent}{tokens.ID_MESH_TVERTLIST} {{")

        for i, uv in enumerate(uv_coords):
            # UV V-flip: Blender V=0 at bottom, game/OED/DDS convention V=0 at top.
            # The importer applies (1-V) on load, so we reverse it here.
            self.write_line(f"{indent}\t{tokens.ID_MESH_TVERT} {i}\t{self.format_float(uv.x)}\t{self.format_float(1.0 - uv.y)}\t{self.format_float(0.0)}")

        self.write_line(f"{indent}}}")  # End UV vertex list

        # Export UV faces
        self.write_line(f"{indent}{tokens.ID_MESH_NUMTVFACES} {len(uv_faces)}")
        self.write_line(f"{indent}{tokens.ID_MESH_TFACELIST} {{")

        for i, face_uvs in enumerate(uv_faces):
            self.write_line(f"{indent}\t{tokens.ID_MESH_TFACE} {i}\t{face_uvs[0]}\t{face_uvs[1]}\t{face_uvs[2]}")

        self.write_line(f"{indent}}}")  # End UV face list

    def export_mesh_normals(self, mesh, indent_level, neg_scale=False):
        """Export mesh normals using per-face-corner split normals (like 3ds Max GetVertexNormal)"""
        indent = self.get_indent(indent_level)

        self.write_line(f"{indent}{tokens.ID_MESH_NORMALS} {{")
        use_corner_normals = hasattr(mesh, 'corner_normals') and len(mesh.corner_normals) == len(mesh.loops)

        # Export face normals and vertex normals
        for i, tri in enumerate(mesh.loop_triangles):
            # Face normal
            self.write_line(f"{indent}\t{tokens.ID_MESH_FACENORMAL} {i}\t{self.format_point3(tri.normal)}")

            # Vertex normals for this face using split normals (per-face-corner)
            # Reverse order for negative scale to match winding reversal
            loops = list(tri.loops)
            if neg_scale:
                loops = list(reversed(loops))
            for loop_index in loops:
                vertex_index = mesh.loops[loop_index].vertex_index
                if use_corner_normals:
                    normal = mesh.corner_normals[loop_index].vector
                elif hasattr(mesh, 'vertex_normals') and len(mesh.vertex_normals) > vertex_index:
                    normal = mesh.vertex_normals[vertex_index]
                else:
                    normal = tri.normal
                self.write_line(f"{indent}\t\t{tokens.ID_MESH_VERTEXNORMAL} {vertex_index}\t{self.format_point3(normal)}")

        self.write_line(f"{indent}}}")  # End normals

    def _ensure_split_normals(self, mesh):
        """Ensure split normals are available across Blender versions."""
        if hasattr(mesh, "calc_normals_split"):
            mesh.calc_normals_split()
            return True
        if hasattr(mesh, "calc_normals"):
            mesh.calc_normals()
        return False

    def export_mesh_vertex_colors(self, mesh, indent_level, neg_scale=False):
        """Export mesh vertex colors"""
        indent = self.get_indent(indent_level)

        # Blender 4.5 API - use color_attributes instead of vertex_colors
        if not hasattr(mesh, 'color_attributes') or not mesh.color_attributes:
            print(f"ASE Export: No color attributes found")
            return

        color_layer = mesh.color_attributes.active_color
        if not color_layer:
            print(f"ASE Export: No active color attribute")
            return

        # Similar to UV export, collect unique colors
        colors = []
        color_faces = []
        color_map = {}

        for tri in mesh.loop_triangles:
            face_colors = []
            # Reverse loop order for negative scale to match winding reversal
            loops = reversed(tri.loops) if neg_scale else tri.loops
            for loop_index in loops:
                color = color_layer.data[loop_index].color
                color_key = tuple(round(c, 6) for c in color[:3])  # RGB only

                if color_key not in color_map:
                    color_map[color_key] = len(colors)
                    colors.append(color)

                face_colors.append(color_map[color_key])

            color_faces.append(face_colors)

        # Export vertex colors
        self.write_line(f"{indent}{tokens.ID_MESH_NUMCVERTEX} {len(colors)}")
        self.write_line(f"{indent}{tokens.ID_MESH_CVERTLIST} {{")

        for i, color in enumerate(colors):
            self.write_line(f"{indent}\t{tokens.ID_MESH_VERTCOL} {i}\t{self.format_color(color[:3])}")

        self.write_line(f"{indent}}}")  # End vertex color list

        # Export color faces
        self.write_line(f"{indent}{tokens.ID_MESH_NUMCVFACES} {len(color_faces)}")
        self.write_line(f"{indent}{tokens.ID_MESH_CFACELIST} {{")

        for i, face_colors in enumerate(color_faces):
            self.write_line(f"{indent}\t{tokens.ID_MESH_CFACE} {i}\t{face_colors[0]}\t{face_colors[1]}\t{face_colors[2]}")

        self.write_line(f"{indent}}}")  # End color face list

    def export_mesh_weights(self, obj, mesh, indent_level):
        """Export mesh vertex bone weights (MESH_WEIGHTS block)"""
        indent = self.get_indent(indent_level)

        print(f"ASE Export: Exporting mesh weights for '{obj.name}'")

        # Check if object has vertex groups (bone weights)
        if not obj.vertex_groups:
            print(f"ASE Export: No vertex groups on '{obj.name}', skipping weights")
            return
        print(f"ASE Export: '{obj.name}' has {len(obj.vertex_groups)} vertex groups: {[g.name for g in obj.vertex_groups]}")

        # Check for armature modifier to get bone mapping
        armature_mod = None
        for mod in obj.modifiers:
            if mod.type == 'ARMATURE' and mod.object:
                armature_mod = mod
                break

        if not armature_mod:
            print(f"ASE Export: No Armature modifier found on '{obj.name}' (modifiers: {[m.type for m in obj.modifiers]}), skipping weights")
            return

        armature_obj = armature_mod.object
        print(f"ASE Export: Found armature '{armature_obj.name}' with {len(armature_obj.pose.bones)} bones")

        # Create bone name to number mapping (BN01, BN02, etc.)
        bone_number_map = {}
        for bone_name in armature_obj.pose.bones.keys():
            if bone_name.startswith("BN") and len(bone_name) >= 4:
                try:
                    bone_number = int(bone_name[2:4]) - 1  # BN01 -> 0, BN02 -> 1, etc.
                    bone_number_map[bone_name] = bone_number
                    print(f"ASE Export: Mapped bone '{bone_name}' to number {bone_number}")
                except ValueError:
                    print(f"ASE Export: Could not parse bone number from '{bone_name}'")

        if not bone_number_map:
            print(f"ASE Export: No BN-numbered bones found, skipping weights")
            return

        print(f"ASE Export: Mesh has {len(mesh.vertices)} vertices, object has {len(obj.vertex_groups)} vertex groups")

        # Start MESH_WEIGHTS block
        self.write_line(f"{indent}{tokens.ID_MESH_WEIGHTS} {{")

        # Export weights for each vertex
        weighted_count = 0
        for vertex_idx, vertex in enumerate(mesh.vertices):
            # Get vertex groups for this vertex (up to 4 influences)
            influences = [-1, -1, -1, -1]
            weights = [0.0, 0.0, 0.0, 0.0]

            # Collect all vertex group influences for this vertex
            vertex_influences = []
            for group in obj.vertex_groups:
                try:
                    weight = group.weight(vertex_idx)
                    if weight > 0 and group.name in bone_number_map:  # Include all non-zero weights
                        vertex_influences.append((bone_number_map[group.name], weight))
                except RuntimeError:
                    # Vertex not in this group
                    continue

            # Sort by bone index (ascending) and take top 4
            vertex_influences.sort(key=lambda x: x[0])
            vertex_influences = vertex_influences[:4]

            # Fill influences and weights arrays
            for i, (bone_num, weight) in enumerate(vertex_influences):
                influences[i] = bone_num
                weights[i] = weight

            if vertex_influences:
                weighted_count += 1

            # Write MESH_WEIGHTSVERTEX line
            # Format: *MESH_WEIGHTSVERTEX vertex_idx bone1 bone2 bone3 bone4 weight1 weight2 weight3 weight4
            self.write_line(f"{indent}\t{tokens.ID_MESH_WEIGHTSVERTEX} {vertex_idx}\t{influences[0]}\t{influences[1]}\t{influences[2]}\t{influences[3]}\t{self.format_float(weights[0])}\t{self.format_float(weights[1])}\t{self.format_float(weights[2])}\t{self.format_float(weights[3])}")

        self.write_line(f"{indent}}}")  # End MESH_WEIGHTS
        print(f"ASE Export: Exported weights for {len(mesh.vertices)} vertices ({weighted_count} with bone influences, {len(mesh.vertices) - weighted_count} empty)")

    def export_camera_object(self, obj):
        """Export camera object (placeholder)"""
        self.write_line(f"{tokens.ID_CAMERA} {{")
        self.export_node_header(obj, 1)
        self.export_node_transform(obj, 1)
        # TODO: Add camera-specific properties
        self.write_line("}")

    def export_light_object(self, obj):
        """Export light object with full LIGHT_SETTINGS block."""
        self.write_line(f"{tokens.ID_LIGHT} {{")
        self.export_node_header(obj, 1)

        # Light type - all game lights are omni/point
        self.write_line(f"\t{tokens.ID_LIGHT_TYPE} Omni")

        self.export_node_transform(obj, 1)

        # Light flags
        self.write_line(f"\t{tokens.ID_LIGHT_SHADOWS} Off")
        self.write_line(f"\t{tokens.ID_LIGHT_USELIGHT} 1")
        self.write_line(f"\t{tokens.ID_LIGHT_SPOTSHAPE} Circle")
        self.write_line(f"\t{tokens.ID_LIGHT_USEGLOBAL} 0")
        self.write_line(f"\t{tokens.ID_LIGHT_ABSMAPBIAS} 0")
        self.write_line(f"\t{tokens.ID_LIGHT_OVERSHOOT} 0")

        # Light settings
        light_data = obj.data
        r, g, b = light_data.color[0], light_data.color[1], light_data.color[2]
        attn_end = 0.0
        if hasattr(light_data, 'use_custom_distance') and light_data.use_custom_distance:
            attn_end = light_data.cutoff_distance

        self.write_line(f"\t{tokens.ID_LIGHT_SETTINGS} {{")
        self.write_line(f"\t\t{tokens.ID_TIMEVALUE} 0")
        self.write_line(f"\t\t{tokens.ID_LIGHT_COLOR} {self.format_float(r)}\t{self.format_float(g)}\t{self.format_float(b)}")
        self.write_line(f"\t\t{tokens.ID_LIGHT_INTENS} {self.format_float(0.0)}")
        self.write_line(f"\t\t{tokens.ID_LIGHT_ASPECT} {self.format_float(-1.0)}")
        self.write_line(f"\t\t{tokens.ID_LIGHT_ATTNSTART} {self.format_float(0.0)}")
        self.write_line(f"\t\t{tokens.ID_LIGHT_ATTNEND} {self.format_float(attn_end)}")
        self.write_line(f"\t\t{tokens.ID_LIGHT_TDIST} {self.format_float(-1.0)}")
        self.write_line(f"\t\t{tokens.ID_LIGHT_MAPBIAS} {self.format_float(0.0)}")
        self.write_line(f"\t\t{tokens.ID_LIGHT_MAPRANGE} {self.format_float(0.0)}")
        self.write_line(f"\t\t{tokens.ID_LIGHT_MAPSIZE} 0")
        self.write_line(f"\t\t{tokens.ID_LIGHT_RAYBIAS} {self.format_float(0.2)}")
        self.write_line(f"\t}}")

        self.write_line("}")

    def export_shape_object(self, obj):
        """Export shape object (curve)"""
        self.write_line(f"{tokens.ID_SHAPE} {{")
        self.export_node_header(obj, 1)
        self.export_node_transform(obj, 1)

        curve = obj.data
        splines = curve.splines
        self.write_line(f"\t{tokens.ID_SHAPE_LINECOUNT} {len(splines)}")

        for line_idx, spline in enumerate(splines):
            self.write_line(f"\t{tokens.ID_SHAPE_LINE} {line_idx} {{")
            if spline.use_cyclic_u:
                self.write_line(f"\t\t{tokens.ID_SHAPE_CLOSED}")

            if spline.type == 'BEZIER':
                points = [p.co for p in spline.bezier_points]
            else:
                points = [p.co[:3] for p in spline.points]

            self.write_line(f"\t\t{tokens.ID_SHAPE_VERTEXCOUNT} {len(points)}")
            for idx, co in enumerate(points):
                world = obj.matrix_world @ Vector(co)
                self.write_line(f"\t\t{tokens.ID_SHAPE_VERTEX_KNOT}\t{idx}\t{self.format_point3(world)}")

            self.write_line(f"\t}}")

        self.write_line("}")

    def export_helper_object(self, obj):
        """Export helper object (empty)"""
        self.write_line(f"{tokens.ID_HELPER} {{")
        self.export_node_header(obj, 1)

        # Helper class — always "Dummy" to match 3dsMax SDK GetClassName() output
        self.write_line(f'\t{tokens.ID_HELPER_CLASS} "Dummy"')

        self.export_node_transform(obj, 1)

        # Bounding box (after NODE_TM)
        bbox_min, bbox_max = self.compute_hierarchy_bounds(obj)
        self.write_line(f"\t{tokens.ID_BOUNDINGBOX_MIN} {self.format_point3(bbox_min)}")
        self.write_line(f"\t{tokens.ID_BOUNDINGBOX_MAX} {self.format_point3(bbox_max)}")

        self.write_line("}")

    def export_armature_bones(self, armature_obj):
        """Export armature bones as GEOMOBJECTs with NODE_BONENUMBER."""
        if not armature_obj.pose:
            return

        # Sort bones by name for deterministic output (BN01, BN02, ...)
        bones = sorted(armature_obj.pose.bones, key=lambda b: b.name)
        for bone in bones:
            self.export_bone_object(armature_obj, bone)

    def export_bone_object(self, armature_obj, pose_bone):
        """Export a single bone as a GEOMOBJECT with a small bone mesh."""
        bone_name = self._bone_export_name(pose_bone.name)
        parent_name = None
        if pose_bone.parent:
            parent_name = self._bone_export_name(pose_bone.parent.name)
        else:
            parent_name = armature_obj.name

        matrix_world = armature_obj.matrix_world @ pose_bone.matrix

        self.write_line(f"{tokens.ID_GEOMETRY} {{")
        indent = self.get_indent(1)

        self.write_line(f'{indent}{tokens.ID_NODE_NAME} "{self.fixup_name(bone_name)}"')
        if parent_name:
            self.write_line(f'{indent}{tokens.ID_NODE_PARENT} "{self.fixup_name(parent_name)}"')

        if bone_name.startswith("BN") and len(bone_name) >= 4 and bone_name[2:4].isdigit():
            bone_number = int(bone_name[2:4]) - 1
            self.write_line(f"{indent}{tokens.ID_NODE_BONENUMBER} {bone_number}")

        self._write_node_tm(bone_name, matrix_world, 1)
        if self.include_mesh:
            self._export_bone_mesh(matrix_world, 1)

        # Bone objects typically have no material; emit wireframe color
        self.write_line(f"{indent}{tokens.ID_PROP_MOTIONBLUR} 0")
        self.write_line(f"{indent}{tokens.ID_PROP_CASTSHADOW} 1")
        self.write_line(f"{indent}{tokens.ID_PROP_RECVSHADOW} 1")
        self.write_line(f"{indent}{tokens.ID_WIREFRAME_COLOR} {self.format_color((0.6, 0.6, 0.6))}")

        self.write_line("}")

    def _bone_export_name(self, name):
        """Trim bone names like 'BN01 Hips' to 'BN01' for export."""
        if name.startswith("BN") and len(name) >= 4 and name[2:4].isdigit():
            return name[:4]
        return name

    def _export_bone_mesh(self, matrix_world, indent_level):
        """Emit a small bone mesh with 9 verts / 14 faces to match Max bone output."""
        indent = self.get_indent(indent_level)

        # Bone mesh in local space (approximate 3ds Max bone mesh)
        base = 0.01
        tip = 0.001
        base_z = 0.01
        tip_z = 0.10

        verts_local = [
            Vector(( base,  base, base_z)),
            Vector(( base, -base, base_z)),
            Vector((-base, -base, base_z)),
            Vector((-base,  base, base_z)),
            Vector(( tip,  tip,  tip_z)),
            Vector(( tip, -tip,  tip_z)),
            Vector((-tip, -tip,  tip_z)),
            Vector((-tip,  tip,  tip_z)),
            Vector((0.0, 0.0, 0.0)),
        ]

        faces = [
            (8, 0, 1),
            (8, 1, 2),
            (8, 2, 3),
            (8, 3, 0),
            (0, 1, 5),
            (0, 5, 4),
            (1, 2, 6),
            (1, 6, 5),
            (2, 3, 7),
            (2, 7, 6),
            (3, 0, 4),
            (3, 4, 7),
            (4, 5, 6),
            (4, 6, 7),
        ]

        # Transform verts to world space
        verts_world = []
        for v in verts_local:
            w = matrix_world @ v
            verts_world.append([w[i] * self.scale for i in range(3)])

        self.write_line(f"{indent}{tokens.ID_MESH} {{")
        self.write_line(f"{indent}\t{tokens.ID_TIMEVALUE} 0")
        self.write_line(f"{indent}\t{tokens.ID_MESH_NUMVERTEX} {len(verts_world)}")
        self.write_line(f"{indent}\t{tokens.ID_MESH_NUMFACES} {len(faces)}")

        self.write_line(f"{indent}\t{tokens.ID_MESH_VERTEX_LIST} {{")
        for i, v in enumerate(verts_world):
            self.write_line(f"{indent}\t\t{tokens.ID_MESH_VERTEX} {i:4d}\t{self.format_point3(v)}")
        self.write_line(f"{indent}\t}}")

        self.write_line(f"{indent}\t{tokens.ID_MESH_FACE_LIST} {{")
        for i, (a, b, c) in enumerate(faces):
            ab = bc = ca = 1
            line = (f"{indent}\t\t{tokens.ID_MESH_FACE} {i:4d}:    "
                    f"A: {a:4d} B: {b:4d} C: {c:4d} AB: {ab:4d} BC: {bc:4d} CA: {ca:4d}")
            line += f"\t {tokens.ID_MESH_SMOOTHING} 0 "
            line += f"\t{tokens.ID_MESH_MTLID} 0"
            self.write_line(line)
        self.write_line(f"{indent}\t}}")

        # No UVs / vertex colors
        self.write_line(f"{indent}\t{tokens.ID_MESH_NUMTVERTEX} 0")
        self.write_line(f"{indent}\t{tokens.ID_MESH_NUMCVERTEX} 0")

        # Normals in local space
        if self.include_normals:
            self.write_line(f"{indent}\t{tokens.ID_MESH_NORMALS} {{")
            for i, (a, b, c) in enumerate(faces):
                v1 = verts_local[a]
                v2 = verts_local[b]
                v3 = verts_local[c]
                normal = (v2 - v1).cross(v3 - v1)
                if normal.length > 0:
                    normal.normalize()
                self.write_line(f"{indent}\t\t{tokens.ID_MESH_FACENORMAL} {i}\t{self.format_point3(normal)}")
                self.write_line(f"{indent}\t\t\t{tokens.ID_MESH_VERTEXNORMAL} {a}\t{self.format_point3(normal)}")
                self.write_line(f"{indent}\t\t\t{tokens.ID_MESH_VERTEXNORMAL} {b}\t{self.format_point3(normal)}")
                self.write_line(f"{indent}\t\t\t{tokens.ID_MESH_VERTEXNORMAL} {c}\t{self.format_point3(normal)}")
            self.write_line(f"{indent}\t}}")

        self.write_line(f"{indent}}}")

    def compute_hierarchy_bounds(self, obj):
        """Compute world-space bounding box encompassing all descendant mesh objects"""
        min_coords = [float('inf')] * 3
        max_coords = [float('-inf')] * 3
        found_mesh = False

        def collect_mesh_bounds(node):
            nonlocal min_coords, max_coords, found_mesh
            if node.type == 'MESH' and node.data:
                found_mesh = True
                for corner in node.bound_box:
                    world_corner = node.matrix_world @ Vector(corner)
                    scaled = [world_corner[i] * self.scale for i in range(3)]
                    for i in range(3):
                        min_coords[i] = min(min_coords[i], scaled[i])
                        max_coords[i] = max(max_coords[i], scaled[i])
            for child in node.children:
                collect_mesh_bounds(child)

        collect_mesh_bounds(obj)

        if not found_mesh:
            return (-0.5, -0.5, -0.5), (0.5, 0.5, 0.5)

        return tuple(min_coords), tuple(max_coords)

    # Utility methods
    def write_line(self, text):
        """Write a line to the output stream"""
        if self.file_stream:
            self.file_stream.write(text + "\n")

    def get_indent(self, level):
        """Get indentation string"""
        return "\t" * level

    def format_float(self, value):
        """Format float value"""
        return self.float_format % value

    def format_point3(self, point):
        """Format 3D point/vector"""
        return f"{self.format_float(point[0])}\t{self.format_float(point[1])}\t{self.format_float(point[2])}"

    def format_color(self, color):
        """Format color (RGB)"""
        return self.format_point3(color)

    def fixup_name(self, name):
        """Fix up object name for ASE format (matches 3ds Max FixupName)"""
        # Replace " with ' and control chars (<=31) with _ (reference export.cpp:1723-1740)
        result = []
        for ch in name:
            if ch == '"':
                result.append("'")
            elif ord(ch) <= 31:
                result.append('_')
            else:
                result.append(ch)
        name = ''.join(result)
        return name
