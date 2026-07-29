"""Materials: the .3di material -> Blender node graph port, texture
resolution through the asset resolver, and marker materials.

Mixin part of BlenderSceneBuilder -- instance state lives in core.__init__.
Cross-mixin contracts: meshes calls _create_material per LOD0 part, and
overlays calls _create_marker_material and _resolve_ctrl_reg (light colorgen)
through self.
"""

from __future__ import annotations

import os

import bpy


class MaterialsMixin:
    def _resolve_ctrl_reg(self, reg_index):
        """Resolve a control register index to its name string from the IR."""
        if reg_index < 0 or reg_index >= self.ir.control_register_count:
            return None
        if not self.ir.control_registers:
            return None
        name = self.ir.control_registers[reg_index].name
        if isinstance(name, bytes):
            name = name.decode("utf-8", errors="replace").rstrip("\x00")
        return name if name else None

    def _create_material(self, ir_mat):
        mat = bpy.data.materials.new(f"Material_{ir_mat.index}")
        mat.use_nodes = True
        mat.node_tree.nodes.clear()

        bsdf = mat.node_tree.nodes.new("ShaderNodeBsdfPrincipled")
        output = mat.node_tree.nodes.new("ShaderNodeOutputMaterial")
        mat.node_tree.links.new(bsdf.outputs["BSDF"], output.inputs["Surface"])
        bsdf.inputs["Base Color"].default_value = (0.8, 0.2, 0.2, 1.0)

        # NovaLogic's engine (D3D8 fixed-function) defaults to diffuse-only
        # with no specular contribution. The gsys_phong lookup texture provides
        # specular only when shader_type explicitly enables it. Set Blender
        # defaults to match: fully rough, no specular.
        bsdf.inputs["Roughness"].default_value = 1.0
        if "Specular IOR Level" in bsdf.inputs:
            bsdf.inputs["Specular IOR Level"].default_value = 0.0
        elif "Specular" in bsdf.inputs:
            bsdf.inputs["Specular"].default_value = 0.0

        shader = ir_mat.shader_name.decode("utf-8", errors="replace").rstrip("\x00")
        if shader:
            mat.name = f"{mat.name}_{shader}"
            mat["opennova_shader"] = shader

        # Store original name before Blender mangles duplicates with .NNN suffixes
        mat["ase_material_name"] = mat.name

        # Store all IR texture names as custom properties for round-trip export.
        # IR texture slots:
        #   1 = DIFFUSE  -> exported as MAP_DIFFUSE
        #   2 = DETAIL   -> lightmap/overlay, NOT exported (causes df4oed crash)
        #   3 = NORMAL   -> stored but not exported to ASE
        # NOTE: We do NOT export slot 2 as MAP_OPACITY because:
        # 1. These are lightmaps, not alpha masks
        # 2. MAP_OPACITY triggers a crash in df4oed's material preview (sub_403750)
        for t_idx in range(ir_mat.texture_count):
            tex = ir_mat.textures[t_idx]
            tex_name = tex.name.decode("utf-8", errors="replace").rstrip("\x00").strip()
            if not tex_name:
                continue
            if tex.slot == 1 or (t_idx == 0 and "ase_diffuse_bitmap" not in mat):
                mat["ase_diffuse_bitmap"] = tex_name
            elif tex.slot == 2:
                mat["ase_detail_bitmap"] = tex_name
            elif tex.slot == 3:
                mat["ase_normal_bitmap"] = tex_name
                mat["ase_normal_type"] = int(tex.type)  # 0=diffuse, 4=MDT, 5=TGA alpha

        # Default diffuse matching 3ds Max Standard material (0.588)
        mat.diffuse_color = (0.588, 0.588, 0.588, 1.0)

        if ir_mat.flags & 0x04:  # TWO_SIDED
            mat.use_backface_culling = False

        # Find and load diffuse texture for Blender viewport display
        tex_node = None
        diffuse_bitmap = mat.get("ase_diffuse_bitmap", "")
        if diffuse_bitmap:
            tex_path = self._resolve_texture(diffuse_bitmap)
            print(f"[TEX] mat={ir_mat.index} diffuse={diffuse_bitmap!r} -> {tex_path}")
            if tex_path:
                tex_node = mat.node_tree.nodes.new("ShaderNodeTexImage")
                tex_node.name = f"Diffuse_{diffuse_bitmap}"
                try:
                    existing = bpy.data.images.get(os.path.basename(tex_path))
                    if existing:
                        tex_node.image = existing
                    else:
                        tex_node.image = bpy.data.images.load(tex_path)
                    img = tex_node.image
                    print(f"[TEX] Loaded image: {img.name} size={img.size[0]}x{img.size[1]} channels={img.channels}")
                    mat.node_tree.links.new(tex_node.outputs["Color"], bsdf.inputs["Base Color"])

                    if ir_mat.flags & 0x01:  # ALPHA_TEST
                        mat.node_tree.links.new(tex_node.outputs["Alpha"], bsdf.inputs["Alpha"])
                        mat.blend_method = "CLIP"
                except Exception as e:
                    print(f"Failed to load texture {tex_path}: {e}")

        # Blend mode (overrides CLIP for true alpha-blend materials)
        if ir_mat.blend_mode == 1:  # ALPHA
            mat.blend_method = "BLEND"
            mat.show_transparent_back = False
            if tex_node:
                mat.node_tree.links.new(tex_node.outputs["Alpha"], bsdf.inputs["Alpha"])
        elif ir_mat.blend_mode == 2:  # ADDITIVE
            mat.blend_method = "BLEND"
            if "Emission Strength" in bsdf.inputs:
                bsdf.inputs["Emission Strength"].default_value = 1.0
            if tex_node and "Emission Color" in bsdf.inputs:
                mat.node_tree.links.new(tex_node.outputs["Color"], bsdf.inputs["Emission Color"])

        mat["blend_mode"] = ir_mat.blend_mode

        # Detail/lightmap texture (slot 2): create MixRGB Multiply node.
        # This gives visual representation in Blender (diffuse * detail) and
        # an exportable node structure (exporter detects MixRGB Multiply → 2 textures).
        detail_bitmap = mat.get("ase_detail_bitmap", "")
        if detail_bitmap and tex_node:
            detail_path = self._resolve_texture(detail_bitmap)
            if detail_path:
                try:
                    detail_tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
                    detail_tex.name = f"Detail_{detail_bitmap}"
                    existing = bpy.data.images.get(os.path.basename(detail_path))
                    if existing:
                        detail_tex.image = existing
                    else:
                        detail_tex.image = bpy.data.images.load(detail_path)

                    # Create MixRGB Multiply node: diffuse * detail → Base Color
                    mix_node = mat.node_tree.nodes.new("ShaderNodeMixRGB")
                    mix_node.blend_type = 'MULTIPLY'
                    mix_node.inputs["Fac"].default_value = 1.0
                    # Disconnect current diffuse → Base Color link
                    for link in list(mat.node_tree.links):
                        if (link.to_socket == bsdf.inputs["Base Color"]
                                and link.from_node == tex_node):
                            mat.node_tree.links.remove(link)
                            break
                    # Wire: diffuse → Color1, detail → Color2, Mix → Base Color
                    mat.node_tree.links.new(tex_node.outputs["Color"], mix_node.inputs["Color1"])
                    mat.node_tree.links.new(detail_tex.outputs["Color"], mix_node.inputs["Color2"])
                    mat.node_tree.links.new(mix_node.outputs["Color"], bsdf.inputs["Base Color"])
                    print(f"[TEX] mat={ir_mat.index} detail={detail_bitmap!r} -> MixRGB Multiply")
                except Exception as e:
                    print(f"Failed to load detail texture {detail_path}: {e}")

        # Wire bump/normal map (slot 3)
        # NovaLogic's engine uses height-based bump mapping, NOT tangent-space
        # normal maps. The bump data comes from:
        #   - Type 4 (NORMAL_MDT): .mdt files — proprietary format, can't load
        #   - Type 5 (NORMAL_TGA): TGA alpha channel contains height data
        #   - Stage1:alpha: diffuse texture alpha = height map (DOT3 shaders)
        # Use ShaderNodeBump (height→normal) instead of ShaderNodeNormalMap.
        normal_bitmap = mat.get("ase_normal_bitmap", "")
        normal_type = mat.get("ase_normal_type", 0)
        bump_wired = False
        if normal_bitmap and normal_type != 4:  # Skip MDT — Blender can't load
            tex_path = self._resolve_texture(normal_bitmap)
            if tex_path:
                try:
                    normal_tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
                    normal_tex.name = f"Bump_{normal_bitmap}"
                    existing = bpy.data.images.get(os.path.basename(tex_path))
                    if existing:
                        normal_tex.image = existing
                    else:
                        normal_tex.image = bpy.data.images.load(tex_path)
                    normal_tex.image.colorspace_settings.name = "Non-Color"
                    bump_node = mat.node_tree.nodes.new("ShaderNodeBump")
                    if normal_type == 5:
                        # NORMAL_TGA: height data is in the alpha channel
                        mat.node_tree.links.new(normal_tex.outputs["Alpha"], bump_node.inputs["Height"])
                    else:
                        mat.node_tree.links.new(normal_tex.outputs["Color"], bump_node.inputs["Height"])
                    mat.node_tree.links.new(bump_node.outputs["Normal"], bsdf.inputs["Normal"])
                    bump_wired = True
                except Exception as e:
                    print(f"Failed to load bump map {tex_path}: {e}")

        # Fallback: for bump shaders with no separate bump texture,
        # the diffuse texture's alpha channel IS the height map.
        # Shader types 1-6 (DOT3, PHONGT, BUMP) all use diffuse alpha as bump.
        _bump_shaders = ("DOT3", "PHONGT", "BUMP")
        if not bump_wired and tex_node and any(s in shader for s in _bump_shaders):
            bump_node = mat.node_tree.nodes.new("ShaderNodeBump")
            mat.node_tree.links.new(tex_node.outputs["Alpha"], bump_node.inputs["Height"])
            mat.node_tree.links.new(bump_node.outputs["Normal"], bsdf.inputs["Normal"])

        # Handle EMISSIVE flag / emissive_type
        if ir_mat.flags & 0x08:
            if "Emission Strength" in bsdf.inputs:
                bsdf.inputs["Emission Strength"].default_value = 1.0

        # Glass / reflection
        if ir_mat.is_glass:
            if "Transmission Weight" in bsdf.inputs:
                bsdf.inputs["Transmission Weight"].default_value = 0.5
            elif "Transmission" in bsdf.inputs:
                bsdf.inputs["Transmission"].default_value = 0.5
            if "IOR" in bsdf.inputs:
                bsdf.inputs["IOR"].default_value = 1.45
            rc = ir_mat.reflect_color
            if rc[0] != 0.0 or rc[1] != 0.0 or rc[2] != 0.0:
                mat["reflect_color"] = [rc[0], rc[1], rc[2], rc[3]]

        # Specular intensity — in the NovaLogic engine, specular is only active
        # when shader_type selects a bump+specular or phong+specular mode.
        # The gsys_phong lookup uses fixed exponents (pow 4/16/64).
        # Map specular_intensity to both Specular IOR Level and Roughness.
        if ir_mat.specular_intensity > 0:
            spec = min(ir_mat.specular_intensity / 255.0, 1.0)
            if "Specular IOR Level" in bsdf.inputs:
                bsdf.inputs["Specular IOR Level"].default_value = spec
            elif "Specular" in bsdf.inputs:
                bsdf.inputs["Specular"].default_value = spec
            # Derive roughness from specular: higher specular = lower roughness.
            # The phong LUT exponents (4-64) produce relatively tight highlights,
            # so map 0->1.0 roughness, 255->0.3 roughness.
            bsdf.inputs["Roughness"].default_value = 1.0 - spec * 0.7

        # Phong shader minimum specular for round-trip fidelity.
        # Without this, the exporter can't detect Phong (specular=0 → DOT3 path).
        _phong_shaders = ("PHONGT", "PHONGO", "BUMPPHONG", "ENVPHONG")
        if any(s in shader for s in _phong_shaders):
            spec_key = "Specular IOR Level" if "Specular IOR Level" in bsdf.inputs else "Specular"
            if spec_key in bsdf.inputs and bsdf.inputs[spec_key].default_value == 0:
                bsdf.inputs[spec_key].default_value = 0.3
            if bsdf.inputs["Roughness"].default_value >= 1.0:
                bsdf.inputs["Roughness"].default_value = 0.5

        # Luminosity — drives emission in the engine
        if ir_mat.luminosity > 0:
            lum = min(ir_mat.luminosity / 255.0, 1.0)
            if "Emission Strength" in bsdf.inputs:
                bsdf.inputs["Emission Strength"].default_value = lum

        # UV tiling (apply Mapping node if tiling != 0 and != 1)
        u_tile = ir_mat.u_tiling
        v_tile = ir_mat.v_tiling
        if (u_tile != 0.0 and u_tile != 1.0) or (v_tile != 0.0 and v_tile != 1.0):
            if u_tile == 0.0:
                u_tile = 1.0
            if v_tile == 0.0:
                v_tile = 1.0
            uv_node = mat.node_tree.nodes.new("ShaderNodeUVMap")
            mapping = mat.node_tree.nodes.new("ShaderNodeMapping")
            mapping.inputs["Scale"].default_value = (u_tile, v_tile, 1.0)
            mat.node_tree.links.new(uv_node.outputs["UV"], mapping.inputs["Vector"])
            # Wire mapping output to all existing texture image nodes
            for node in mat.node_tree.nodes:
                if node.type == "TEX_IMAGE":
                    mat.node_tree.links.new(mapping.outputs["Vector"], node.inputs["Vector"])

        # Store shader animation parameters as custom properties for round-trip
        if ir_mat.u_params.style != 0:
            mat["uv_u_style"] = int(ir_mat.u_params.style)
            mat["uv_u_rate"] = ir_mat.u_params.gen_rate
            mat["uv_u_phase"] = ir_mat.u_params.phase
            mat["uv_u_start"] = ir_mat.u_params.start
            mat["uv_u_end"] = ir_mat.u_params.end
        if ir_mat.v_params.style != 0:
            mat["uv_v_style"] = int(ir_mat.v_params.style)
            mat["uv_v_rate"] = ir_mat.v_params.gen_rate
            mat["uv_v_phase"] = ir_mat.v_params.phase
            mat["uv_v_start"] = ir_mat.v_params.start
            mat["uv_v_end"] = ir_mat.v_params.end
        if ir_mat.alpha_gen.style != 0:
            mat["alpha_gen_style"] = int(ir_mat.alpha_gen.style)
            mat["alpha_gen_rate"] = ir_mat.alpha_gen.rate
            mat["alpha_gen_phase"] = ir_mat.alpha_gen.phase
            mat["alpha_gen_start"] = int(ir_mat.alpha_gen.start)
            mat["alpha_gen_end"] = int(ir_mat.alpha_gen.end)
        if ir_mat.rgb_gen.style != 0:
            mat["rgb_gen_style"] = int(ir_mat.rgb_gen.style)
            mat["rgb_gen_rate"] = ir_mat.rgb_gen.rate
            mat["rgb_gen_phase"] = ir_mat.rgb_gen.phase
            sc = ir_mat.rgb_gen.start_color
            ec = ir_mat.rgb_gen.end_color
            mat["rgb_gen_start_color"] = [sc[0], sc[1], sc[2], sc[3]]
            mat["rgb_gen_end_color"] = [ec[0], ec[1], ec[2], ec[3]]
        if ir_mat.animation.num_frames > 0:
            mat["tex_anim_frames"] = int(ir_mat.animation.num_frames)
            mat["tex_anim_type"] = int(ir_mat.animation.animation_type)
            mat["tex_anim_time"] = int(ir_mat.animation.cycle_frame_time)
        if ir_mat.alpha_threshold > 0:
            mat["alpha_threshold"] = ir_mat.alpha_threshold

        # Store emissive_type for round-trip (0=none, 2=full)
        if ir_mat.emissive_type != 0:
            mat["emissive_type"] = int(ir_mat.emissive_type)

        # Store control register names for round-trip (resolved from IR indices)
        if ir_mat.rgb_gen.style > 0x70 and ir_mat.rgb_gen.reg >= 0:
            creg = self._resolve_ctrl_reg(ir_mat.rgb_gen.reg)
            if creg:
                mat["rgb_gen_ctrlreg"] = creg
        if ir_mat.alpha_gen.style > 0x70 and ir_mat.alpha_gen.reg >= 0:
            creg = self._resolve_ctrl_reg(ir_mat.alpha_gen.reg)
            if creg:
                mat["alpha_gen_ctrlreg"] = creg
        if ir_mat.u_params.style > 0x70 and ir_mat.u_params.reg >= 0:
            creg = self._resolve_ctrl_reg(ir_mat.u_params.reg)
            if creg:
                mat["uv_u_ctrlreg"] = creg
        if ir_mat.v_params.style > 0x70 and ir_mat.v_params.reg >= 0:
            creg = self._resolve_ctrl_reg(ir_mat.v_params.reg)
            if creg:
                mat["uv_v_ctrlreg"] = creg

        return mat

    def _resolve_texture(self, texture_name: str) -> str | None:
        if not texture_name or not self.resolver:
            return None
        return self.resolver.resolve_texture(texture_name)

    def _create_marker_material(self, name: str, color: tuple):
        if name in bpy.data.materials:
            return bpy.data.materials[name]
        mat = bpy.data.materials.new(name)
        mat.use_nodes = True
        mat.node_tree.nodes.clear()
        bsdf = mat.node_tree.nodes.new("ShaderNodeBsdfPrincipled")
        output = mat.node_tree.nodes.new("ShaderNodeOutputMaterial")
        mat.node_tree.links.new(bsdf.outputs["BSDF"], output.inputs["Surface"])
        bsdf.inputs["Base Color"].default_value = (*color, 1.0)
        bsdf.inputs["Roughness"].default_value = 1.0
        if "Emission Color" in bsdf.inputs:
            bsdf.inputs["Emission Color"].default_value = (*color, 1.0)
        elif "Emission" in bsdf.inputs:
            bsdf.inputs["Emission"].default_value = (*color, 1.0)
        if "Emission Strength" in bsdf.inputs:
            bsdf.inputs["Emission Strength"].default_value = 0.3
        return mat
