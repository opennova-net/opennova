#include "wac/parser.h"

#include <array>

#include "wac/command.h"
#include "wac/lexer.h"

namespace opennova::wac {
namespace {

// Marker name for a bare operand used directly as a condition / comparison side.
constexpr const char *kOperandMarker = "$operand";

const char *cmp_command(std::string_view op) {
    if (op == "==") return "eq";
    if (op == "!=" || op == "<>" || op == "~=") return "ne";
    if (op == "<") return "lt";
    if (op == ">") return "gt";
    if (op == "<=") return "le";
    if (op == ">=") return "ge";
    return nullptr;
}

class Parser {
public:
    explicit Parser(std::string_view src) : toks_(lex(src)) {}

    ParseResult run() {
        ParseResult r;
        while (!at_end()) {
            Stmt s;
            if (parse_statement(s)) {
                r.statements.push_back(std::move(s));
            }
        }
        r.diagnostics = std::move(diags_);
        return r;
    }

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;
    std::vector<Diagnostic> diags_;

    const Token &cur() const { return toks_[pos_]; }
    bool at_end() const { return cur().kind == TokKind::End; }
    void advance() { if (!at_end()) ++pos_; }

    bool is_kw(std::string_view low) const {
        return cur().kind == TokKind::Keyword && cur().lowered == low;
    }
    bool is_op(std::string_view low) const {
        return cur().kind == TokKind::Operator && cur().lowered == low;
    }
    void warn(const std::string &msg) {
        diags_.push_back(Diagnostic{cur().line, cur().col, msg, false});
    }
    void error(const std::string &msg) {
        diags_.push_back(Diagnostic{cur().line, cur().col, msg, true});
    }

    bool kw_in(std::initializer_list<const char *> set) const {
        if (cur().kind != TokKind::Keyword) return false;
        for (const char *k : set) {
            if (cur().lowered == k) return true;
        }
        return false;
    }

    // ---- statements ----
    bool parse_statement(Stmt &out) {
        if (at_end()) return false;
        if (is_kw("if")) return parse_if(out);
        if (kw_in({"do", "dornd", "doseq", "ploop", "gloop"})) return parse_block(out);
        if (kw_in({"var", "array"})) { skip_decl(); return false; }
        if (is_kw("run") || is_kw("enter") || is_kw("leave")) { skip_line_word(); return false; }
        if (kw_in({"else", "elseif", "endif", "end", "enif", "then", "next", "enddo"})) {
            // Stray block terminator at statement scope; skip.
            advance();
            return false;
        }
        if (cur().kind == TokKind::Word) {
            out.kind = Stmt::Action;
            out.call = parse_call();
            return true;
        }
        // Anything else (stray operator/paren) — skip to recover.
        advance();
        return false;
    }

    bool parse_if(Stmt &out) {
        out.kind = Stmt::If;
        advance(); // 'if'
        out.cond = parse_expr();
        if (is_kw("then")) advance(); else warn("expected 'then'");
        out.body = parse_block_until({"else", "elseif", "endif", "end", "enif"});
        while (is_kw("elseif")) {
            advance();
            ElseIf ei;
            ei.cond = parse_expr();
            if (is_kw("then")) advance(); else warn("expected 'then' after elseif");
            ei.body = parse_block_until({"else", "elseif", "endif", "end", "enif"});
            out.elifs.push_back(std::move(ei));
        }
        if (is_kw("else")) {
            advance();
            out.has_else = true;
            out.else_body = parse_block_until({"endif", "end", "enif"});
        }
        if (kw_in({"endif", "end", "enif"})) {
            advance();
        } else {
            warn("expected 'endif'");
        }
        return true;
    }

    bool parse_block(Stmt &out) {
        out.kind = Stmt::Block;
        out.block_kind = cur().lowered;
        advance();
        // Body runs to 'enddo' (loops) / 'endif'. 'next' separates sections — we
        // flatten them in this milestone (full loop iteration semantics deferred).
        out.body = parse_block_until({"enddo", "endif", "end", "enif"});
        if (kw_in({"enddo", "endif", "end", "enif"})) advance();
        return true;
    }

    std::vector<Stmt> parse_block_until(std::initializer_list<const char *> stop) {
        std::vector<Stmt> body;
        while (!at_end() && !kw_in(stop)) {
            // 'next' inside a loop body separates sections; consume + continue.
            if (is_kw("next")) { advance(); continue; }
            Stmt s;
            size_t before = pos_;
            if (parse_statement(s)) body.push_back(std::move(s));
            if (pos_ == before) advance(); // guard against no-progress
        }
        return body;
    }

    void skip_decl() {
        advance(); // var/array
        if (cur().kind == TokKind::Word) advance(); // name
        // optional size / extras on the line — consume words/numbers until a
        // keyword or end.
        while (!at_end() && cur().kind == TokKind::Word) advance();
    }
    void skip_line_word() {
        advance();
        if (cur().kind == TokKind::Word) advance();
    }

    // ---- calls ----
    Call parse_call() {
        Call c;
        c.name = cur().text;
        c.line = cur().line;
        advance();
        const CommandDef *def = wac_find_command(c.name);
        if (cur().kind == TokKind::LParen) {
            advance();
            while (!at_end() && cur().kind != TokKind::RParen) {
                Arg a;
                if (parse_arg(a)) c.args.push_back(std::move(a));
                if (cur().kind == TokKind::Comma) advance();
                else if (cur().kind != TokKind::RParen) {
                    // tolerate stray tokens inside the arg list
                    if (cur().kind != TokKind::Word && cur().kind != TokKind::String &&
                        !is_op("-")) {
                        break;
                    }
                }
            }
            if (cur().kind == TokKind::RParen) advance();
        } else {
            // Paren-less: collect up to argc primaries (e.g. `chain 7`, `random 3`).
            int want = def ? def->argc : 0;
            for (int i = 0; i < want; ++i) {
                if (!arg_starts_here()) break;
                Arg a;
                if (!parse_arg(a)) break;
                c.args.push_back(std::move(a));
                if (cur().kind == TokKind::Comma) advance();
            }
        }
        return c;
    }

    bool arg_starts_here() const {
        return cur().kind == TokKind::Word || cur().kind == TokKind::String || is_op("-");
    }

    bool parse_arg(Arg &out) {
        if (is_op("-")) {
            advance();
            if (cur().kind == TokKind::Word) {
                out.text = cur().text;
                out.negate = true;
                advance();
                return true;
            }
            return false;
        }
        if (cur().kind == TokKind::String) {
            out.text = cur().text;
            out.is_string = true;
            advance();
            return true;
        }
        if (cur().kind == TokKind::Word) {
            out.text = cur().text;
            advance();
            return true;
        }
        return false;
    }

    // ---- expressions (xor < or < and < cmp < unary < primary) ----
    Expr parse_expr() { return parse_xor(); }

    Expr parse_xor() {
        Expr e = parse_or();
        while (is_op("xor")) {
            advance();
            Expr rhs = parse_or();
            Expr combined;
            combined.kind = Expr::Xor;
            combined.kids.push_back(std::move(e));
            combined.kids.push_back(std::move(rhs));
            e = std::move(combined);
        }
        return e;
    }
    Expr parse_or() {
        Expr e = parse_and();
        while (is_op("or") || is_op("||")) {
            advance();
            Expr rhs = parse_and();
            Expr combined;
            combined.kind = Expr::Or;
            combined.kids.push_back(std::move(e));
            combined.kids.push_back(std::move(rhs));
            e = std::move(combined);
        }
        return e;
    }
    Expr parse_and() {
        Expr e = parse_cmp();
        while (is_op("and") || is_op("&&")) {
            advance();
            Expr rhs = parse_cmp();
            Expr combined;
            combined.kind = Expr::And;
            combined.kids.push_back(std::move(e));
            combined.kids.push_back(std::move(rhs));
            e = std::move(combined);
        }
        return e;
    }
    Expr parse_cmp() {
        Expr left = parse_unary();
        if (cur().kind == TokKind::Operator) {
            const char *cmd = cmp_command(cur().lowered);
            if (cmd) {
                advance();
                Expr right = parse_unary();
                Arg la, ra;
                if (expr_as_operand(left, la) && expr_as_operand(right, ra)) {
                    Expr e;
                    e.kind = Expr::Leaf;
                    e.call.name = cmd;
                    e.call.args.push_back(std::move(la));
                    e.call.args.push_back(std::move(ra));
                    return e;
                }
                // Mixed call/operand comparison — keep the left side as the leaf.
                return left;
            }
        }
        return left;
    }
    Expr parse_unary() {
        if (is_op("not") || is_op("!")) {
            advance();
            Expr child = parse_unary();
            Expr e;
            e.kind = Expr::Not;
            e.kids.push_back(std::move(child));
            return e;
        }
        return parse_primary();
    }
    Expr parse_primary() {
        if (cur().kind == TokKind::LParen) {
            advance();
            Expr e = parse_expr();
            if (cur().kind == TokKind::RParen) advance(); else warn("expected ')'");
            return e;
        }
        if (cur().kind == TokKind::Word) {
            // Command call if it's a registered keyword or directly followed by '('.
            bool is_cmd = wac_find_command(cur().text) != nullptr;
            bool paren_next = (pos_ + 1 < toks_.size() && toks_[pos_ + 1].kind == TokKind::LParen);
            Expr e;
            e.kind = Expr::Leaf;
            if (is_cmd || paren_next) {
                e.call = parse_call();
            } else {
                // bare operand -> truthiness leaf
                e.call.name = kOperandMarker;
                Arg a;
                a.text = cur().text;
                e.call.args.push_back(std::move(a));
                advance();
            }
            return e;
        }
        if (cur().kind == TokKind::String) {
            Expr e;
            e.kind = Expr::Leaf;
            e.call.name = kOperandMarker;
            Arg a;
            a.text = cur().text;
            a.is_string = true;
            e.call.args.push_back(std::move(a));
            advance();
            return e;
        }
        // Empty / unexpected leaf — emit a neutral operand "0".
        Expr e;
        e.kind = Expr::Leaf;
        e.call.name = kOperandMarker;
        Arg a;
        a.text = "0";
        e.call.args.push_back(std::move(a));
        if (!at_end() && cur().kind != TokKind::Keyword) advance();
        return e;
    }

    static bool expr_as_operand(const Expr &e, Arg &out) {
        if (e.kind == Expr::Leaf && e.call.name == kOperandMarker && !e.call.args.empty()) {
            out = e.call.args[0];
            return true;
        }
        return false;
    }
};

} // namespace

ParseResult parse(std::string_view source) {
    Parser p(source);
    return p.run();
}

} // namespace opennova::wac
