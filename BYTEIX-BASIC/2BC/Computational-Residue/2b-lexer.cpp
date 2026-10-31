#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

enum TokType {
    T_INT, T_CONSTANT, T_RETURN, T_IF, T_ELSE, T_DURING, T_EXEC_PARENT,
    T_IDENT, T_NUMBER, T_STRING,
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE,
    T_EQUALS, T_EQEQ, T_NE, T_GREATER, T_LESS, T_GE, T_LE,
    T_SEMI, T_COLON, T_PLUS, T_PLUS_EQ, T_MINUS, T_STAR, T_SLASH, T_EOF
};

struct Token { TokType type; std::string text; int line; int indent; };
struct LexResult { std::vector<Token> tokens; int errors; };

static const char *tok_name(TokType t) {
    switch (t) {
        case T_INT: return "INT"; case T_CONSTANT: return "CONSTANT";
        case T_RETURN: return "RETURN"; case T_IF: return "IF";
        case T_ELSE: return "ELSE"; case T_DURING: return "DURING"; case T_EXEC_PARENT: return "EXEC_PARENT";
        case T_IDENT: return "IDENT"; case T_NUMBER: return "NUMBER";
        case T_STRING: return "STRING"; case T_LPAREN: return "LPAREN";
        case T_RPAREN: return "RPAREN"; case T_LBRACE: return "LBRACE";
        case T_RBRACE: return "RBRACE"; case T_EQUALS: return "EQUALS";
        case T_EQEQ: return "EQEQ"; case T_NE: return "NOT_EQUAL";
        case T_GREATER: return "GREATER"; case T_LESS: return "LESS";
        case T_GE: return "GREATER_EQUAL"; case T_LE: return "LESS_EQUAL";
        case T_SEMI: return "SEMI"; case T_COLON: return "COLON";
        case T_PLUS: return "PLUS"; case T_PLUS_EQ: return "PLUS_EQUAL"; case T_MINUS: return "MINUS";
        case T_STAR: return "STAR"; case T_SLASH: return "SLASH";
        case T_EOF: return "EOF";
    }
    return "?";
}
static bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static void lex_error(const char *file, int line, const char *msg, const char *detail) {
    fprintf(stderr, "%s:%d: error: %s%s\n", file, line, msg, detail);
}

LexResult lex(const std::string &src, const char *file) {
    LexResult r; r.errors = 0;
    size_t i = 0, n = src.size(); int line = 1, indent = 0;
    bool line_start = true;
    while (i < n) {
        if (line_start) {
            indent = 0;
            while (i < n && (src[i] == ' ' || src[i] == '\t' || src[i] == '\r')) {
                if (src[i] == ' ') indent++;
                else if (src[i] == '\t') indent += 4;
                i++;
            }
            if (i >= n) break;
            if (src[i] == '\n') { line++; i++; continue; }
            line_start = false;
        }
        char c = src[i];
        if (c == '\n') { line++; i++; line_start = true; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '~' && i + 1 < n && src[i + 1] == '!') {
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (is_ident_start(c)) {
            size_t s = i;
            while (i < n && (is_ident_start(src[i]) || is_digit(src[i]))) i++;
            std::string w = src.substr(s, i - s); TokType t = T_IDENT;
            if (w == "int") t = T_INT;
            else if (w == "constant") t = T_CONSTANT;
            else if (w == "return") t = T_RETURN;
            else if (w == "if") t = T_IF;
            else if (w == "else") t = T_ELSE;
            else if (w == "during") t = T_DURING;
            else if (w == "execparentcode") t = T_EXEC_PARENT;
            r.tokens.push_back({t, w, line, indent}); continue;
        }
        if (is_digit(c)) {
            size_t s = i; while (i < n && is_digit(src[i])) i++;
            r.tokens.push_back({T_NUMBER, src.substr(s, i - s), line, indent}); continue;
        }
        if (c == '"') {
            int start_line = line; std::string val; i++; bool closed = false;
            while (i < n) {
                if (src[i] == '"') { closed = true; i++; break; }
                if (src[i] == '\n') break;
                if (src[i] == '/' && i + 1 < n && src[i + 1] == 'n') { val += '\n'; i += 2; continue; }
                val += src[i++];
            }
            if (!closed) { lex_error(file, start_line, "unterminated string literal", ""); r.errors++; }
            else r.tokens.push_back({T_STRING, val, start_line, indent});
            continue;
        }
        TokType t; size_t advance = 1; std::string spelling(1, c);
        if (i + 1 < n) {
            std::string two = src.substr(i, 2);
            if (two == "==") { t = T_EQEQ; advance = 2; spelling = two; }
            else if (two == "!=") { t = T_NE; advance = 2; spelling = two; }
            else if (two == ">=") { t = T_GE; advance = 2; spelling = two; }
            else if (two == "<=") { t = T_LE; advance = 2; spelling = two; }
            else if (two == "+=") { t = T_PLUS_EQ; advance = 2; spelling = two; }
            else t = T_EOF;
        } else t = T_EOF;
        if (t == T_EOF) {
            switch (c) {
                case '(': t = T_LPAREN; break; case ')': t = T_RPAREN; break;
                case '{': t = T_LBRACE; break; case '}': t = T_RBRACE; break;
                case '=': t = T_EQUALS; break; case '>': t = T_GREATER; break;
                case '<': t = T_LESS; break; case ';': t = T_SEMI; break;
                case ':': t = T_COLON; break; case '+': t = T_PLUS; break;
                case '-': t = T_MINUS; break; case '*': t = T_STAR; break;
                case '/': t = T_SLASH; break;
                default: { char d[8] = {'\'', c, '\'', 0}; lex_error(file, line, "unexpected character ", d); r.errors++; i++; continue; }
            }
        }
        r.tokens.push_back({t, spelling, line, indent}); i += advance;
    }
    r.tokens.push_back({T_EOF, "", line, 0}); return r;
}

static bool read_file(const char *path, std::string &out) {
    FILE *f = fopen(path, "rb"); if (!f) return false;
    char buf[4096]; size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, got);
    fclose(f); return true;
}
#ifdef LEXER_STANDALONE
int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <file.2b>\n", argv[0]); return 1; }
    size_t len = strlen(argv[1]);
    if (len < 3 || strcmp(argv[1] + len - 3, ".2b") != 0) { fprintf(stderr, "error: source file must have a .2b extension\n"); return 1; }
    std::string src;
    if (!read_file(argv[1], src)) { fprintf(stderr, "error: cannot open '%s'\n", argv[1]); return 1; }
    LexResult r = lex(src, argv[1]);
    if (r.errors) { fprintf(stderr, "%d error(s); not compiling.\n", r.errors); return 1; }
    for (const Token &t : r.tokens) {
        printf("%3d  %-13s ", t.line, tok_name(t.type));
        if (t.type == T_STRING) { putchar('"'); for (char c : t.text) { if (c == '\n') printf("\\n"); else putchar(c); } puts("\""); }
        else puts(t.text.c_str());
    }
    return 0;
}
#endif
