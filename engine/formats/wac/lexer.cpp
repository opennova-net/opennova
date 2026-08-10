#include "wac/lexer.h"

#include <array>
#include <cctype>

#include <io/strutil.h>

namespace opennova::wac {
namespace {

using opennova::strutil::to_lower;

bool is_word_char(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == '#' || c == '.' || c == ':';
}

} // namespace

bool is_wac_keyword(std::string_view w) {
    static const char *kw[] = {
        "if", "then", "else", "elseif", "endif", "end", "enif",
        "do", "dornd", "doseq", "next", "enddo",
        "ploop", "gloop", "var", "array", "run", "enter", "leave",
    };
    for (const char *k : kw) {
        if (w == k) return true;
    }
    return false;
}

bool is_word_operator(std::string_view w) {
    return w == "and" || w == "or" || w == "xor" || w == "not";
}

std::vector<Token> lex(std::string_view src) {
    std::vector<Token> toks;
    int line = 1;
    int col = 1;
    size_t i = 0;
    const size_t n = src.size();

    auto last_value_like = [&]() -> bool {
        if (toks.empty()) return false;
        TokKind k = toks.back().kind;
        return k == TokKind::Word || k == TokKind::String || k == TokKind::RParen;
    };
    auto advance = [&](size_t count) {
        for (size_t k = 0; k < count && i < n; ++k) {
            if (src[i] == '\n') { ++line; col = 1; } else { ++col; }
            ++i;
        }
    };
    auto skip_to_eol = [&]() {
        while (i < n && src[i] != '\n') advance(1);
    };
    auto push = [&](TokKind kind, std::string text, int tl, int tc) {
        Token t;
        t.kind = kind;
        t.lowered = to_lower(text);
        t.text = std::move(text);
        t.line = tl;
        t.col = tc;
        toks.push_back(std::move(t));
    };

    while (i < n) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(1); continue; }

        int tl = line, tc = col;

        if (c == ';') { skip_to_eol(); continue; }
        if (c == '/') {
            if (i + 1 < n && src[i + 1] == '/') { skip_to_eol(); continue; }
            if (last_value_like()) { advance(1); push(TokKind::Operator, "/", tl, tc); continue; }
            skip_to_eol();
            continue;
        }
        if (c == '"') {
            advance(1); // opening quote
            std::string s;
            while (i < n && src[i] != '"' && src[i] != '\n') { s.push_back(src[i]); advance(1); }
            if (i < n && src[i] == '"') advance(1); // closing quote
            push(TokKind::String, std::move(s), tl, tc);
            continue;
        }
        if (c == '(') { advance(1); push(TokKind::LParen, "(", tl, tc); continue; }
        if (c == ')') { advance(1); push(TokKind::RParen, ")", tl, tc); continue; }
        if (c == ',') { advance(1); push(TokKind::Comma, ",", tl, tc); continue; }

        // Two-char operators.
        if (i + 1 < n) {
            char d = src[i + 1];
            std::array<const char *, 8> two = {"<=", ">=", "==", "!=", "<>", "~=", "&&", "||"};
            std::string pair = {c, d};
            for (const char *op : two) {
                if (pair == op) {
                    advance(2);
                    push(TokKind::Operator, pair, tl, tc);
                    goto next_token;
                }
            }
        }
        // One-char operators.
        if (c == '<' || c == '>' || c == '+' || c == '-' || c == '*' || c == '%' ||
            c == '^' || c == '!' || c == '=') {
            advance(1);
            push(TokKind::Operator, std::string(1, c), tl, tc);
            continue;
        }

        // Word.
        if (is_word_char(c)) {
            std::string w;
            while (i < n && is_word_char(src[i])) { w.push_back(src[i]); advance(1); }
            std::string low = to_lower(w);
            if (is_wac_keyword(low)) {
                push(TokKind::Keyword, std::move(w), tl, tc);
            } else if (is_word_operator(low)) {
                push(TokKind::Operator, std::move(w), tl, tc);
            } else {
                push(TokKind::Word, std::move(w), tl, tc);
            }
            continue;
        }

        // Unknown char — recover by skipping.
        advance(1);
    next_token:;
    }

    Token end;
    end.kind = TokKind::End;
    end.line = line;
    end.col = col;
    toks.push_back(end);
    return toks;
}

} // namespace opennova::wac
