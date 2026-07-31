#include "wac/vm.h"

#include <cctype>
#include <cstring>

#include "wac/bytecode.h"
#include "wac/command.h"
#include "world/world.h"

#include <io/strutil.h>

namespace opennova::wac {
namespace {

bool ieq(const char *a, const char *b) { return opennova::strutil::iequals(a, b); }

uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

} // namespace

void WacVm::load(const Program &program) {
    prog_ = &program;
    events_.assign(static_cast<size_t>(program.event_count > 0 ? program.event_count : 1),
                   EventState{});
    rng_seed_ = 0x12333333u; // [orig: WacScript_InitAndLoad @ 0x4f966b]
    acc_ = 0;
    cur_event_ = 0;
    time_ = 0;
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

int32_t WacVm::rand_range(int n) {
    if (n <= 0) return 0;
    uint32_t v = next_rand();
    return static_cast<int32_t>((static_cast<uint32_t>(n) * (v & 0xFFFFu) + 0x8000u) >> 16);
}

int32_t WacVm::read(opennova::world::World &w, uint32_t ref) const {
    switch (operand_kind(ref)) {
        case OperandKind::Pool: {
            uint32_t i = operand_index(ref);
            return (i < prog_->operands.size()) ? prog_->operands[i] : 0;
        }
        case OperandKind::MissionVar:
            return w.vars.get_mission(static_cast<int>(operand_index(ref)));
        case OperandKind::GlobalVar:
            return w.vars.get_global(static_cast<int>(operand_index(ref)));
        case OperandKind::MusicVar:
            return w.vars.get_music(static_cast<int>(operand_index(ref)));
        case OperandKind::Builtin: {
            switch (static_cast<Builtin>(operand_index(ref))) {
                case Builtin::Ticks: return static_cast<int32_t>(time_); // VM executions [orig: dword_C6EAD8]
                case Builtin::Result: return acc_;
                case Builtin::Health: return w.cached.local_health;
                case Builtin::NearType: return w.cached.near_type;
                case Builtin::NearDist: return w.cached.near_dist;
                case Builtin::NearId: return w.cached.near_id;
                case Builtin::Wind: return 0;
                case Builtin::Mana: return 0;
                case Builtin::Bluekills: return w.kill_stats.bluekills_by_player;   // [orig: 0xC846F0]
                case Builtin::Greenkills: return w.kill_stats.greenkills_by_player; // [orig: 0xC846F8]
                case Builtin::Humans: return w.cached.humans;                       // [orig: 0xC6EB14]
                // GameOver/WinVar/LoseVar derive from the round winner (0 until the
                // round ends, like the scoreboard winner dword the original derives
                // them from each pre-tick cache pass — a green/0 outcome never raises
                // GameOver). [orig: WacScript_CacheLocalPlayerState @0x4f57bb/c9/cf]
                case Builtin::GameOver: return w.round_end.winner_team != 0 ? 1 : 0;
                case Builtin::WinVar: return w.round_end.winner_team == 1 ? 1 : 0;
                case Builtin::LoseVar: return w.round_end.winner_team == 2 ? 1 : 0;
                case Builtin::AccuracySpread: return w.wac_values.accuracy_spread;
            }
            return 0;
        }
    }
    return 0;
}

void WacVm::write(opennova::world::World &w, uint32_t ref, int32_t v) const {
    switch (operand_kind(ref)) {
        case OperandKind::MissionVar: w.vars.set_mission(static_cast<int>(operand_index(ref)), v); break;
        case OperandKind::GlobalVar: w.vars.set_global(static_cast<int>(operand_index(ref)), v); break;
        case OperandKind::MusicVar: w.vars.set_music(static_cast<int>(operand_index(ref)), v); break;
        case OperandKind::Builtin:
            // The retail named-value resolver returns the address of this mutable
            // engine dword, so ordinary set/add/sub/inc/dec/store write through.
            // [orig: WacScript_ResolveParameter @0x4f2940 ->
            //  wac_var_accuracyspread @0xC6EAE8]
            if (static_cast<Builtin>(operand_index(ref)) == Builtin::AccuracySpread)
                w.wac_values.accuracy_spread = v;
            break;
        default: break; // pool values are not lvalues
    }
}

int32_t WacVm::arg_as_string_index(uint32_t ref) const {
    if (operand_kind(ref) == OperandKind::Pool) {
        uint32_t i = operand_index(ref);
        if (i < prog_->operands.size()) return prog_->operands[i];
    }
    return -1;
}

int32_t WacVm::dispatch(opennova::world::World &w, int cmd, const uint32_t *args, int argc) {
    if (cmd < 0 || cmd >= wac_command_count()) return 0;
    const CommandDef &def = wac_commands()[cmd];
    const char *n = def.name;
    auto A = [&](int i) -> int32_t { return (i < argc && args) ? read(w, args[i]) : 0; };
    auto S = [&](int i) -> std::string {
        if (i >= argc || !args) return std::string();
        int32_t si = arg_as_string_index(args[i]);
        if (si >= 0 && si < static_cast<int32_t>(prog_->strings.size())) return prog_->strings[si];
        return std::string();
    };
    EventState &es = events_[(cur_event_ >= 0 && cur_event_ < static_cast<int>(events_.size())) ? cur_event_ : 0];
    auto &cmds = w.commands;

    // ---- temporal / event conditions ----
    if (ieq(n, "never")) return es.ever_fired ? 0 : 1;
    if (ieq(n, "previous")) return es.fired_count > 0 ? 1 : 0;
    if (ieq(n, "past")) return static_cast<int32_t>(time_) >= A(0) ? 1 : 0;
    if (ieq(n, "ontick") || ieq(n, "onptick")) return static_cast<int32_t>(time_) == A(0) ? 1 : 0;
    if (ieq(n, "elapse") || ieq(n, "chain") || ieq(n, "before")) {
        return (static_cast<int32_t>(time_) - static_cast<int32_t>(es.last_fired_tick)) >= A(0) ? 1 : 0;
    }

    // ---- group / entity state conditions ----
    if (ieq(n, "groupdead")) return cmds.group_dead(A(0)) ? 1 : 0;
    if (ieq(n, "groupalive")) return cmds.group_alive(A(0)) ? 1 : 0;
    if (ieq(n, "SSNdead")) return cmds.ssn_dead(static_cast<uint16_t>(A(0))) ? 1 : 0;
    if (ieq(n, "SSNalive")) return cmds.ssn_alive(static_cast<uint16_t>(A(0))) ? 1 : 0;
    if (ieq(n, "SSNexists")) return cmds.ssn_exists(static_cast<uint16_t>(A(0))) ? 1 : 0;
    if (ieq(n, "SSNcritical")) return cmds.ssn_alive(static_cast<uint16_t>(A(0))) ? 0 : 1;
    if (ieq(n, "SSNarea") || ieq(n, "SSNarea3D") || ieq(n, "SSNloc")) {
        return cmds.ssn_in_area(static_cast<uint16_t>(A(0)), A(1)) ? 1 : 0;
    }

    // ---- comparison / value functions ----
    if (ieq(n, "eq")) return A(0) == A(1) ? 1 : 0;
    if (ieq(n, "ne")) return A(0) != A(1) ? 1 : 0;
    if (ieq(n, "lt")) return A(0) < A(1) ? 1 : 0;
    if (ieq(n, "gt")) return A(0) > A(1) ? 1 : 0;
    if (ieq(n, "le")) return A(0) <= A(1) ? 1 : 0;
    if (ieq(n, "ge")) return A(0) >= A(1) ? 1 : 0;
    if (ieq(n, "true")) return A(0) != 0 ? 1 : 0;
    if (ieq(n, "false")) return A(0) == 0 ? 1 : 0;
    if (ieq(n, "random")) return rand_range(A(0)) == 0 ? 1 : 0;

    // ---- variable mutation ----
    if (ieq(n, "set")) { if (argc >= 2) write(w, args[0], A(1)); return A(1); }
    if (ieq(n, "add")) { if (argc >= 2) { int32_t v = read(w, args[0]) + A(1); write(w, args[0], v); return v; } return 0; }
    if (ieq(n, "sub")) { if (argc >= 2) { int32_t v = read(w, args[0]) - A(1); if (v < 0) v = 0; write(w, args[0], v); return v; } return 0; }
    if (ieq(n, "inc")) { if (argc >= 1) { int32_t v = read(w, args[0]) + 1; write(w, args[0], v); return v; } return 0; }
    if (ieq(n, "dec")) { if (argc >= 1) { int32_t v = read(w, args[0]) - 1; if (v < 0) v = 0; write(w, args[0], v); return v; } return 0; }
    if (ieq(n, "store")) { if (argc >= 1) write(w, args[0], acc_); return acc_; }
    if (ieq(n, "load")) { return A(0); }

    // ---- entity actions ----
    if (ieq(n, "killSSN")) return cmds.kill_ssn(static_cast<uint16_t>(A(0))) ? 1 : 0;
    if (ieq(n, "removeSSN")) return cmds.remove_ssn(static_cast<uint16_t>(A(0))) ? 1 : 0;
    if (ieq(n, "SSNHP")) return cmds.set_ssn_hp(static_cast<uint16_t>(A(0)), A(1)) ? 1 : 0;
    if (ieq(n, "SSNADDHP")) return cmds.add_ssn_hp(static_cast<uint16_t>(A(0)), A(1)) ? 1 : 0;
    if (ieq(n, "SSNtoWP")) return cmds.set_ssn_waypoint(static_cast<uint16_t>(A(0)), A(1)) ? 1 : 0;
    if (ieq(n, "SSNMin")) return cmds.set_ssn_engage_min(static_cast<uint16_t>(A(0)), A(1)) ? 1 : 0;
    if (ieq(n, "SSNMax")) return cmds.set_ssn_engage_max(static_cast<uint16_t>(A(0)), A(1)) ? 1 : 0;
    if (ieq(n, "SSNAtt")) return cmds.set_ssn_attack_max(static_cast<uint16_t>(A(0)), A(1)) ? 1 : 0;
    if (ieq(n, "SSNanim")) return cmds.set_ssn_anim(static_cast<uint16_t>(A(0)), A(1)) ? 1 : 0;
    if (ieq(n, "hideSSN")) return cmds.set_ssn_hidden(static_cast<uint16_t>(A(0)), true) ? 1 : 0;
    if (ieq(n, "unhideSSN")) return cmds.set_ssn_hidden(static_cast<uint16_t>(A(0)), false) ? 1 : 0;
    if (ieq(n, "holdSSN")) return cmds.set_ssn_held(static_cast<uint16_t>(A(0)), true) ? 1 : 0;
    if (ieq(n, "unholdSSN")) return cmds.set_ssn_held(static_cast<uint16_t>(A(0)), false) ? 1 : 0;
    if (ieq(n, "disableSSN")) return cmds.set_ssn_disabled(static_cast<uint16_t>(A(0)), true) ? 1 : 0;
    if (ieq(n, "enableSSN")) return cmds.set_ssn_disabled(static_cast<uint16_t>(A(0)), false) ? 1 : 0;

    // ---- group actions ----
    if (ieq(n, "kill") || ieq(n, "Gkill")) return cmds.kill_group(A(0));
    if (ieq(n, "GtoWP")) return cmds.group_to_waypoint(A(0), A(1));
    if (ieq(n, "GroupHP")) return cmds.set_group_hp(A(0), A(1));
    if (ieq(n, "GroupMin")) return cmds.set_group_engage_min(A(0), A(1));
    if (ieq(n, "GroupMax")) return cmds.set_group_engage_max(A(0), A(1));
    if (ieq(n, "GroupAtt")) return cmds.set_group_attack_max(A(0), A(1));

    // ---- environment ----
    if (ieq(n, "fogtype")) { w.env.fog_type = A(0); ++w.env.generation; return 0; }
    if (ieq(n, "fogdist")) { w.env.fog_dist = A(0); ++w.env.generation; return 0; }
    if (ieq(n, "rain")) { w.env.rain = A(0); ++w.env.generation; return 0; }
    if (ieq(n, "snow")) { w.env.snow = A(0); ++w.env.generation; return 0; }
    if (ieq(n, "overcast")) { w.env.overcast = A(0); ++w.env.generation; return 0; }
    if (ieq(n, "skyspeed")) { w.env.sky_speed = A(0); ++w.env.generation; return 0; }
    if (ieq(n, "TOD")) { w.env.time_of_day = A(0); ++w.env.generation; return 0; }
    if (ieq(n, "sun")) { w.env.sun_rgb = static_cast<uint32_t>(A(0)); ++w.env.generation; return 0; }
    if (ieq(n, "sky")) { w.env.sky_rgb = static_cast<uint32_t>(A(0)); ++w.env.generation; return 0; }
    if (ieq(n, "fog") || ieq(n, "fogcolor")) { w.env.fog_rgb = static_cast<uint32_t>(A(0)); ++w.env.generation; return 0; }

    // ---- objective ----
    // [orig: WacAction_Win @0x4ed4a0 — Server_ProcessRoundEnd(team) straight through.]
    if (ieq(n, "win")) {
        w.effects.push({"win", A(0), 0, 0, 0, std::string()});
        w.process_round_end(A(0));
        return 0;
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
        w.effects.push({"lose", team, 0, 0, 0,
                        std::string(team == 1 ? "STRMISC_KILLEDBLUE" : "STRMISC_KILLEDGREEN")});
        w.process_round_end(2);
        return 1;
    }

    // ---- player text / debug console ----
    // text/ptext feed the player message channel [orig: WAC text @ 0x4EDB50
    // -> Chat_AddMessageChannel1 @ 0x4985D0], while consol/pconsol feed the
    // distinct on-screen debug channel [orig: @ 0x4EDBE0 ->
    // Chat_AddDebugMessage]. Keep them separate so game hosts can present
    // mission text without leaking authored debug output into the HUD.
    if (ieq(n, "text") || ieq(n, "ptext")) {
        w.effects.push({"text", 0, 0, 0, 0, S(0)});
        return 0;
    }
    if (ieq(n, "consol") || ieq(n, "pconsol")) {
        w.effects.push({"debug_text", 0, 0, 0, 0, S(0)});
        return 0;
    }
    if (ieq(n, "text#")) {
        w.effects.push({"text", A(1), 0, 0, 0, S(0)});
        return 0;
    }
    if (ieq(n, "consol#")) {
        w.effects.push({"debug_text", A(1), 0, 0, 0, S(0)});
        return 0;
    }

    // ---- scripted voice (wave / pwave): play a .wav by FILENAME ----
    // [orig: wave/pwave @ 0x4ED610] loads the named wav from the archive
    // (Audio_LoadWavFileFromArchive @ 0x766480) and plays it on a single dedicated
    // voice channel (dword_C6EC30) that it RESETS first -- so a new wave interrupts
    // the previous one. This is a separate channel from the .DBF dialog queue
    // (PlayWavList), not serialized with it. The filename rides the effect string.
    // pwave is the network-broadcast twin (same handler); host-side identical.
    if (ieq(n, "wave") || ieq(n, "pwave")) {
        w.effects.push({"dialog_wav", 0, 0, 0, 0, S(0)});
        return 0;
    }

    // ---- default: record the command as an observable effect ----
    w.effects.push({def.name, A(0), A(1), A(2), A(3), S(0)});
    return 0;
}

void WacVm::execute(opennova::world::World &w) {
    if (!prog_) return;
    acc_ = 0;
    cur_event_ = 0;
    const std::vector<uint32_t> &code = prog_->code;
    size_t ip = 0;
    const size_t n = code.size();

    auto ev = [&](int i) -> EventState & {
        if (i < 0 || i >= static_cast<int>(events_.size())) i = 0;
        return events_[i];
    };
    auto apply_fold = [&](Op op, int32_t result) {
        switch (op) {
            case Op::FoldAnd: acc_ = (acc_ && result) ? 1 : 0; break;
            case Op::FoldOr: acc_ = (acc_ || result) ? 1 : 0; break;
            case Op::FoldAdd: acc_ += result; break;
            case Op::FoldSub: acc_ -= result; break;
            case Op::FoldMul: acc_ *= result; break;
            case Op::FoldDiv: if (result) acc_ /= result; break;
            case Op::FoldMod: if (result) acc_ %= result; break;
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
    while (ip < n) {
        if (++guard > kGuardLimit) break;
        uint32_t word = code[ip];
        if (word == kProgramTerminator) break;
        Op op = instr_op(word);
        uint32_t operand = instr_operand24(word);
        bool negate = instr_negate(word);
        switch (op) {
            case Op::EnterEvent:
                cur_event_ = static_cast<int>(operand);
                acc_ = 0;
                ip += 1;
                break;
            case Op::Jump:
                ip = operand;
                break;
            case Op::AndChain: {
                EventState &es = ev(cur_event_);
                bool cond = acc_ != 0;
                if (cond && !es.active) {
                    es.last_fired_tick = time_;
                    es.ever_fired = true;
                    es.active = true;
                    es.fired_count++;
                    ip += 1; // fall into body
                } else {
                    es.active = cond;
                    ip = operand; // skip body
                }
                break;
            }
            case Op::OrChain: {
                EventState &es = ev(cur_event_);
                bool cond = acc_ != 0;
                if (cond || !es.active) { es.active = cond; ip = operand; }
                else {
                    es.last_fired_tick = time_;
                    es.ever_fired = true;
                    es.active = cond;
                    ip += 1;
                }
                break;
            }
            case Op::MarkFired: {
                EventState &es = ev(cur_event_);
                es.last_fired_tick = time_;
                es.ever_fired = true;
                es.fired_count++;
                ip += 1;
                break;
            }
            case Op::StoreVar:
                if (ip + 1 < n) { write(w, code[ip + 1], acc_); ip += 2; } else ip += 1;
                break;
            case Op::PLoop:
            case Op::DoRnd:
            case Op::DoSeq:
            case Op::GroupIter:
            case Op::LocalPlayer:
            case Op::PopExpr:
                ip += 1; // not emitted by this compiler; skip safely
                break;
            default: {
                int cmd = instr_command_index(word);
                int argc = 0;
                if (cmd >= 0 && cmd < wac_command_count()) argc = wac_commands()[cmd].argc;
                if (ip + 1 + static_cast<size_t>(argc) > n) {
                    argc = static_cast<int>(n - ip - 1);
                }
                const uint32_t *args = (argc > 0) ? &code[ip + 1] : nullptr;
                int32_t result = dispatch(w, cmd, args, argc);
                if (negate) result = (result == 0) ? 1 : 0;
                apply_fold(op, result);
                ip += 1 + static_cast<size_t>(argc);
                break;
            }
        }
    }
    ++time_; // advance the WAC time base after the run [orig: dword_C6EAD8 @0x4f81d3]
}

} // namespace opennova::wac
