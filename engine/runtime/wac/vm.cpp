#include <runtime/wac/vm.h>
#include <formats/mus/mus.h>

#include <algorithm>
#include <cmath>

#include <formats/wac/bytecode.h>
#include <formats/wac/command.h>
#include <formats/wac/help.h>
#include <runtime/wac/remote_command.h>
#include <runtime/wac/retail_ftol.h>
#include <runtime/world/world.h>

#include <base/io/strutil.h>

namespace opennova::wac {
namespace {

bool ieq(const char *a, const char *b) { return opennova::strutil::iequals(a, b); }

uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

// [orig: Math_PowFloat @0x4F9BA0] binary32 input, exponentiation by
// squaring, then the VM keeps EAX from the SSE2 conversion.
// [orig: WacScript_ExecuteBytecode @0x4F615F]
int32_t power_fold(int32_t base, int32_t exponent) {
    double square = static_cast<float>(base);
    uint32_t magnitude = exponent < 0 ? 0u - uint32_t(exponent) : uint32_t(exponent);
    double result = 1.0;
    do {
        if (magnitude & 1u) result *= square;
        magnitude >>= 1;
        if (magnitude) square *= square;
    } while (magnitude);
    if (exponent < 0) result = 1.0 / result;
    return retail_ftol_sse2(result);
}


} // namespace

void WacVm::load(const Program &program) {
    prog_ = &program;
    events_.assign(static_cast<size_t>(program.event_count > 0 ? program.event_count : 1),
                   EventState{});
    loop_counters_.assign(program.loop_count, 0);
    loop_choices_.assign(program.loop_count, 0);
    rng_seed_ = 0x12333333u; // [orig: WacScript_InitAndLoad @ 0x4f966b]
    acc_ = 0;
    cur_event_ = 0;
    time_ = 0;
    dispatch_count_ = 0;
    entity_bindings_.clear();
    groups_.clear();
    auto_item_ = 0xFFFF;
}

WacVm::RuntimeState WacVm::capture_runtime_state() const {
    RuntimeState state;
    state.events = events_;
    state.loop_counters = loop_counters_;
    state.loop_choices = loop_choices_;
    state.rng_seed = rng_seed_;
    state.accumulator = acc_;
    state.current_event = cur_event_;
    state.time = time_;
    state.entity_bindings = entity_bindings_;
    state.auto_item = auto_item_;
    return state;
}

void WacVm::restore_runtime_state(const Program &program,
                                  const RuntimeState &state) {
    prog_ = &program;
    events_ = state.events;
    loop_counters_ = state.loop_counters;
    loop_choices_ = state.loop_choices;
    rng_seed_ = state.rng_seed;
    acc_ = state.accumulator;
    cur_event_ = state.current_event;
    time_ = state.time;
    entity_bindings_ = state.entity_bindings;
    auto_item_ = state.auto_item;
    groups_.clear();
}

uint32_t WacVm::next_rand() {
    // [orig: WacScript_ExecuteBytecode DORND @ 0x4f5a83..0x4f5a91 — rol 9,
    //  then the SIGNED carry: sar edx,1Fh; and edx,1ABB09h; add. Same idiom
    //  (and same seed) as the weather PRNG @ 0x57e9fc (env #22/#25).]
    uint32_t v = rol32(rng_seed_, 9);
    v += static_cast<uint32_t>(static_cast<int32_t>(v) >> 31) & 0x1ABB09u;
    rng_seed_ = v;
    return v;
}

int32_t WacVm::read(opennova::world::World &w, uint32_t ref) const {
    switch (operand_kind(ref)) {
        case OperandKind::Pool: {
            uint32_t i = operand_index(ref);
            return (i < prog_->operands.size()) ? prog_->operands[i] : 0;
        }
        case OperandKind::EntitySsn: {
            const uint32_t i = operand_index(ref);
            return i < entity_bindings_.size() ? entity_bindings_[i] : 0xFFFF;
        }
        case OperandKind::MissionVar:
            return w.script.vars.get_mission(static_cast<int>(operand_index(ref)));
        case OperandKind::GlobalVar:
            return w.script.vars.get_global(static_cast<int>(operand_index(ref)));
        case OperandKind::MusicVar:
            return prog_->music_globals ? mus::mus_globals_read(*prog_->music_globals, operand_index(ref)) : 0;
        case OperandKind::EventFired: {
            const uint32_t i = operand_index(ref);
            return i < events_.size() && events_[i].ever_fired;
        }
        case OperandKind::Builtin: {
            switch (static_cast<Builtin>(operand_index(ref))) {
                case Builtin::AutoItem: return static_cast<int32_t>(auto_item_);
                case Builtin::SquadSSN: return w.script.squad_events.selected_ssn;
                case Builtin::SquadWho: return w.script.squad_events.selected_who;
                case Builtin::RandomResult: return w.script.wac_values.random_result;
                case Builtin::Ticks: return static_cast<int32_t>(time_); // VM executions [orig: wac_var_ticks]
                case Builtin::Result: return acc_;
                case Builtin::Health: return w.cached.local_health;
                case Builtin::Wind: return w.weather.wind_scale();   // Env_WindScale [orig: @0x26c68c0]
                case Builtin::Mana: return cached_mana_;
                case Builtin::CurTOD: return cached_tod_;
                case Builtin::Bluekills: return w.kill_stats.bluekills_by_player;   // [orig: 0xC846F0]
                case Builtin::Greenkills: return w.kill_stats.greenkills_by_player; // [orig: 0xC846F8]
                case Builtin::Humans: return w.cached.humans;                       // [orig: 0xC6EB14]
                // Refreshed once at bytecode entry; a win/lose command does
                // not change these cached words until the next execution.
                // [orig: WacScript_CacheLocalPlayerState @0x4F57BB/C9/CF]
                case Builtin::GameOver: return cached_game_over_;
                case Builtin::WinVar: return cached_win_;
                case Builtin::LoseVar: return cached_lose_;
                case Builtin::AccuracySpread: return w.script.wac_values.accuracy_spread;
                case Builtin::Fallmps: return w.script.wac_values.fallmps;               // [orig: 0xC6EAE4]
				case Builtin::Seatbelt:
					return w.script.wac_values.seatbelt;
				case Builtin::Night: return w.weather.night_phase;   // Env_IsNightPhase [orig: @0x26c645c]
                case Builtin::Breathtime: return w.script.wac_values.breathtime; // [orig: 0xC6EAE0]
                case Builtin::Autogain: return w.script.wac_values.autogain;     // [orig: wac_var_autogain 0xC6EAFC]
                case Builtin::Scratch: return scratch_;                          // [orig: dword_C6EAEC]
            }
            return 0;
        }
    }
    return 0;
}

void WacVm::write(opennova::world::World &w, uint32_t ref, int32_t v) {
    switch (operand_kind(ref)) {
        case OperandKind::MissionVar: w.script.vars.set_mission(static_cast<int>(operand_index(ref)), v); break;
        case OperandKind::GlobalVar: w.script.vars.set_global(static_cast<int>(operand_index(ref)), v); break;
        case OperandKind::MusicVar:
            if (prog_->music_globals) mus::mus_globals_write_raw(*prog_->music_globals, operand_index(ref), v);
            break;
        case OperandKind::Builtin:
            // The retail named-value resolver returns the address of the row's
            // mutable engine dword, so ordinary set/add/sub/inc/dec/store write
            // through to every row. [orig: WacScript_ResolveParameter
            // @0x4f2a92..0x4f2a9f -> the table @0x82EEF0]
            switch (static_cast<Builtin>(operand_index(ref))) {
                case Builtin::AutoItem: auto_item_ = static_cast<uint32_t>(v); break; // [orig: WacCmd_Set @0x4ED520]
                case Builtin::SquadSSN: w.script.squad_events.selected_ssn = v; break;
                case Builtin::SquadWho: w.script.squad_events.selected_who = v; break;
                case Builtin::RandomResult: w.script.wac_values.random_result = v; break;
                case Builtin::Ticks: time_ = static_cast<uint32_t>(v); break;   // [orig: wac_var_ticks 0xC6EAD8]
                case Builtin::Result: acc_ = v; break;                           // [orig: wac_var_result 0xC6EB24]
                case Builtin::Health: w.cached.local_health = v; break;
                case Builtin::Mana: cached_mana_ = v; break;
                case Builtin::CurTOD: cached_tod_ = v; break;
                case Builtin::GameOver: cached_game_over_ = v; break;
                case Builtin::WinVar: cached_win_ = v; break;
                case Builtin::LoseVar: cached_lose_ = v; break;
                case Builtin::Bluekills: w.kill_stats.bluekills_by_player = v; break;   // [orig: 0xC846F0]
                case Builtin::Greenkills: w.kill_stats.greenkills_by_player = v; break; // [orig: 0xC846F8]
                // Rebuilt by the next slot-list pass. [orig: wac_var_humans 0xC6EB14]
                case Builtin::Humans: w.cached.humans = v; break;
                case Builtin::AccuracySpread: w.script.wac_values.accuracy_spread = v; break; // [orig: 0xC6EAE8]
                case Builtin::Fallmps: w.script.wac_values.fallmps = v; break;       // [orig: 0xC6EAE4]
                case Builtin::Seatbelt: w.script.wac_values.seatbelt = v; break;     // [orig: 0xC6EADC]
                case Builtin::Breathtime: w.script.wac_values.breathtime = v; break; // [orig: 0xC6EAE0]
                case Builtin::Autogain: w.script.wac_values.autogain = v; break;     // [orig: 0xC6EAFC]
                case Builtin::Wind: w.commands.set_wind_scale(v); break; // Env_WindScale [orig: the `wind` row @0x82EEF0]
                case Builtin::Scratch: scratch_ = v; break;               // [orig: dword_C6EAEC]
                case Builtin::Night: w.weather.night_phase = v; break; // [orig: @0x26C645C]
            }
            break;
        default: break; // pool values are not lvalues
    }
}

int32_t WacVm::current_value(opennova::world::World &w, uint32_t ref) const {
    switch (operand_kind(ref)) {
        case OperandKind::MissionVar:
        case OperandKind::GlobalVar:
        case OperandKind::EventFired:
        case OperandKind::Builtin:
            return read(w, ref);
        default:
            return 0;
    }
}

// The string a string-typed parameter hands its handler. Text and Filename
// are raw slots: the call passes the operand's ADDRESS and the handler reads
// the bytes there [orig: WacScript_ExecuteBytecode @0x4F5F92 (case 5),
// @0x4F5FA9 (case 6), @0x4F6012 (case 9)]. A string operand is the
// string-pool copy the resolver made [orig: WacScript_ResolveParameter
// @0x4F2E16]; any other operand is a dword whose bytes run, least
// significant first, up to the first NUL, on into the words stored after it:
// the numbered, declared and global banks sit end to end, and the value pool
// is one array. The model ends where those blocks end (the IF-tick array
// follows the globals), and an engine, event or music dword stands alone.
// A TextToken slot's dword is the text pointer the resolver pooled
// [orig: @0x4F2FAE], here the index of the program's text token.
std::string WacVm::operand_string(opennova::world::World &w, uint32_t ref, ParamType type) const {
    if (type == ParamType::TextToken) {
        const int32_t token = read(w, ref);
        return token >= 0 && size_t(token) < prog_->text_tokens.size()
                ? prog_->text_tokens[size_t(token)].text : std::string();
    }
    if (type != ParamType::Text && type != ParamType::Filename) return std::string();
    const uint32_t index = operand_index(ref);
    if (operand_kind(ref) == OperandKind::Text) return prog_->text_at(index);
    std::string text;
    // False once the dword held a NUL: the string ended inside it.
    const auto append = [&text](int32_t word) {
        for (int shift = 0; shift < 32; shift += 8) {
            const char byte = static_cast<char>((uint32_t(word) >> shift) & 0xFFu);
            if (byte == '\0') return false;
            text += byte;
        }
        return true;
    };
    const auto &vars = w.script.vars;
    switch (operand_kind(ref)) {
        case OperandKind::MissionVar: {
            int i = static_cast<int>(index);
            while (i < world::ScriptVarStore::kMissionVars && append(vars.get_mission(i))) ++i;
            if (i < world::ScriptVarStore::kMissionVars) break;
            for (int g = 0; g < world::ScriptVarStore::kGlobalVars && append(vars.get_global(g)); ++g) {}
            break;
        }
        case OperandKind::GlobalVar:
            for (int g = static_cast<int>(index); g < world::ScriptVarStore::kGlobalVars &&
                    append(vars.get_global(g)); ++g) {}
            break;
        case OperandKind::Pool:
            for (size_t i = index; i < prog_->operands.size() && append(prog_->operands[i]); ++i) {}
            break;
        default:
            append(read(w, ref));
            break;
    }
    return text;
}

int32_t WacVm::dispatch(opennova::world::World &w, int cmd, const uint32_t *args, int argc, uint32_t instruction) {
    ++dispatch_count_;
    if (cmd < 0 || cmd >= wac_command_count()) {
        record_gap(w, cmd, instruction);
        return 0;
    }
    const CommandDef &def = wac_commands()[cmd];
    // A command whose registry flags carry 0x18 is serialized for S2C 0x23
    // before, or instead of, its local call. [orig: WacScript_ExecuteBytecode
    // @0x4F58B0 — flags test @0x4f5ca5, the arm through @0x4f5ee9]
    if (cmd_is_replicated(def)) return replicate(w, cmd, def, args, argc, instruction);
    const char *n = def.name;
    auto A = [&](int i) -> int32_t {
        if (i >= argc || args == nullptr) return 0;
        return read(w, args[i]);
    };
    auto H = [&](int i) -> world::EntityHandle {
        return i < argc && args != nullptr ? world::EntityHandle{uint16_t(A(i))}
                                          : world::EntityHandle{};
    };
    auto S = [&](int i) -> std::string {
        if (i >= argc || !args || i >= 4) return std::string();
        return operand_string(w, args[i], def.params[i]);
    };
    EventState &es = events_[(cur_event_ >= 0 && cur_event_ < static_cast<int>(events_.size())) ? cur_event_ : 0];
    auto &cmds = w.commands;
    const auto FX = [&](int i) -> std::string {
        const int32_t handle = A(i);
        return handle > 0 && size_t(handle) <= prog_->effect_names.size()
                ? prog_->effect_names[size_t(handle) - 1] : std::string();
    };

    // [orig: WacCmd_Reset @0x4ED300] Reset this event and its descendants;
    // adjacent events at the same depth retain their history.
    if (ieq(n, "reset")) {
        const int event = A(0);
        if (event >= 0 && size_t(event) < prog_->event_depths.size()) {
            const uint16_t depth = prog_->event_depths[event];
            events_[event] = {};
            for (size_t i = size_t(event) + 1; i < events_.size() &&
                    i < prog_->event_depths.size() && prog_->event_depths[i] > depth; ++i)
                events_[i] = {};
        }
        return 1;
    }

    if (ieq(n, "weaponfired")) return w.script.weapon_input.fire_requested(A(0));
    if (ieq(n, "blockfire")) return w.script.weapon_input.set_fire_blocked(A(0), A(1) != 0);

    // ---- temporal / event conditions ----
    if (ieq(n, "never")) return es.ever_fired ? 0 : 1;
    // Subtract in unsigned storage before the signed comparison, as the
    // retail dword arithmetic does across the tick counter's wrap.
    // [orig: WacCmd_Past @0x4ED010; WacCmd_Before @0x4ED030]
    if (ieq(n, "past")) return static_cast<int32_t>(time_ - uint32_t(A(0))) >= 0;
    if (ieq(n, "before")) return static_cast<int32_t>(time_ - uint32_t(A(0))) < 0;
    if (ieq(n, "ontick")) return static_cast<int32_t>(time_) == A(0) ? 1 : 0;
    if (ieq(n, "onptick")) {
        // The selected player's slot play-tick dword in whole seconds (a
        // truncating signed /62); an entity without an active slot reads 0.
        // [orig: WacCmd_OnPlayerTick @0x4F0E10 — the Entity_ValidatePtr call
        //  @0x4F0E58, `mov ecx,[eax+184h]` @0x4F0E65, /62 @0x4F0E6B..0x4F0E7C,
        //  the compare @0x4F0E7E..0x4F0E86]
        const world::EntityHandle handle{static_cast<uint16_t>(auto_item_)};
        const world::MatchPlayer *player = w.match.player(handle);
        if (w.registry.get(handle) == nullptr || player == nullptr) return 0;
        return static_cast<int32_t>(player->play_ticks) / 62 == A(0) ? 1 : 0;
    }
    if (ieq(n, "elapse")) {
        // An event which has never fired is immediately eligible.
        // [orig: WacCmd_Elapse @0x4ECEF0]
        return !es.ever_fired ||
                static_cast<int32_t>(time_ - es.last_fired_tick - uint32_t(A(0))) >= 0;
    }
    if (ieq(n, "previous") || ieq(n, "chain")) {
        // Skip descendants of the preceding event. The predecessor must
        // have fired more recently than this event; tick-zero stamps retain
        // their retail strict-comparison behavior.
        // [orig: WacCmd_Chain @0x4ECF20; WacCmd_Previous @0x4ECF90]
        const auto depth = [&](int event) {
            return event >= 0 && size_t(event) < prog_->event_depths.size()
                    ? prog_->event_depths[event] : uint16_t(0);
        };
        int preceding = cur_event_ - 1;
        while (preceding >= 0 && depth(preceding) > depth(cur_event_)) --preceding;
        if (preceding < 0 || size_t(preceding) >= events_.size()) return 0;
        const EventState &prior = events_[preceding];
        if (!prior.ever_fired ||
                static_cast<int32_t>(es.last_fired_tick - prior.last_fired_tick) >= 0)
            return 0;
        const uint32_t delay = ieq(n, "chain") ? uint32_t(A(0)) : 0u;
        return static_cast<int32_t>(time_ - prior.last_fired_tick - delay) >= 0;
    }

    // ---- group / entity state conditions ----
    if (ieq(n, "groupdead")) return cmds.group_dead(A(0)) ? 1 : 0;
    if (ieq(n, "groupalive")) return cmds.group_alive(A(0)) ? 1 : 0;
    if (ieq(n, "SSNdead")) return cmds.wac_ssn_dead(H(0)) ? 1 : 0;
    if (ieq(n, "SSNalive")) return cmds.wac_ssn_alive(H(0)) ? 1 : 0;
    if (ieq(n, "SSNexists")) return cmds.ssn_exists(H(0)) ? 1 : 0;
    if (ieq(n, "SSNLeadSSN2SSN")) return cmds.ssn_leads_target(H(0), H(1), H(2), A(3));
    if (ieq(n, "fxrain")) return A(0) != 0 ? cmds.rain_effect(A(0), FX(0), next_rand()) : 1;
    if (ieq(n, "ammo2tgt")) return cmds.fire_ammo_at_target(A(0), A(1));
    if (ieq(n, "ammo2ssn")) return cmds.fire_ammo_from_ssn(A(0), H(1), H(2));
    if (ieq(n, "ammoarea")) return cmds.fire_ammo_in_area(A(0), A(1));
    if (ieq(n, "ammorain"))
        return A(0) != 0 ? cmds.rain_ammo_near_player(A(0), next_rand()) : 1;
    if (ieq(n, "teleport")) { cmds.teleport_group_to_marker(A(0), A(1)); return 0; }
    if (ieq(n, "SSNcritical")) return cmds.ssn_critical(H(0));
    if (ieq(n, "SSNride")) return cmds.ssn_has_rider(H(0));
    if (ieq(n, "ssnname")) return cmds.set_ssn_name(H(0), S(1));
    if (ieq(n, "ssn2ssn")) return cmds.order_boarding(H(0), H(1));
    if (ieq(n, "SSNarea") || ieq(n, "SSNarea3D"))
        return cmds.ssn_in_script_area(H(0), A(1), ieq(n, "SSNarea3D"));
    if (ieq(n, "SSNloc")) return cmds.ssn_at_location(H(0), A(1));
    if (ieq(n, "SSNwounded"))
        return cmds.ssn_wounded(H(0)) ? 1 : 0;
    if (ieq(n, "SSNnearSSN"))
        return cmds.ssn_within_distance(H(0),
                                        H(1), A(2)) ? 1 : 0;
    if (ieq(n, "SSNlosSSN"))
        return cmds.ssn_los_clear_within(H(0),
                                         H(1), A(2)) ? 1 : 0;
    if (ieq(n, "SSNseesSSN"))
        return cmds.ssn_sees_within(H(0),
                                    H(1), A(2)) ? 1 : 0;

    // ---- comparison / value functions ----
    // ---- the local-player condition family (the co-op choreography gates:
    // 05TRcoop's vehicle chains are `if area(N) and eq(vX,..) and not
    // meride(SSN) then set(vX,..)` — with these unimplemented every chain
    // evaluated false and no scripted vehicle ever received its drive order).
    // Schema rows: area idx111 @0x4ED0C0, meride idx116 @0x4F1260, SSNonSSN
    // idx16 @0x4F19A0 (see formats/wac/command_table_data.cpp).
    // [orig: WacCmd_Area @0x4ED0C0; Area3D @0x4ED120]
    if (ieq(n, "area") || ieq(n, "area3D"))
        return cmds.ssn_in_script_area(w.cached.local_player, A(0), ieq(n, "area3D"));
    if (ieq(n, "outside")) {
        // [orig: WacCmd_Outside @0x4ED050] First blink hit, no indoor-flag proxy.
        const world::Entity *player = w.registry.get(w.cached.local_player);
        return player != nullptr && player->blink_hits[0] == 0;
    }
    if (ieq(n, "location")) return w.script.wac_values.local_location == A(0);
    if (ieq(n, "meride")) return cmds.local_player_standing_on_ssn(H(0));
    if (ieq(n, "meattached"))
        return cmds.local_player_attached_to_ssn(H(0)) ? 1 : 0;
    if (ieq(n, "medrive"))
        return cmds.local_player_driving_ssn(H(0)) ? 1 : 0;
    if (ieq(n, "meongun"))
        return cmds.local_player_on_gun_of_ssn(H(0)) ? 1 : 0;
    if (ieq(n, "SSNonSSN")) {
        // [orig: Entity_IsOnTopOfChain @0x4F19A0 — B reachable from A's
        // groundEntity chain within 3 hops]
        return cmds.ssn_on_chain_of(H(0),
                                    H(1)) ? 1 : 0;
    }
    if (ieq(n, "SSNSpawn")) return cmds.set_ssn_respawns(H(0), A(1));
    if (ieq(n, "GroupSpawn")) {
        cmds.set_group_respawns(A(0), A(1));
        return 1;
    }
    if (ieq(n, "eq")) return A(0) == A(1) ? 1 : 0;
    if (ieq(n, "ne")) return A(0) != A(1) ? 1 : 0;
    if (ieq(n, "lt")) return A(0) < A(1) ? 1 : 0;
    if (ieq(n, "gt")) return A(0) > A(1) ? 1 : 0;
    if (ieq(n, "le")) return A(0) <= A(1) ? 1 : 0;
    if (ieq(n, "ge")) return A(0) >= A(1) ? 1 : 0;
    if (ieq(n, "true")) return A(0) != 0 ? 1 : 0;
    if (ieq(n, "false")) return A(0) == 0 ? 1 : 0;
    if (ieq(n, "random")) {
        // [orig: WacCmd_Random @0x4ED280] Always advance, including zero or
        // negative limits. Signed 64-bit IMUL, rounded SHRD, then low-word +1.
        const int64_t product = int64_t(A(0)) * int64_t(next_rand() & 0xFFFFu) + 0x8000;
        const int32_t result = int32_t(uint32_t(uint64_t(product) >> 16) + 1u);
        w.script.wac_values.random_result = result;
        return result == 1 ? 1 : 0;
    }
    if (ieq(n, "squadevent")) return w.script.squad_events.query(A(0));
    if (ieq(n, "squadclear")) return w.script.squad_events.clear_selected();

    // ---- variable mutation ----
    if (ieq(n, "set")) { if (argc >= 2) write(w, args[0], A(1)); return A(1); }
    if (ieq(n, "add")) { if (argc >= 2) { int32_t v = int32_t(uint32_t(read(w, args[0])) + uint32_t(A(1))); write(w, args[0], v); return v; } return 0; }
    if (ieq(n, "sub")) { if (argc >= 2) { int32_t v = int32_t(uint32_t(read(w, args[0])) - uint32_t(A(1))); write(w, args[0], v); return v; } return 0; }
    if (ieq(n, "inc")) { if (argc >= 1) { int32_t v = int32_t(uint32_t(read(w, args[0])) + 1u); write(w, args[0], v); return v; } return 0; }
    if (ieq(n, "dec")) { if (argc >= 1) { int32_t v = int32_t(uint32_t(read(w, args[0])) - 1u); write(w, args[0], v); return v; } return 0; }
    if (ieq(n, "store")) { if (argc >= 1) write(w, args[0], acc_); return acc_; }
    if (ieq(n, "load")) { return A(0); }

    // ---- entity actions ----
    // The killSSN handler (IDB misnomer). [orig: Entity_ResetWeaponState @0x4F1E40]
    if (ieq(n, "killSSN")) return cmds.wac_kill_ssn(H(0)) ? 1 : 0;
    if (ieq(n, "removeSSN")) return cmds.remove_ssn(H(0)) ? 1 : 0;
    if (ieq(n, "remove")) { cmds.remove_group(A(0)); return 0; }
    if (ieq(n, "ssnuse")) return cmds.use_boarding_target(H(0));
    if (ieq(n, "SSNHP")) return cmds.set_ssn_hp(H(0), A(1)) ? 1 : 0;
    if (ieq(n, "SSNADDHP")) return cmds.add_ssn_hp(H(0), A(1)); // 1 only when clamped
    if (ieq(n, "SSNtoWP")) return cmds.set_ssn_waypoint(H(0), A(1)) ? 1 : 0;
    if (ieq(n, "SSNMin")) return cmds.set_ssn_engage_min(H(0), A(1)) ? 1 : 0;
    if (ieq(n, "SSNMax")) return cmds.set_ssn_engage_max(H(0), A(1)) ? 1 : 0;
    if (ieq(n, "SSNAtt")) return cmds.set_ssn_attack_max(H(0), A(1)) ? 1 : 0;
    if (ieq(n, "SSNanim")) return cmds.set_ssn_anim(H(0), A(1)) ? 1 : 0;
    if (ieq(n, "anim")) return cmds.set_local_anim(A(0)) ? 0 : 1;
    // [orig: WacCmd_SsnFace @0x4F1C60]
    if (ieq(n, "ssnface")) {
        const world::Entity *entity = w.registry.get(H(0));
        if (entity == nullptr || entity->item_id == 0 || w.facials.for_entity(*entity) == nullptr)
            return 0;
        w.facials.override_expression(*entity, A(1));
        return 1;
    }

    if (ieq(n, "fall")) return cmds.raise_local_player() ? 1 : 0;
    if (ieq(n, "ssnturn")) return cmds.set_ssn_turn(H(0), A(1)) ? 1 : 0;
    if (ieq(n, "tele")) return cmds.teleport_local_to_ssn(H(0)) ? 1 : 0;
    if (ieq(n, "forceanim")) {
        // The notice rides the system ring, not the chat ring.
        // [orig: Script_ForceAnimation @0x4F2610 (the Chat_AddDebugMessage
        //  call @0x4F266A), return 0 @0x4F2682]
        w.script.forced_animation = A(0);
        const std::string key = world::infantry_anim_key(A(0));
        const std::string message = A(0) == 0 ? "force anim OFF" :
                "force " + (key.empty() ? std::to_string(A(0)) : key);
        w.out.effects.push({"debug_text", 0, 0, 0, 0, message});
        return 0;
    }
    if (ieq(n, "dropflare")) {
        // [orig: WacCmd_DropFlare @0x4F2710] Unlike SSNanim, this checks
        // the +0x20 ItemDef pointer, not the +0x1C type index or health.
        world::Entity *entity = w.registry.get(cmds.resolve_target(H(0)));
        if (entity == nullptr || !entity->has_item_def) return 0;
        w.vehicles.release_flares(*entity);
        return 1;
    }
    if (ieq(n, "IsPSPallteam")) {
        // [orig: WacCmd_IsPspAllTeam @0x4EE4B0] The spawn registry includes
        // dead points and differs from the Advance & Secure capture chain.
        const world::SpawnZoneRegistry points = w.zones.build_spawn_zone_list();
        if (points.empty()) return 0;
        for (const auto handle : points.entries) {
            const world::Entity *point = w.registry.get(handle);
            if (point == nullptr) return 0;
            const int team = point->team < 128 ? point->team : int(point->team) - 256;
            if (team != A(0)) return 0;
        }
        return 1;
    }
    if (ieq(n, "event"))
        return w.script.bms_events != nullptr && w.script.bms_events->is_active(A(0)) ? 1 : 0;
    if (ieq(n, "dooropen")) return w.doors.group_open(w, A(0)) ? 1 : 0;
    if (ieq(n, "opendoors") || ieq(n, "closedoors")) {
        w.doors.command_group(w, A(0), ieq(n, "opendoors"), false);
        return 1;
    }
    if (ieq(n, "setaccuracy"))
        return cmds.set_ssn_accuracy(H(0), A(1), A(2)) ? 1 : 0;
    if (ieq(n, "ssnguard"))
        return cmds.set_ssn_guard(H(0), A(1) != 0) ? 1 : 0;
    if (ieq(n, "ssncspd") || ieq(n, "ssnpspd")) {
        const world::EntityHandle ssn = H(0);
        if (!cmds.ssn_exists(ssn)) return 0;
        cmds.apply_ai_command(ssn, ieq(n, "ssncspd") ? 29 : 30,
                              A(1), 0, 0);
        // Retail reports success for any resolved row with an ItemTypeIndex
        // (ssn_exists); only the AI-event queue is gated on the brain
        // [orig: WacScript_SendAIEvent10ToEntity @0x4F74B0 — gate @0x4F74FD,
        //  queue gate @0x4F7508, return 1 @0x4F755A; event-11 twin @0x4F7570].
        return 1;
    }

    // ---- group actions ----
    // Returns 0 [orig: WacCmd_Kill @0x4EDC90, `xor eax,eax` @0x4EDC9D] after the
    // group walk [orig: Entity_KillAllByNetId @0x43C8E0].
    if (ieq(n, "kill")) { cmds.kill_group(A(0)); return 0; }
    if (ieq(n, "Gkill") || ieq(n, "Gremove")) {
        const int32_t group = A(0);
        if (group >= 0 && size_t(group) < groups_.size()) {
            for (world::EntityHandle h : groups_[group]) {
                if (const world::Entity *entity = w.registry.get(h)) {
                    // Gkill runs the killSSN body on every handle (its own
                    // ItemTypeIndex gate) [orig: WacCmd_GroupKill @0x4F1F40
                    // (the killSSN body call @0x4F1F5E)]; Gremove removes without
                    // a gate [orig: WacCmd_GroupRemove @0x4F1F80 (the
                    // Server_RemoveEntityAndNotify call @0x4F1FF2)]. Both return
                    // 0 [orig: WacCmd_GroupKill @0x4F1F72; WacCmd_GroupRemove
                    // @0x4F2006].
                    if (ieq(n, "Gkill")) cmds.wac_kill_ssn(h);
                    else cmds.remove_ssn(h);
                }
            }
        }
        return 0;
    }
    // Both return 1 whatever they visited [orig: TextResource_GetMissionString
    // (the GtoWP handler, IDB misnomer) @0x4ED3E4; WacScript_SetEntityTeamSlot
    // (the GroupHP handler) @0x4F7BD0].
    if (ieq(n, "GtoWP")) { cmds.group_to_waypoint(A(0), A(1)); return 1; }
    if (ieq(n, "GroupHP")) { cmds.set_group_hp(A(0), A(1)); return 1; }
    if (ieq(n, "GroupMin")) return cmds.set_group_engage_min(A(0), A(1));
    if (ieq(n, "GroupMax")) return cmds.set_group_engage_max(A(0), A(1));
    if (ieq(n, "GroupAtt")) return cmds.set_group_attack_max(A(0), A(1));
    if (ieq(n, "pisteam")) {
        const world::Entity *entity = w.registry.get(world::EntityHandle{static_cast<uint16_t>(auto_item_)});
        return entity != nullptr && static_cast<int8_t>(entity->team) == A(0);
    }
    // The selected entity must own an active player slot. A Player flag,
    // ItemDef or positive health alone does not establish that ownership.
    // [orig: WacCmd_PlayerIsKills @0x4F0B60; Entity_ValidatePtr @0x500910]
    if (ieq(n, "pisgold")) {
        // The JO node's +744 flags start at zero. Its PlayEnter flag parser
        // consumes/discards every token and clears the entire dword; the
        // recovered executable has no producer that sets the Gold bit.
        // [orig: WacCmd_PlayerIsGold @0x4F0AF0;
        // NapiNPPlayer_Create @0x4C7850;
        // CNapiNetwork_ParseServerVarList @0x4C4310, clear @0x4C4387]
        return 0;
    }
    if (ieq(n, "piskills")) {
        const world::EntityHandle handle{static_cast<uint16_t>(auto_item_)};
        const world::MatchPlayer *player = w.match.player(handle);
        return w.registry.get(handle) != nullptr && player != nullptr &&
                player->stats[world::MatchStats::kEnemyKills] >= A(0);
    }
    if (ieq(n, "pisvar") || ieq(n, "psetvar")) {
        // Retail checks only the upper bound. Negative indices address other
        // player-slot fields; the portable model rejects them (D-WAC-2).
        // [orig: WacCmd_PlayerIsVar @0x4F0BD0; WacCmd_PlayerSetVar @0x4F0CB0]
        const int32_t index = A(0);
        const world::EntityHandle handle{static_cast<uint16_t>(auto_item_)};
        world::MatchPlayer *player = w.match.player(handle);
        if (index < 0 || index > 16 || w.registry.get(handle) == nullptr || player == nullptr)
            return 0;
        if (ieq(n, "psetvar")) player->script_vars[size_t(index)] = 1;
        return player->script_vars[size_t(index)] != 0;
    }
    if (ieq(n, "AddExp")) return w.match.add_experience(w, H(0), A(1));
    if (ieq(n, "ppunt") || ieq(n, "pkillpunt"))
        return w.match.request_player_punt(w, world::EntityHandle{static_cast<uint16_t>(auto_item_)},
                                           ieq(n, "pkillpunt"));
    if (ieq(n, "Gsetaccuracy")) {
        // [orig: WacCmd_GroupSetAccuracy @0x4F7BE0 — return 1 @0x4F7C43]
        cmds.set_group_accuracy(A(0), A(1), A(2));
        return 1;
    }

    // ---- environment ---- (world::WeatherState carries the handler cites)
    if (ieq(n, "fogtype")) { cmds.set_fog_type(A(0)); return 0; }
    if (ieq(n, "fogdist")) { cmds.set_fog_distance_q16(A(0)); return 0; }
    if (ieq(n, "movefog")) { cmds.move_fog_q16(A(0), A(1)); return 0; }
    if (ieq(n, "rain")) { cmds.set_rain(A(0), A(1)); return 0; }
    if (ieq(n, "snow")) { cmds.set_snow(A(0), A(1)); return 0; }
    if (ieq(n, "overcast")) { cmds.set_overcast(A(0), A(1)); return 0; }
    if (ieq(n, "skyspeed")) { cmds.set_sky_speed(A(0)); return 0; }
    if (ieq(n, "fov")) { cmds.set_fov(A(0)); return 0; }
    if (ieq(n, "skyheight")) { cmds.set_sky_height(A(0)); return 0; }
    // [orig: WacCmd_Tod @0x4EDC70 — return 1 @0x4EDC7F]
    if (ieq(n, "TOD")) { cmds.set_time_of_day_minutes(A(0)); return 1; }
    if (ieq(n, "sunfade")) { cmds.sun_fade(A(0), A(1)); return 0; }
    if (ieq(n, "colorfade")) { cmds.set_color_fade(A(0)); return 0; }
    // The handlers pack three independent operands, retaining carries between
    // components and dword wrap. Variables are evaluated at each execution.
    // [orig: WacCmd_Sun..SkyFogColor @0x4EDCD0..0x4EDE70]
    const auto rgb = [&]() -> uint32_t {
        return uint32_t(A(2)) + ((uint32_t(A(1)) + (uint32_t(A(0)) << 8)) << 8);
    };
    if (ieq(n, "lightning")) { cmds.set_lightning_color(rgb()); return 0; }
    if (ieq(n, "sun")) { cmds.set_weather_color(world::WeatherColorTarget::Sun, rgb()); return 0; }
    if (ieq(n, "sky")) { cmds.set_weather_color(world::WeatherColorTarget::Sky, rgb()); return 0; }
    if (ieq(n, "ground")) { cmds.set_weather_color(world::WeatherColorTarget::Ground, rgb()); return 0; }
    if (ieq(n, "floor")) { cmds.set_weather_color(world::WeatherColorTarget::Floor, rgb()); return 0; }
    if (ieq(n, "ceiling")) { cmds.set_weather_color(world::WeatherColorTarget::Ceiling, rgb()); return 0; }
    if (ieq(n, "cloud")) { cmds.set_weather_color(world::WeatherColorTarget::Cloud, rgb()); return 0; }
    if (ieq(n, "fog") || ieq(n, "fogcolor")) { cmds.set_weather_color(world::WeatherColorTarget::Fog, rgb()); return 0; }
    if (ieq(n, "skyfog") || ieq(n, "skyfogcolor") || ieq(n, "crash")) { cmds.set_weather_color(world::WeatherColorTarget::SkyFog, rgb()); return 0; }
    if (ieq(n, "gain")) { cmds.set_weather_color(world::WeatherColorTarget::Gain, rgb()); return 0; }

    // ---- objective ----
    // [orig: WacAction_Win @0x4ed4a0 — Server_ProcessRoundEnd(team) straight through.]
    if (ieq(n, "win")) {
        w.out.effects.push({"win", A(0), 0, 0, 0, std::string()});
        w.process_round_end(A(0));
        return 1; // [orig: WacAction_Win @0x4ED4AD]
    }
    // [orig: WacAction_Lose @0x4ed3f0 — team 0 resolves Misc/STRMISC_KILLEDGREEN,
    // team 1 Misc/STRMISC_KILLEDBLUE, each through the banner trio
    // (GameMsg_AddChatLineAndRelay @0x5ba170 — the KEY rides the wire, clients
    // re-resolve locally / GameMsg_SetBannerText @0x5ba200 / GameMsg_SetTeamBannerText
    // @0x5ba1d0), then Server_ProcessRoundEnd(2): red wins, the player side loses.
    // Any other team id is a NO-OP returning 0. The banner trio is embedder
    // presentation — the effect carries the gametext key, the embedder resolves it
    // against the 'Misc' section; the banners persist until the next round start
    // (cleared by the round-start HUD reset @0x5b71b0).]
    if (ieq(n, "lose")) {
        const int32_t team = A(0);
        if (team != 0 && team != 1) return 0;
        w.out.effects.push({"lose", team, 0, 0, 0,
                        std::string(team == 1 ? "STRMISC_KILLEDBLUE" : "STRMISC_KILLEDGREEN")});
        w.process_round_end(2);
        return 1;
    }

    // ---- debug console ----
    if (ieq(n, "Help")) {
        // [orig: WacCmd_Help @0x4F6DE0]
        const HelpExportResult result = export_help();
        if (result.help_written)
            w.out.effects.push({"debug_text", 0, 0, 0, 0, "Current Help.wac file saved"});
        if (result.events_written)
            w.out.effects.push({"debug_text", 0, 0, 0, 0, "Current events.xml file saved"});
        return 0;
    }

    // The mission-owned voice channel's readiness query; the wave commands
    // themselves are replicated rows (remote_command.cpp). [orig: @0x4ED380]
    if (ieq(n, "waveready")) return w.script.voice.ready();

    // ssnrelease(ssn) -- detach a transported AI and CLEAR its boarding order.
    // [orig: WacCmd_SsnRelease (ex sub_4F7420) @0x4f7420, the WAC command table's 0x4f7420 row]
    // Previously fell through to the default effect push, i.e. the mission script
    // said "everybody out" and nothing happened: the occupant stayed mounted at
    // command 125 for the rest of the mission.
    if (ieq(n, "ssnrelease")) {
        // Returns 1 once the handle resolves to a live item WITH a parent (a
        // null AI slot still returns 1); every earlier gate returns 0
        // [orig: WacCmd_SsnRelease @0x4F7420 — the +0x1C item test, the
        //  +0x16C parent test @0x4f7463, the canonical 1 past the detach].
        return w.commands.release_boarding_command(H(0)) ? 1 : 0;
    }

    const int32_t values[4] = {A(0), A(1), A(2), A(3)};
    record_gap(w, cmd, instruction, values);
    // ---- default: record the command as an observable effect ----
    w.out.effects.push({def.name, A(0), A(1), A(2), A(3), S(0)});
    return 0;
}

// The operands as the S2C 0x23 body carries them: Text/Filename as the
// string, Ssn as the packed handle word, everything else as the resolved dword.
// [orig: WacScript_ExecuteBytecode @0x4F58B0 — payload loop @0x4f5d26..0x4f5dc2:
//  types 17/18 @0x4f5d71..0x4f5dab, type 11 @0x4f5d3d, else @0x4f5d59]
std::vector<world::ScriptRemoteArg> WacVm::resolve_remote_args(opennova::world::World &w,
                                                               const CommandDef &def,
                                                               const uint32_t *args, int argc) const {
    std::vector<world::ScriptRemoteArg> out(static_cast<size_t>(def.argc));
    for (int i = 0; i < def.argc && i < argc && args != nullptr; ++i) {
        world::ScriptRemoteArg &arg = out[static_cast<size_t>(i)];
        const ParamType type = def.params[i];
        if (type == ParamType::Text || type == ParamType::Filename) {
            arg.text = operand_string(w, args[i], type);
        } else {
            arg.value = read(w, args[i]);
        }
    }
    return out;
}

// [orig: WacScript_ExecuteBytecode @0x4F58B0 — the registry flags-0x18 arm
//  @0x4f5ca5..0x4f5ee9]
int32_t WacVm::replicate(opennova::world::World &w, int cmd, const CommandDef &def,
                         const uint32_t *args, int argc, uint32_t instruction) {
    // A replicated row without a handler body is the same dispatch gap the
    // local arm records, at the same instruction site.
    auto finish = [&](const RemoteCommandResult &result) -> int32_t {
        if (!result.handled) {
            int32_t values[4] = {};
            for (int i = 0; i < 4 && i < argc && args != nullptr; ++i) values[i] = read(w, args[i]);
            record_gap(w, cmd, instruction, values);
        }
        return result.value;
    };
    world::ScriptRemoteCommand record;
    record.command_index = static_cast<uint16_t>(remote_command_wire_index(cmd)); // @0x4f5cb5..0x4f5cce
    record.args = resolve_remote_args(w, def, args, argc);                        // @0x4f5cf9..0x4f5dc2
    const RemoteCommandNames names{&prog_->effect_names, &prog_->sound_names};
    if ((def.flags & 0x10) != 0) {
        // The targeted class reaches the selected player's connection alone
        // and skips the local handler, reporting 1. An unselected, local or
        // unregistered selection runs the handler here instead: the packed
        // handle must name a pool below 5 and a slot inside it, not be the
        // local player, and own an active player slot (Entity_ValidatePtr).
        // [@0x4f5df9..0x4f5e8b; Entity_ValidatePtr @0x500910]
        const world::EntityHandle target{static_cast<uint16_t>(auto_item_)};
        if (target.valid() && target.pool() < world::EntityRegistry::kPoolCount &&
                w.registry.get(target) != nullptr && target != w.cached.local_player &&
                w.match.player(target) != nullptr) {
            record.targeted = true;
            record.target = target;
            w.out.script_remote_commands.push_back(std::move(record));
            return 1;
        }
        return finish(run_remote_command(w, cmd, record.args, names));
    }
    // The broadcast class reaches every in-match remote AND runs here.
    // [@0x4f5ec7..0x4f5ed1, then the call-convention switch @0x4f5ef6]
    const std::vector<world::ScriptRemoteArg> resolved = record.args;
    w.out.script_remote_commands.push_back(std::move(record));
    return finish(run_remote_command(w, cmd, resolved, names));
}

void WacVm::record_gap(opennova::world::World &w, int cmd, uint32_t instruction, const int32_t *arguments) {
    world::RuntimeGapSite gap{world::RuntimeGapKind::WacCommand, cmd, 0, cur_event_, int32_t(instruction)};
    const auto source = std::lower_bound(prog_->instruction_sources.begin(), prog_->instruction_sources.end(),
            instruction, [](const InstructionSource &entry, uint32_t word) { return entry.word < word; });
    if (source != prog_->instruction_sources.end() && source->word == instruction) {
        gap.line = source->line;
        if (source->source_index < prog_->source_names.size()) gap.source = prog_->source_names[source->source_index];
    }
    std::array<int32_t, 4> values{};
    if (arguments != nullptr) std::copy_n(arguments, 4, values.begin());
    w.diagnostics.record(gap, w.logic_tick, values);
}

// [orig: WacScript_CacheLocalPlayerState @0x4F5780, called only at
// WacScript_ExecuteBytecode entry @0x4F58F4, including the initial execution]
void WacVm::cache_player_state(opennova::world::World &w) {
    cached_tod_ = static_cast<int32_t>(w.weather.tod_fixed24) / 279620;
    scratch_ = 0; // the unresolved-parameter sink [orig: @0x4f57b5]
    const int winner = w.match.outcome().winner_team;
    cached_game_over_ = winner != 0 ? 1 : 0;
    cached_win_ = winner == 1 ? 1 : 0;
    cached_lose_ = winner == 2 ? 1 : 0;
    const world::Entity *player = w.registry.get(w.cached.local_player);
    w.cached.local_health = player != nullptr ? world::retail_signed_i16(player->health) : 0;
    cached_mana_ = player != nullptr ? player->mana : 0;
    bind_auto_handle(player != nullptr ? player->handle.packed : uint16_t(0xFFFF));
}

void WacVm::execute(opennova::world::World &w) {
    if (!prog_) return;
    cache_player_state(w);
    acc_ = 0;
    cur_event_ = 0;
    // Retail resolves authored SSNs once during compilation. Bind once when
    // the portable VM first receives its world; aliases preserve packed handles.
    if (entity_bindings_.size() != prog_->operands.size()) {
        entity_bindings_.resize(prog_->operands.size(), 0xFFFF);
        for (size_t i = 0; i < prog_->operands.size(); ++i)
            entity_bindings_[i] = w.commands.resolve_ssn(uint16_t(prog_->operands[i])).packed;
    }
    w.registry.script_groups(groups_);
    const std::vector<uint32_t> &code = prog_->code;
    size_t ip = 0;
    const size_t n = code.size();
    std::array<uint8_t, 16> expr_stack{};
    size_t expr_depth = 0;
    size_t group_index = 0;
    int32_t group_remaining = 0;

    auto ev = [&](int i) -> EventState & {
        if (i < 0 || i >= static_cast<int>(events_.size())) i = 0;
        return events_[i];
    };
    bool arithmetic_fault = false;
    auto apply_fold = [&](Op op, int32_t result) {
        switch (op) {
            case Op::FoldAnd: acc_ = (acc_ && result) ? 1 : 0; break;
            case Op::FoldOr: acc_ = (acc_ || result) ? 1 : 0; break;
            case Op::FoldAdd: acc_ = int32_t(uint32_t(acc_) + uint32_t(result)); break;
            case Op::FoldSub: acc_ = int32_t(uint32_t(acc_) - uint32_t(result)); break;
            case Op::FoldMul: acc_ = int32_t(uint32_t(acc_) * uint32_t(result)); break;
            case Op::FoldDiv:
            case Op::FoldMod:
                if (result == 0 || (acc_ == INT32_MIN && result == -1)) {
                    // Retail faults on invalid IDIV. Keep malformed scripts
                    // observable without taking down the portable host.
                    w.diagnostics.record({world::RuntimeGapKind::WacOpcode, int32_t(op), 2,
                            cur_event_, int32_t(ip)}, w.logic_tick, {acc_, result, 0, 0});
                    arithmetic_fault = true;
                } else acc_ = op == Op::FoldDiv ? acc_ / result : acc_ % result;
                break;
            case Op::FoldPow: acc_ = power_fold(acc_, result); break;
            case Op::FoldEq: acc_ = (acc_ == result) ? 1 : 0; break;
            case Op::FoldNe: acc_ = (acc_ != result) ? 1 : 0; break;
            case Op::FoldLt: acc_ = (acc_ < result) ? 1 : 0; break;
            case Op::FoldGt: acc_ = (acc_ > result) ? 1 : 0; break;
            case Op::FoldLe: acc_ = (acc_ <= result) ? 1 : 0; break;
            case Op::FoldGe: acc_ = (acc_ >= result) ? 1 : 0; break;
            default: acc_ = result; break; // Op::Call / assign
        }
    };

    int guard = 0;
    const int kGuardLimit = 4'000'000; // backstop against malformed jump loops
    while (ip < n && !arithmetic_fault) {
        if (++guard > kGuardLimit) {
            w.diagnostics.record({world::RuntimeGapKind::WacInstructionLimit, 0, 0, cur_event_, int32_t(ip)}, w.logic_tick);
            break;
        }
        uint32_t word = code[ip];
        if (word == kProgramTerminator) break;
        Op op = instr_op(word);
        uint32_t operand = instr_operand24(word);
        bool negate = instr_negate(word);
        switch (op) {
            case Op::EnterEvent:
                cur_event_ = static_cast<int>(operand);
                expr_depth = 0; // the accumulator survives an event boundary
                ip += 1;
                break;
            case Op::Jump:
                ip = operand;
                break;
            case Op::MarkFired: // THEN [orig: @0x4F5984]
            case Op::AndChain:  // ENTER [orig: @0x4F59CF]
            case Op::OrChain: { // LEAVE [orig: @0x4F5A09]
                EventState &es = ev(cur_event_);
                const bool cond = acc_ != 0;
                const bool fire = op == Op::MarkFired ? cond
                        : op == Op::AndChain ? cond && !es.active : !cond && es.active;
                es.active = cond;
                if (fire) {
                    es.last_fired_tick = time_;
                    es.ever_fired = true;
                    ++es.fired_count;
                    ++ip;
                } else {
                    ip = operand;
                }
                break;
            }
            case Op::StoreVar:
                if (ip + 1 < n) { write(w, code[ip + 1], acc_); ip += 2; } else ip += 1;
                break;
            case Op::DoSeq:
            case Op::DoRnd: {
                // [orig: WacScript_ExecuteBytecode @0x4F5A1C..0x4F5AE5]
                if (ip + 1 >= n) { ip = n; break; }
                const uint32_t params = code[ip + 1];
                const uint32_t loop = params >> 16, count = params & 0xFFFF;
                if (loop >= loop_counters_.size()) {
                    w.diagnostics.record({world::RuntimeGapKind::WacOpcode, int32_t(op), 0, cur_event_, int32_t(ip)}, w.logic_tick);
                    ip = operand;
                    break;
                }
                if (op == Op::DoRnd) {
                    // Every opcode 5 steps the generator, a zero count too;
                    // the rounded 16-bit scale keeps only its low byte.
                    // [orig: WacScript_ExecuteBytecode @0x4F5A7E (the step),
                    //  @0x4F5AB8..0x4F5AC4 (imul, round, shrd), @0x4F5AD0 (the
                    //  choice byte)]
                    const uint32_t v = next_rand();
                    loop_choices_[loop] = uint8_t((count * (v & 0xFFFFu) + 0x8000u) >> 16);
                } else {
                    if (loop_counters_[loop] >= count) loop_counters_[loop] = 0;
                    loop_choices_[loop] = loop_counters_[loop]++;
                }
                ip = loop_choices_[loop] == 0 ? ip + 2 : operand;
                break;
            }
            case Op::NextDo: {
                // [orig: WacScript_ExecuteBytecode @0x4F5AEA..0x4F5AFC]
                if (ip + 1 >= n) { ip = n; break; }
                const uint32_t loop = code[ip + 1];
                if (loop >= loop_choices_.size()) {
                    w.diagnostics.record({world::RuntimeGapKind::WacOpcode, int32_t(op), 0, cur_event_, int32_t(ip)}, w.logic_tick);
                    ip = operand;
                    break;
                }
                ip = loop_choices_[loop]-- == 1 ? ip + 2 : operand;
                break;
            }
            case Op::GroupIter:
                group_index = operand;
                group_remaining = group_index < groups_.size()
                        ? static_cast<int32_t>(groups_[group_index].size()) : 0;
                ++ip;
                break;
            case Op::LocalPlayer:
                // [orig: WacScript_ExecuteBytecode @0x4F58B0, opcodes 9/10]
                if (--group_remaining >= 0) {
                    bind_auto_handle(groups_[group_index][group_remaining].packed);
                    ++ip;
                } else {
                    group_remaining = 0;
                    bind_auto_handle(w.cached.local_player.packed);
                    ip = operand;
                }
                break;
            case Op::PopExpr: {
                // The retail expression stack stores bytes, even though the
                // accumulator and call results are dwords. [orig: @0x4F5BDF]
                if (expr_depth != 0) --expr_depth;
                const Op fold = static_cast<Op>(operand & kOpMask);
                int32_t result = fold == Op::Call ? acc_ : expr_stack[expr_depth];
                if ((operand & 0x80u) != 0) result = result == 0;
                apply_fold(fold, result);
                ++ip;
                break;
            }
            default: {
                if ((word & kPushBit) != 0) {
                    if (expr_depth < expr_stack.size()) expr_stack[expr_depth++] = uint8_t(acc_);
                    else w.diagnostics.record({world::RuntimeGapKind::WacOpcode, int32_t(op), 1, cur_event_, int32_t(ip)}, w.logic_tick);
                }
                int cmd = instr_command_index(word);
                int argc = 0;
                if (cmd >= 0 && cmd < wac_command_count()) argc = wac_commands()[cmd].argc;
                if (ip + 1 + static_cast<size_t>(argc) > n) {
                    argc = static_cast<int>(n - ip - 1);
                }
                const uint32_t *args = (argc > 0) ? &code[ip + 1] : nullptr;
                int32_t result = dispatch(w, cmd, args, argc, static_cast<uint32_t>(ip));
                if (negate) result = (result == 0) ? 1 : 0;
                apply_fold(op, result);
                ip += 1 + static_cast<size_t>(argc);
                break;
            }
        }
    }
    // [orig: WacScript_ExecuteBytecode @0x4F61F2 -> counters reset @0x4EE6D0]
    w.script.weapon_input.clear_fire_requests();
    w.script.squad_events.advance_execution();
}

} // namespace opennova::wac
