// WAC bytecode instruction encoding.
//
// Faithful to WacScript_ExecuteBytecode @0x4f58b0 (Jointops.exe). Each
// instruction is a 32-bit word:
//   bit31 (0x80000000) = negate result (NOT)
//   bit30 (0x40000000) = push accumulator onto the expression stack
//   bits24-29 (0x3F<<24) = opcode
//   bits0-23 (0x00FFFFFF) = operand24: jump/var/event index, OR for a CALL the
//                           low 16 bits hold the command-table index.
// For a CALL instruction the opcode field carries the ALU fold applied to the
// call's return value (Assign=0, or And/Or/.. 0x0F-0x1C); argc operand-ref words
// follow inline in the stream. Program terminator word = 0x7A7A7A7A ('zzzz').
//
// Tracked deviation: original operand words are absolute 32-bit pointers into the
// var/operand/string pools; we rebase them to tagged pool-relative references
// (OperandKind). The opcode/dispatch/accumulator behavior is preserved exactly.
#ifndef OPENNOVA_WAC_BYTECODE_H
#define OPENNOVA_WAC_BYTECODE_H

#include <cstdint>

namespace opennova::wac {

// Opcodes (instruction bits 24-29). [orig: HIBYTE(instruction) & 0x3F.]
enum class Op : uint8_t {
    Call = 0,         // function call; result -> accumulator (fold Assign)
    EnterEvent = 1,   // begin event/IF block (operand = event index)
    MarkFired = 2,    // record event fired
    Jump = 3,         // unconditional jump to operand index
    PLoop = 4,        // per-player countdown loop
    DoRnd = 5,        // weighted random branch
    DoSeq = 6,        // sequential countdown loop
    PopExpr = 7,      // expression-stack pop handling
    StoreVar = 8,     // store accumulator to a variable operand
    LocalPlayer = 9,  // resolve local player handle
    GroupIter = 0xA,  // begin group iteration over an event's entity list
    AndChain = 0xB,   // boolean AND chain (else-if)
    OrChain = 0xC,    // boolean OR chain / else / block-end
    // 0x0F-0x1C are ALU folds used as the opcode of a CALL/value instruction:
    FoldAnd = 0x0F,
    FoldOr = 0x10,
    FoldAdd = 0x11,
    FoldSub = 0x12,
    FoldMul = 0x13,
    FoldDiv = 0x14,
    FoldMod = 0x15,
    FoldPow = 0x16,
    FoldEq = 0x17,
    FoldNe = 0x18,
    FoldLt = 0x19,
    FoldGt = 0x1A,
    FoldLe = 0x1B,
    FoldGe = 0x1C,
};

constexpr uint32_t kProgramTerminator = 0x7A7A7A7Au; // 'zzzz'

constexpr uint32_t kNegateBit = 0x80000000u;
constexpr uint32_t kPushBit = 0x40000000u;
constexpr uint32_t kOpShift = 24;
constexpr uint32_t kOpMask = 0x3Fu;
constexpr uint32_t kOperand24Mask = 0x00FFFFFFu;
constexpr uint32_t kCommandIndexMask = 0x0000FFFFu;

inline constexpr uint32_t encode_instr(Op op, uint32_t operand24, bool push = false,
                                       bool negate = false) {
    uint32_t w = (static_cast<uint32_t>(op) & kOpMask) << kOpShift;
    w |= operand24 & kOperand24Mask;
    if (push) w |= kPushBit;
    if (negate) w |= kNegateBit;
    return w;
}

// A CALL instruction: opcode = fold op, low 16 = command index.
inline constexpr uint32_t encode_call(uint16_t command_index, Op fold = Op::Call,
                                      bool push = false, bool negate = false) {
    return encode_instr(fold, command_index, push, negate);
}

inline constexpr Op instr_op(uint32_t w) {
    return static_cast<Op>((w >> kOpShift) & kOpMask);
}
inline constexpr uint32_t instr_operand24(uint32_t w) { return w & kOperand24Mask; }
inline constexpr uint16_t instr_command_index(uint32_t w) {
    return static_cast<uint16_t>(w & kCommandIndexMask);
}
inline constexpr bool instr_push(uint32_t w) { return (w & kPushBit) != 0; }
inline constexpr bool instr_negate(uint32_t w) { return (w & kNegateBit) != 0; }

// ---- Operand references (rebased from the original's absolute pointers) ----
// Tagged 32-bit: high nibble = kind, low 28 bits = index. Pool refs index the
// program's resolved-value pool; var refs read/write the shared ScriptVarStore;
// builtin refs read once-per-tick cached state.
enum class OperandKind : uint8_t {
    Pool = 0,       // index into Program::operands (resolved literal/handle/id)
    MissionVar = 1, // V# -> ScriptVarStore.mission
    GlobalVar = 2,  // G# -> ScriptVarStore.global
    MusicVar = 3,   // M# -> ScriptVarStore.music
    Builtin = 4,    // read-only engine value (ticks/health/near*/...)
};

constexpr uint32_t kOperandKindShift = 28;
constexpr uint32_t kOperandIndexMask = 0x0FFFFFFFu;

inline constexpr uint32_t encode_operand(OperandKind k, uint32_t index) {
    return (static_cast<uint32_t>(k) << kOperandKindShift) | (index & kOperandIndexMask);
}
inline constexpr OperandKind operand_kind(uint32_t ref) {
    return static_cast<OperandKind>((ref >> kOperandKindShift) & 0xF);
}
inline constexpr uint32_t operand_index(uint32_t ref) { return ref & kOperandIndexMask; }

// Read-only builtin variable ids (subset of the original result-table builtins).
enum class Builtin : uint32_t {
    Ticks = 0,    // seconds-equivalent: logic tick counter
    Result = 1,   // accumulator / last return value
    Health = 2,   // local player health
    NearType = 3,
    NearDist = 4,
    NearId = 5,
    Wind = 6,
    Mana = 7,
};

} // namespace opennova::wac

#endif // OPENNOVA_WAC_BYTECODE_H
