// Shared script variable store.
//
// THE key shared-state seam between the two scripting systems: the original's
// mission-variable array dword_C6B240 is written by BOTH WAC (`set(V#)`,
// WacScript_ResolveParameter resolves V# -> 4*idx+0xC6B240) AND the BMS event
// system (EventAction_Dispatch MisvarChange writes dword_C6B240[idx];
// EventTrigger_EvaluateCondition cat 4 compares it). They are literally the same
// storage, so a single ScriptVarStore backs both evaluators.
//
// Globals G# (dword_C6BA40) persist across mission link; mission V# clear at
// mission start; music M# is a small parallel bank.
#ifndef OPENNOVA_WORLD_VAR_STORE_H
#define OPENNOVA_WORLD_VAR_STORE_H

#include <array>
#include <cstdint>

namespace opennova::world {

class ScriptVarStore {
public:
    static constexpr int kMissionVars = 512; // V0..V511  [orig: 0xC6B240]
    static constexpr int kGlobalVars = 256;  // G0..G255  [orig: 0xC6BA40]
    static constexpr int kMusicVars = 16;    // M0..M15

    // Values are raw 32-bit (16.16 fixed where the slot holds a scaled scalar),
    // matching the original's int storage.
    int32_t get_mission(int i) const { return in_range(i, kMissionVars) ? mission_[i] : 0; }
    void set_mission(int i, int32_t v) { if (in_range(i, kMissionVars)) mission_[i] = v; }

    int32_t get_global(int i) const { return in_range(i, kGlobalVars) ? global_[i] : 0; }
    void set_global(int i, int32_t v) { if (in_range(i, kGlobalVars)) global_[i] = v; }

    int32_t get_music(int i) const { return in_range(i, kMusicVars) ? music_[i] : 0; }
    void set_music(int i, int32_t v) { if (in_range(i, kMusicVars)) music_[i] = v; }

    // V# cleared at mission start; G# preserved across link (CP03.wac spec).
    void clear_mission() { mission_.fill(0); }
    void clear_all() { mission_.fill(0); global_.fill(0); music_.fill(0); }

private:
    static bool in_range(int i, int n) { return i >= 0 && i < n; }
    std::array<int32_t, kMissionVars> mission_{};
    std::array<int32_t, kGlobalVars> global_{};
    std::array<int32_t, kMusicVars> music_{};
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_VAR_STORE_H
