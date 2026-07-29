"""BlenderSceneBuilder: the facade class composed from the concern mixins.

Split from the 2,209-line scene_builder.py (quality campaign W3-5). The
mixins share the instance state declared in __init__ here and dispatch to
each other through self; the public surface is unchanged.
"""

from __future__ import annotations

import bpy

from .animation import AnimationMixin
from .armature import ArmatureMixin
from .helpers import _ensure_object_mode
from .materials import MaterialsMixin
from .meshes import MeshesMixin
from .overlays import OverlaysMixin


class BlenderSceneBuilder(
        ArmatureMixin, AnimationMixin, MeshesMixin, MaterialsMixin, OverlaysMixin):
    """Build a Blender scene from a ThreediModelIR + optional BadFile.

    - Part hierarchy using empties
    - Vertices localized to their part's coordinate space
    - Meshes parented to their part node
    """

    def __init__(self, ir, bad_file=None, anim_context=None,
                 resolver=None, import_collisions=True, import_occlusion=True,
                 import_lights=True):
        self.ir = ir
        self.bad_file = bad_file
        self.anim_context = anim_context
        self.resolver = resolver
        self.import_collisions = import_collisions
        self.import_occlusion = import_occlusion
        self.import_lights = import_lights

        # Runtime state
        self.part_nodes: dict[int, object] = {}   # index -> Empty object
        self.mesh_objects: list[object] = []       # mesh objects with _part_index
        self.material_dict: dict[int, object] = {}
        self.root_object = None
        self.armature_object = None
        self._bone_infos = []                      # populated by build_armature_from_bad
        self._mesh_bone_data = {}                  # mesh_obj.name -> per-vertex bone data
        self._world_rot_corrections = []           # BAD world rot -> Blender rest world rot
        self._rest_local_quats = []                # Blender local rest quaternions
        self._rest_local_inv_mats = []             # Blender local rest inverse 3x3 matrices

    def build_basic_scene(self, name: str):
        _ensure_object_mode()

        self.build_part_hierarchy(name)
        mesh_objects = self.create_basic_meshes(name)
        self._build_additional_lods(name)
        self.create_scene_markers()
        self.create_user_points()
        if self.import_occlusion:
            self.create_occlusion_visualization()
        if self.import_collisions:
            self.create_collision_visualization()
        if self.import_lights:
            self.create_scene_lights()

        # Build armature, bind meshes, and build animations if BAD + anim context
        if self.bad_file and self.anim_context:
            try:
                self.build_armature_from_bad(self.bad_file, name)
                self.bind_meshes_to_armature()
                self.build_animations_from_context(self.anim_context)
            except Exception as e:
                import traceback
                traceback.print_exc()
                print(f"Warning: failed to build animations: {e}")
        elif int(self.ir.mesh_type) == 3:
            # Preserve skin weights for static skinned models that have no BAD.
            # This keeps ASE MESH_WEIGHTS data so OED re-export doesn't collapse
            # all vertices onto subobject 0.
            try:
                self.build_armature_from_parts(name)
                self.bind_meshes_to_armature()
            except Exception as e:
                import traceback
                traceback.print_exc()
                print(f"Warning: failed to build synthetic armature: {e}")

        if not mesh_objects:
            print(f"No meshes created for {name}")
            return None

        if self.root_object:
            self.root_object.select_set(True)
            bpy.context.view_layer.objects.active = self.root_object
        for obj in mesh_objects:
            obj.select_set(True)

        print(f"Imported {len(self.part_nodes)} parts + {len(mesh_objects)} meshes for {name}")
        return mesh_objects

    def merge_with_existing_scene(self, main_builder):
        _ensure_object_mode()
        self.root_object = main_builder.root_object
        self.part_nodes = main_builder.part_nodes
        # Share material_dict so secondary models reuse existing Blender
        # material objects instead of creating duplicates (Material_0.001 etc.).
        # This prevents material count explosion during ASE export.
        self.material_dict = main_builder.material_dict
        mesh_objects = self.create_basic_meshes("merged")

        # Bind arms meshes to the main builder's existing armature
        if main_builder.armature_object and hasattr(main_builder, '_bone_infos'):
            self.armature_object = main_builder.armature_object
            self._bone_infos = main_builder._bone_infos
            self.bind_meshes_to_armature()

        return bool(mesh_objects)
