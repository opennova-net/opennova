#pragma once

#include <array>
#include <string>
#include <vector>

namespace opennova::object::project {

constexpr int kMaxLods = 8;
constexpr int kMaxAnimFrames = 8;

struct AnimFrame {
  std::string path;
  int enabled = 0;
};

struct Material {
  std::string name;
  std::string shader_tag;
  int rattrib = 0;
  int pattrib = 0;
  int ptype = 0;
  int geofx = 0;
  float geofx_value = 0.0f;
  int alphatestvalue = 0;
  std::array<std::string, 2> diffuse_tex{};
  std::array<int, 2> diffuse_flags{};
  std::array<std::string, 2> normal_tex{};
  std::array<int, 2> normal_flags{};
  int anim_frames = 0;
  int anim_type = 0;
  int anim_frametime = 0;
  std::string anim_ctrlreg;
  std::array<std::array<AnimFrame, kMaxAnimFrames>, 2> anim_diffuse{};
  std::array<std::array<AnimFrame, kMaxAnimFrames>, 2> anim_normal{};
  std::array<int, 3> reflect_rgb{};
  int rgbgen_style = 0;
  float rgbgen_rate = 0.0f;
  float rgbgen_phase = 0.0f;
  std::array<int, 3> rgbgen_srgb{};
  std::array<int, 3> rgbgen_ergb{};
  std::string rgbgen_ctrlreg;
  int alphagen_style = 0;
  float alphagen_rate = 0.0f;
  float alphagen_phase = 0.0f;
  float alphagen_start = 0.0f;
  float alphagen_end = 0.0f;
  std::string alphagen_ctrlreg;
  int mapfunc_u_style = 0;
  float mapfunc_u_rate = 0.0f;
  float mapfunc_u_phase = 0.0f;
  float mapfunc_u_start = 0.0f;
  float mapfunc_u_end = 0.0f;
  std::string mapfunc_u_ctrlreg;
  int mapfunc_v_style = 0;
  float mapfunc_v_rate = 0.0f;
  float mapfunc_v_phase = 0.0f;
  float mapfunc_v_start = 0.0f;
  float mapfunc_v_end = 0.0f;
  std::string mapfunc_v_ctrlreg;
};

struct AxisFunc {
  int func_id = 0;
  float param0 = 0.0f;
  float param1 = 0.0f;
  float param2 = 0.0f;
  float param3 = 0.0f;
  std::string ctrl_reg;
};

struct PartAnim {
  int rotate_type = 0;
  int scale_type = 0;
  int trans_type = 0;
  int transform_as = 0;
  float yaw_rate = 0.0f;
  float pitch_rate = 0.0f;
  float roll_rate = 0.0f;
  AxisFunc yaw;
  AxisFunc pitch;
  AxisFunc roll;
  int reverse_rotate = 0;
  AxisFunc scale;
  AxisFunc scale_x;
  AxisFunc scale_y;
  AxisFunc scale_z;
  AxisFunc trans_x;
  AxisFunc trans_y;
  AxisFunc trans_z;
};

struct Light {
  std::string name;
  int colorgen_style = 0;
  float colorgen_rate = 0.0f;
  float colorgen_phase = 0.0f;
  std::array<int, 3> colorgen_start{};
  std::array<int, 3> colorgen_end{};
  std::string colorgen_ctrlreg;
  int disable_corona = 0;
  int disable_lightterrain = 0;
  int disable_lightobjects = 0;
};

struct Lod {
  std::string scene_file;      // token from 3dp (may include .ase)
  std::string scene_base_name; // without extension
  int attributes = 0;
  std::string render_function;
  float threshold = 0.0f;
  bool part_anim_enabled = false;
  std::vector<PartAnim> part_anims;
  std::vector<Light> lights;
};

struct Project {
  std::string source_path;  // path of the .3dp file
  std::string base_dir;     // parent directory of the .3dp
  int version = 0;
  int poly_collision_lod = 0;
  std::vector<Material> materials;
  std::array<Lod, kMaxLods> lods{};

  // Convenience: first LOD ASE path (base_dir + scenename) if present.
  std::string ase_path;
};

}  // namespace opennova::object::project
