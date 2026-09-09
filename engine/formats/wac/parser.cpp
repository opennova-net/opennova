#include <formats/wac/parser.h>

#include <array>

#include <formats/wac/command.h>
#include <formats/wac/lexer.h>

// [orig: Script_Compile @0x4f31f0 — the shunting-yard statement/expression parser (operator
//  precedence via Script_GetOperatorPrecedence, parameters via WacScript_ResolveParameter @0x4f2920)]

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
    bool next_is_kw(std::string_view low) const {
        if (pos_ + 1 >= toks_.size()) return false;
        const Token &t = toks_[pos_ + 1];
        return t.kind == TokKind::Keyword && t.lowered == low;
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
        if (kw_in({"var", "array", "run"})) {
            out.kind = is_kw("run") ? Stmt::Run : Stmt::Declaration;
            out.array_declaration = is_kw("array");
            out.call.line = cur().line;
            advance();
            if (cur().kind != TokKind::Word && cur().kind != TokKind::String) {
                error("expected a name");
                return false;
            }
            out.call.name = cur().text;
            advance();
            return true;
        }
        if (kw_in({"else", "elseif", "endif", "end", "enif", "then", "next", "enddo"})) {
            // Stray block terminator at statement scope; skip.
            advance();
            return false;
        }
        if (cur().kind == TokKind::Word) {
            if (pos_ + 1 < toks_.size() && toks_[pos_ + 1].text == "=") {
                out.kind = Stmt::Assignment;
                out.assignment_target.text = cur().text;
                out.call.line = cur().line;
                advance();
                advance();
                out.cond = parse_expr();
            } else {
                out.cond = parse_expr();
                if (out.cond.kind == Expr::Leaf && out.cond.call.name != kOperandMarker) {
                    out.kind = Stmt::Action;
                    out.call = out.cond.call;
                } else out.kind = Stmt::Expression;
            }
            return true;
        }
        if (cur().kind == TokKind::LParen || is_op("-") || is_op("not") || is_op("!")) {
            out.kind = Stmt::Expression;
            out.cond = parse_expr();
            return true;
        }
        // Anything else (stray operator/paren) — skip to recover.
        advance();
        return false;
    }

    IfMode parse_if_mode() {
        IfMode mode = IfMode::Then;
        if (is_kw("enter")) mode = IfMode::Enter;
        else if (is_kw("leave")) mode = IfMode::Leave;
        else if (!is_kw("then")) {
            warn("expected 'then', 'enter' or 'leave'");
            return mode;
        }
        advance();
        return mode;
    }

    void parse_event_name(std::string &name) {
        if (cur().kind != TokKind::LBracket) return;
        advance();
        if (cur().kind == TokKind::Word || cur().kind == TokKind::String) {
            name = cur().text;
            advance();
        } else error("expected an event name");
        if (cur().kind == TokKind::RBracket) advance();
        else error("expected ']'");
    }

    bool parse_if(Stmt &out) {
        out.kind = Stmt::If;
        advance(); // 'if'
        parse_event_name(out.event_name);
        out.cond = parse_expr();
        parse_event_name(out.event_name);
        out.mode = parse_if_mode();
        out.body = parse_block_until({"else", "elseif", "endif", "end", "enif"});
        // Retail chains alternatives as `elseif` OR the two-word `else if`
        // (the shipped corpus' dominant form — e.g. 00TRg.wac), both closed by
        // the chain's ONE endif. `else` directly followed by `if` is that
        // chain, never a nested if: nesting would hand the single endif to the
        // inner if and fold every trailing statement into the else branch.
        while (is_kw("elseif") || (is_kw("else") && next_is_kw("if"))) {
            if (is_kw("elseif")) {
                advance();
            } else {
                advance(); // 'else'
                advance(); // 'if'
            }
            ElseIf ei;
            parse_event_name(ei.event_name);
            ei.cond = parse_expr();
            parse_event_name(ei.event_name);
            ei.mode = parse_if_mode();
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
        out.call.line = cur().line;
        advance();
        if (out.block_kind == "gloop") {
            const bool parens = cur().kind == TokKind::LParen;
            if (parens) advance();
            if (!parse_arg(out.block_argument)) error("expected a group after GLOOP");
            if (parens) {
                if (cur().kind == TokKind::RParen) advance();
                else error("expected ')' after GLOOP group");
            }
        }
        out.body = parse_block_until({"next", "enddo", "endif", "end", "enif"});
        while (is_kw("next")) {
            advance();
            out.next_bodies.push_back(parse_block_until({"next", "enddo", "endif", "end", "enif"}));
        }
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

    // [orig: Script_GetOperatorPrecedence @0x4EE540] AND and OR share
    // precedence 1; comparisons 2, +/- 3, */% 4, power 5.
    static int precedence(std::string_view op) {
        if (op == "and" || op == "or" || op == "&&" || op == "||" || op == "xor") return 1;
        if (cmp_command(op)) return 2;
        if (op == "+" || op == "-") return 3;
        if (op == "*" || op == "/" || op == "%") return 4;
        if (op == "^") return 5;
        return 0;
    }

    Expr parse_expr(int minimum = 1) {
        Expr left = parse_unary();
        while (cur().kind == TokKind::Operator && precedence(cur().lowered) >= minimum) {
            const std::string op = cur().lowered;
            const int level = precedence(op);
            advance();
            Expr expression;
            expression.kind = Expr::Binary;
            expression.op = op;
            expression.kids.push_back(std::move(left));
            expression.kids.push_back(parse_expr(level + 1));
            left = std::move(expression);
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
        if (is_op("-")) {
            Expr e;
            e.call.name = kOperandMarker;
            e.call.line = cur().line;
            Arg a;
            if (parse_arg(a)) e.call.args.push_back(std::move(a));
            else error("expected a value after '-'");
            return e;
        }
        if (cur().kind == TokKind::LParen) {
            advance();
            Expr e;
            e.kind = Expr::Group;
            e.kids.push_back(parse_expr());
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
                e.call.line = cur().line;
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
