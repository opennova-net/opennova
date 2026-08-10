// WAC lexer. Tokenizes .wac source faithfully to Script_Compile's tokenizer
// (Jointops.exe @0x4f31f0): case-insensitive keywords, `;` / `/` / `//` comments,
// optional parens, comma/space-separated args, quoted strings. Error-tolerant:
// the shipped corpus contains typos (e.g. `enif`, bare `never`) and must not
// crash the lexer.
#ifndef OPENNOVA_WAC_LEXER_H
#define OPENNOVA_WAC_LEXER_H

#include <string>
#include <string_view>
#include <vector>

namespace opennova::wac {

enum class TokKind {
    Word,      // command name / operand / variable (letters, digits, _ # . :)
    String,    // "quoted"
    Keyword,   // if/then/else/elseif/endif/end/do/dornd/doseq/next/enddo/ploop/gloop/var/array/run/enter/leave
    Operator,  // symbolic + word operators (and/or/xor/not)
    LParen,
    RParen,
    Comma,
    End,
};

struct Token {
    TokKind kind = TokKind::End;
    std::string text;     // original-case lexeme (operators use the canonical form)
    std::string lowered;  // lower-cased copy for keyword/operator matching
    int line = 1;
    int col = 1;
};

// Lexes the whole source. Never throws; unterminated strings etc. are recovered.
std::vector<Token> lex(std::string_view source);

// Keyword / word-operator classification helpers (used by lexer + parser).
bool is_wac_keyword(std::string_view lowered);
bool is_word_operator(std::string_view lowered);

} // namespace opennova::wac

#endif // OPENNOVA_WAC_LEXER_H
