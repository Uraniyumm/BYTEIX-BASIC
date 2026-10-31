#include "2b-lexer.cpp"
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unordered_map>
#include <functional>
#include <lib/omni-engine.h>

// 2BC -- BYTEIX-BASIC compiler (Linux x86-64)
// Usage: ./2BC program.2b '>>' program


static const uint64_t BASE = 0x400000;
static const size_t HDR = 64 + 56;
typedef std::vector<uint8_t> Bytes;
static void put8(Bytes &b, uint8_t v) { b.push_back(v); }
static void put16(Bytes &b, uint16_t v) { for (int i=0;i<2;i++) b.push_back((v>>(8*i))&255); }
static void put32(Bytes &b, uint32_t v) { for (int i=0;i<4;i++) b.push_back((v>>(8*i))&255); }
static void put64(Bytes &b, uint64_t v) { for (int i=0;i<8;i++) b.push_back((v>>(8*i))&255); }
struct Patch { size_t at, data_off; };
struct CallPatch { size_t at; };
struct NamedCallPatch { size_t at; std::string name; };
struct Symbol { std::string name; int scope; bool constant; bool pointer; bool volatile_u32; };
struct Scope { int parent; };
enum DeclRole { ROLE_PRIMARY, ROLE_CHILD, ROLE_SUBINT, ROLE_SUBCHILD };
struct Table { std::string name; int scope; std::vector<std::string> texts; std::vector<long> values; bool numeric; size_t text_off, val_off; };
struct Slot { int table; int var; };
static const size_t INPUT_BUF = (size_t)-1;   // Patch::data_off sentinel: the line-input buffer
static const size_t INPUT_MAX = 256;
// Runtime helpers (assembled with GNU as, position independent). r14 = match cursor.
static const uint8_t readline_code[]={0x53,0x41,0x54,0x49,0x89,0xFC,0x31,0xDB,0x81,0xFB,0xFF,0x00,0x00,0x00,0x73,0x2F,0x49,0x8D,0x34,0x1C,0x31,0xC0,0x31,0xFF,0xBA,0x01,0x00,0x00,0x00,0x0F,0x05,0x48,0x83,0xF8,0x01,0x75,0x0D,0x41,0x0F,0xB6,0x04,0x1C,0x3C,0x0A,0x74,0x11,0xFF,0xC3,0xEB,0xD6,0x85,0xDB,0x75,0x09,0x41,0xC6,0x04,0x24,0x00,0x41,0x5C,0x5B,0xC3,0x85,0xDB,0x74,0x0A,0x41,0x80,0x7C,0x1C,0xFF,0x0D,0x75,0x02,0xFF,0xCB,0x41,0xC6,0x04,0x1C,0x00,0x41,0x5C,0x5B,0xC3};
static const uint8_t match_lit_code[]={0x4C,0x89,0xF7,0x8A,0x06,0x84,0xC0,0x74,0x0C,0x3A,0x07,0x75,0x11,0x48,0xFF,0xC6,0x48,0xFF,0xC7,0xEB,0xEE,0x49,0x89,0xFE,0xB8,0x01,0x00,0x00,0x00,0xC3,0x31,0xC0,0xC3};
static const uint8_t match_set_code[]={0x31,0xC9,0x80,0x3E,0x00,0x74,0x30,0xFF,0xC1,0x4C,0x89,0xF7,0x49,0x89,0xF0,0x41,0x8A,0x00,0x84,0xC0,0x74,0x1B,0x3A,0x07,0x75,0x08,0x49,0xFF,0xC0,0x48,0xFF,0xC7,0xEB,0xED,0x80,0x3E,0x00,0x74,0x05,0x48,0xFF,0xC6,0xEB,0xF6,0x48,0xFF,0xC6,0xEB,0xD1,0x49,0x89,0xFE,0x89,0xC8,0xC3,0x31,0xC0,0xC3};
struct Declaration { std::string name; DeclRole role; std::vector<Token> body; int scope; int line; };

struct Compiler {
    std::vector<Token> t; const char *file; size_t p=0;
    Bytes code, data; std::vector<Patch> patches; std::vector<CallPatch> int_calls; std::vector<NamedCallPatch> named_calls; std::unordered_map<std::string,size_t> named_functions;
    std::vector<Symbol> symbols; std::vector<Scope> scopes; int active_scope=0;
    size_t frame_patch=0;
    int modify_address_var=-1;
    bool in_named_function=false;
    bool sequence_mode=false;
    std::vector<Table> tables; std::vector<Slot> last_slots; int cap_counter=0;
    std::vector<CallPatch> rl_calls, ml_calls, ms_calls; bool uses_input=false;
    Compiler(const std::vector<Token>& toks,const char*f):t(toks),file(f){}
    const Token &cur() const { return t[p < t.size() ? p : t.size()-1]; }
    bool fail(const char *msg) { fprintf(stderr,"%s:%d: error: %s\n",file,cur().line,msg); return false; }
    bool expect(TokType ty,const char *what) { if(cur().type!=ty){std::string m=std::string("expected ")+what;return fail(m.c_str());} p++;return true; }
    void syscall(){put8(code,0x0F);put8(code,0x05);}
    void exit_code(uint32_t s){put8(code,0xBF);put32(code,s);put8(code,0xB8);put32(code,60);syscall();}
    void write_string(const std::string&s,uint32_t fd=1){
        if(s.empty())return;
        size_t off=data.size();data.insert(data.end(),s.begin(),s.end());
        put8(code,0xB8);put32(code,1); put8(code,0xBF);put32(code,fd);
        put8(code,0x48);put8(code,0x8D);put8(code,0x35);patches.push_back({code.size(),off});put32(code,0);
        put8(code,0xBA);put32(code,(uint32_t)s.size());syscall();
    }
    int find_var(const std::string&n) const {
        int scope=active_scope;
        while(scope>=0){
            for(int i=(int)symbols.size()-1;i>=0;i--) if(symbols[i].scope==scope&&symbols[i].name==n)return i;
            scope=scopes[scope].parent;
        }
        return -1;
    }
    int find_current_var(const std::string&n) const {
        for(int i=(int)symbols.size()-1;i>=0;i--)if(symbols[i].scope==active_scope&&symbols[i].name==n)return i;
        return -1;
    }
    int declare_var(const std::string&n,bool constant){
        if(find_current_var(n)>=0){std::string m="variable '"+n+"' is already declared in this int";fail(m.c_str());return -1;}
        symbols.push_back({n,active_scope,constant,false,false});return (int)symbols.size()-1;
    }
    static int32_t disp(int i){return -8*(i+1);}
    void load(int i){put8(code,0x48);put8(code,0x8B);put8(code,0x85);put32(code,(uint32_t)disp(i));}
    void store(int i){put8(code,0x48);put8(code,0x89);put8(code,0x85);put32(code,(uint32_t)disp(i));}
    void push(){put8(code,0x50);} void pop(){put8(code,0x58);} void mov_rcx_rax(){put8(code,0x48);put8(code,0x89);put8(code,0xC1);}
    size_t jmp(uint8_t op1,uint8_t op2=0){if(op2){put8(code,op1);put8(code,op2);}else put8(code,op1);size_t at=code.size();put32(code,0);return at;}
    void patch_to(size_t at,size_t target){int32_t rel=(int32_t)(target-(at+4));for(int i=0;i<4;i++)code[at+i]=(uint8_t)((uint32_t)rel>>(8*i));}
    void patch_here(size_t at){patch_to(at,code.size());}
    void call_print_int(){put8(code,0xE8);int_calls.push_back({code.size()});put32(code,0);}

    size_t add_cstr(const std::string&x){size_t off=data.size();data.insert(data.end(),x.begin(),x.end());data.push_back(0);return off;}
    void lea_data(uint8_t rex,uint8_t modrm,size_t off){put8(code,rex);put8(code,0x8D);put8(code,modrm);patches.push_back({code.size(),off});put32(code,0);}
    void call_to(std::vector<CallPatch>&v){put8(code,0xE8);v.push_back({code.size()});put32(code,0);}
    int find_table(const std::string&n) const {
        int scope=active_scope;
        while(scope>=0){
            for(int i=(int)tables.size()-1;i>=0;i--) if(tables[i].scope==scope&&tables[i].name==n)return i;
            scope=scopes[scope].parent;
        }
        return -1;
    }
    // NAME = { item, item, ... }   items: numbers, 'c' characters, "strings", + - * /
    bool parse_table_decl(){
        Table tb;tb.name=cur().text;tb.scope=active_scope;tb.numeric=true;p+=3;
        for(const Table&o:tables)if(o.scope==active_scope&&o.name==tb.name)return fail("data table is already declared in this int");
        while(cur().type!=T_RBRACE){
            if(cur().type==T_EOF)return fail("missing '}' in data table");
            if(cur().type==T_COMMA){p++;continue;}
            std::string txt;long val=0;
            switch(cur().type){
                case T_NUMBER:{bool hex=cur().text.size()>2&&cur().text[0]=='0'&&(cur().text[1]=='x'||cur().text[1]=='X');val=strtol(cur().text.c_str(),NULL,hex?16:10);txt=std::to_string(val);break;}
                case T_CHARLIT:val=strtol(cur().text.c_str(),NULL,10);txt=std::string(1,(char)val);tb.numeric=false;break;
                case T_STRING:txt=cur().text;tb.numeric=false;break;
                case T_PLUS:txt="+";val='+';tb.numeric=false;break;
                case T_MINUS:txt="-";val='-';tb.numeric=false;break;
                case T_STAR:txt="*";val='*';tb.numeric=false;break;
                case T_SLASH:txt="/";val='/';tb.numeric=false;break;
                default:return fail("data table items must be numbers, characters, strings, or + - * /");
            }
            if(txt.empty())return fail("data table items cannot be empty");
            tb.texts.push_back(txt);tb.values.push_back(val);p++;
        }
        p++;
        if(tb.texts.empty())return fail("data table must have at least one item");
        if(cur().type==T_SEMI)p++;
        tb.text_off=data.size();
        for(const std::string&x:tb.texts){data.insert(data.end(),x.begin(),x.end());data.push_back(0);}
        data.push_back(0);
        tb.val_off=data.size();
        for(long v:tb.values)for(int k=0;k<8;k++)data.push_back((uint8_t)(((uint64_t)(int64_t)v)>>(8*k)));
        tables.push_back(tb);
        return true;
    }
    bool input_compare_ahead() const {
        return cur().type==T_IDENT&&cur().text=="input"&&p+4<t.size()&&(t[p+1].type==T_EQUALS||t[p+1].type==T_EQEQ)&&
               t[p+2].type==T_LPAREN&&t[p+3].type==T_STRING&&t[p+4].type==T_RPAREN;
    }
    void emit_read_input(){uses_input=true;lea_data(0x48,0x3D,INPUT_BUF);call_to(rl_calls);}
    // Matches the input line against a pattern: literal text plus {TABLE} slots.
    // Every failure jump is appended to `fails`; captured item indexes go in hidden variables.
    bool compile_input_match(const std::string&pat,std::vector<size_t>&fails){
        struct Part{bool slot;std::string lit;int table;};
        std::vector<Part> parts;std::string lit;
        for(size_t i=0;i<pat.size();i++){
            size_t close;
            if(pat[i]=='{'&&(close=pat.find('}',i+1))!=std::string::npos){
                std::string name=pat.substr(i+1,close-i-1);int ti=find_table(name);
                if(ti<0){std::string m="unknown data table '"+name+"' in input pattern";return fail(m.c_str());}
                if(!lit.empty()){parts.push_back({false,lit,-1});lit.clear();}
                parts.push_back({true,"",ti});i=close;
            } else lit+=pat[i];
        }
        if(!lit.empty())parts.push_back({false,lit,-1});
        last_slots.clear();
        lea_data(0x4C,0x35,INPUT_BUF);                       // r14 = cursor = start of the line
        for(const Part&pt:parts){
            if(!pt.slot){lea_data(0x48,0x35,add_cstr(pt.lit));call_to(ml_calls);}
            else{lea_data(0x48,0x35,tables[pt.table].text_off);call_to(ms_calls);}
            put8(code,0x48);put8(code,0x85);put8(code,0xC0);  // test rax,rax
            fails.push_back(jmp(0x0F,0x84));                  // jz  -> no match
            if(pt.slot){int v=declare_var("$cap"+std::to_string(cap_counter++),false);if(v<0)return false;store(v);last_slots.push_back({pt.table,v});}
        }
        put8(code,0x41);put8(code,0x80);put8(code,0x3E);put8(code,0x00); // cmp byte [r14],0
        fails.push_back(jmp(0x0F,0x85));                      // jne -> trailing text
        return true;
    }
    void load_capture_value(const Slot&sl){
        load(sl.var);lea_data(0x48,0x0D,tables[sl.table].val_off);
        put8(code,0x48);put8(code,0x8B);put8(code,0x44);put8(code,0xC1);put8(code,0xF8); // mov rax,[rcx+rax*8-8]
    }
    // DIGIT + OPERATION + DIGIT : evaluate the captures of the last matched pattern and print the result.
    bool emit_eval(const std::vector<int>&terms){
        if(last_slots.size()!=3||terms.size()!=3)return fail("expression must be: NUMBERTABLE + OPERATORTABLE + NUMBERTABLE, repeating the input pattern");
        for(int i=0;i<3;i++)if(terms[i]!=last_slots[i].table)return fail("expression must name the input pattern's data tables in the same order");
        if(!tables[last_slots[0].table].numeric||!tables[last_slots[2].table].numeric)return fail("operands of an expression must come from number-only data tables");
        for(const std::string&x:tables[last_slots[1].table].texts)if(x!="+"&&x!="-"&&x!="*"&&x!="/")return fail("the operator data table may only contain + - * /");
        load_capture_value(last_slots[0]);push();
        load_capture_value(last_slots[2]);mov_rcx_rax();pop();            // rax=a rcx=b
        put8(code,0x48);put8(code,0x8B);put8(code,0x95);put32(code,(uint32_t)disp(last_slots[1].var)); // mov rdx,[op]
        lea_data(0x4C,0x05,tables[last_slots[1].table].val_off);          // lea r8,[op values]
        put8(code,0x49);put8(code,0x8B);put8(code,0x54);put8(code,0xD0);put8(code,0xF8); // mov rdx,[r8+rdx*8-8]
        std::vector<size_t> to_print,to_end;
        auto arm=[&](uint8_t ch,std::function<void()> body){
            put8(code,0x48);put8(code,0x83);put8(code,0xFA);put8(code,ch);  // cmp rdx,ch
            size_t nx=jmp(0x0F,0x85);body();to_print.push_back(jmp(0xE9));patch_here(nx);
        };
        arm('+',[&]{put8(code,0x48);put8(code,0x01);put8(code,0xC8);});
        arm('-',[&]{put8(code,0x48);put8(code,0x29);put8(code,0xC8);});
        arm('*',[&]{put8(code,0x48);put8(code,0x0F);put8(code,0xAF);put8(code,0xC1);});
        arm('/',[&]{
            put8(code,0x48);put8(code,0x85);put8(code,0xC9);               // test rcx,rcx
            size_t ok=jmp(0x0F,0x85);
            write_string("error: division by zero\n",2);
            to_end.push_back(jmp(0xE9));patch_here(ok);
            put8(code,0x48);put8(code,0x99);put8(code,0x48);put8(code,0xF7);put8(code,0xF9); // cqo; idiv rcx
        });
        for(size_t j:to_print)patch_here(j);
        call_print_int();write_string("\n");
        for(size_t j:to_end)patch_here(j);
        return true;
    }
    // `example:` is a documentation block: skip its line and every more-indented line under it.
    bool skip_example(){
        if(!(cur().type==T_IDENT&&cur().text=="example"&&p+1<t.size()&&t[p+1].type==T_COLON))return false;
        int base=cur().indent,line=cur().line;p+=2;
        while(cur().type!=T_EOF&&(cur().line==line||cur().indent>base))p++;
        return true;
    }
    void skip_then(){if(cur().type==T_IDENT&&cur().text=="then"&&p+1<t.size()&&t[p+1].type==T_COLON)p+=2;}
    bool factor(){
        if(cur().type==T_NUMBER || cur().type==T_CHARLIT){unsigned long v=strtoul(cur().text.c_str(),NULL,(cur().type==T_NUMBER&&cur().text.size()>2&&cur().text[0]=='0'&&(cur().text[1]=='x'||cur().text[1]=='X'))?16:10);if(v>0xffffffffUL)return fail("integer literal too large (maximum 0xFFFFFFFF)");put8(code,0xB8);put32(code,(uint32_t)v);p++;return true;}
        if(cur().type==T_IDENT && cur().text=="char" && find_var("char")<0){
            // In sequence mode, child-body numeric rows supply char values directly.
            // Otherwise, char reads and caches one byte from standard input.
            int idx=declare_var("char",false);if(idx<0)return false;
            if(!sequence_mode){
                const uint8_t read_char[]={0x48,0x83,0xEC,0x08,0x48,0x89,0xE6,0x31,0xC0,0x31,0xFF,0xBA,0x01,0x00,0x00,0x00,0x0F,0x05,0x0F,0xB6,0x04,0x24,0x48,0x83,0xC4,0x08};
                code.insert(code.end(),read_char,read_char+sizeof(read_char));store(idx);
            }
            load(idx);p++;return true;
        }
        if(cur().type==T_NULL){put8(code,0x31);put8(code,0xC0);p++;return true;}
        if(cur().type==T_LBRACE){
            p++; if(cur().type!=T_IDENT)return fail("expected a variable name inside memory reference braces");
            std::string n=cur().text;int i=find_var(n);if(i<0){std::string m="undefined variable '"+n+"' in memory reference";return fail(m.c_str());}p++;
            if(!expect(T_RBRACE,"'}' after memory reference"))return false;
            load(i);
            if(symbols[i].volatile_u32){put8(code,0x8B);put8(code,0x00);}else{put8(code,0x48);put8(code,0x8B);put8(code,0x00);}
            return true;
        }
        if(cur().type==T_IDENT){std::string n=cur().text;int i=find_var(n);if(i<0){std::string m="undefined variable '"+n+"'";return fail(m.c_str());}p++;if(cur().type==T_LPAREN&&p+1<t.size()&&t[p+1].type==T_RPAREN){p+=2;}load(i);return true;}
        if(cur().type==T_LPAREN){p++;if(!expr())return false;return expect(T_RPAREN,"')'");}
        if(cur().type==T_MINUS){p++;if(!factor())return false;put8(code,0x48);put8(code,0xF7);put8(code,0xD8);return true;}
        return fail("expected a number, variable, or parenthesized expression");
    }
    bool term(){if(!factor())return false;while(cur().type==T_STAR||cur().type==T_SLASH){TokType op=cur().type;p++;push();if(!factor())return false;mov_rcx_rax();pop();if(op==T_STAR){put8(code,0x48);put8(code,0x0F);put8(code,0xAF);put8(code,0xC1);}else{put8(code,0x48);put8(code,0x99);put8(code,0x48);put8(code,0xF7);put8(code,0xF9);}}return true;}
    bool expr(){if(!term())return false;while(cur().type==T_PLUS||cur().type==T_MINUS){TokType op=cur().type;p++;push();if(!term())return false;mov_rcx_rax();pop();put8(code,0x48);if(op==T_PLUS){put8(code,0x01);put8(code,0xC8);}else{put8(code,0x29);put8(code,0xC8);}}return true;}
    size_t emit_false_jump(TokType op){switch(op){case T_EQUALS:case T_EQEQ:return jmp(0x0F,0x85);case T_NE:return jmp(0x0F,0x84);case T_GREATER:return jmp(0x0F,0x8E);case T_LESS:return jmp(0x0F,0x8D);case T_GE:return jmp(0x0F,0x8C);case T_LE:return jmp(0x0F,0x8F);default:return jmp(0x0F,0x85);}}
    bool print_interpolated(const std::string&s){
        // In a character-sequence callback, /n in the callback's printf marks
        // the logical end of the result, while the input row owns the newline.
        // This lets one callback produce several space-separated results per row.
        std::string printable = s;
        if (sequence_mode && in_named_function && !printable.empty() && printable.back() == '\n')
            printable.pop_back();
        const std::string &output = printable;
        size_t pos=0;
        while(pos<output.size()){
            size_t open=output.find('{',pos);
            if(open==std::string::npos){write_string(output.substr(pos));break;}
            size_t close=output.find('}',open+1);
            if(close==std::string::npos){write_string(output.substr(pos));break;}
            write_string(output.substr(pos,open-pos));
            std::string name=output.substr(open+1,close-open-1);
            int idx=find_var(name);
            if(idx<0){std::string m="unknown variable '"+name+"' in string interpolation";return fail(m.c_str());}
            load(idx);if(symbols[idx].pointer){put8(code,0x48);put8(code,0x8B);put8(code,0x00);}else if(symbols[idx].volatile_u32){put8(code,0x8B);put8(code,0x00);}call_print_int();pos=close+1;
        }
        return true;
    }
    bool if_statement(){
        std::vector<size_t> end_jumps;
        TokType branch=cur().type;
        bool have_input=false;
        while(branch==T_IF || branch==T_ELIF){
            p++;
            std::vector<size_t> jfs;
            if(input_compare_ahead()){
                std::string pat=t[p+3].text;p+=5;
                if(!have_input){emit_read_input();have_input=true;}
                if(!compile_input_match(pat,jfs))return false;
                if(cur().type==T_SEMI)p++;
            } else {
                if(!expr())return false;
                TokType op=cur().type;
                if(op!=T_EQUALS&&op!=T_EQEQ&&op!=T_NE&&op!=T_GREATER&&op!=T_LESS&&op!=T_GE&&op!=T_LE)return fail("expected comparison operator in if/elif condition");
                p++;push();if(!expr())return false;mov_rcx_rax();pop();put8(code,0x48);put8(code,0x39);put8(code,0xC8);
                if(cur().type==T_SEMI)p++;
                jfs.push_back(emit_false_jump(op));
            }
            skip_then();
            if(!statement())return false;
            while(skip_example()){}
            if(cur().type==T_ELIF || cur().type==T_ELSE){end_jumps.push_back(jmp(0xE9));for(size_t j:jfs)patch_here(j);branch=cur().type;continue;}
            for(size_t j:jfs)patch_here(j);
            branch=T_EOF;break;
        }
        if(branch==T_ELSE){p++;if(!expect(T_COLON,"':' after else"))return false;if(!statement())return false;}
        for(size_t j:end_jumps)patch_here(j);
        return true;
    }
    bool comparison_operator(TokType op) const { return op==T_EQUALS||op==T_EQEQ||op==T_NE||op==T_GREATER||op==T_LESS||op==T_GE||op==T_LE; }
    bool during_statement(){
        const int base_indent=cur().indent;
        const int loop_line=cur().line;
        p++;
        size_t condition_start=p;
        size_t loop_head=code.size();
        if(!expr())return false;
        TokType op=cur().type;
        if(!comparison_operator(op))return fail("expected comparison operator in during condition");
        p++;
        push();
        if(!expr())return false;
        mov_rcx_rax();pop();
        size_t condition_end=p;
        if(cur().type==T_SEMI)return fail("do not put a semicolon after a during condition");
        put8(code,0x48);put8(code,0x39);put8(code,0xC8);
        size_t exit_jump=emit_false_jump(op);

        // Conservatively detect a simple literal comparison that is always true.
        if(condition_end==condition_start+3 &&
           t[condition_start].type==T_NUMBER && t[condition_start+2].type==T_NUMBER){
            long a=strtol(t[condition_start].text.c_str(),NULL,10), b=strtol(t[condition_start+2].text.c_str(),NULL,10); bool always=false;
            switch(op){case T_EQUALS:case T_EQEQ:always=a==b;break;case T_NE:always=a!=b;break;case T_GREATER:always=a>b;break;case T_LESS:always=a<b;break;case T_GE:always=a>=b;break;case T_LE:always=a<=b;break;default:break;}
            if(always){fprintf(stderr,"%s:%d: error: during condition is always true; possible infinite loop\n",file,loop_line);return false;}
        }

        size_t body_start=p;
        while(cur().type!=T_EOF && cur().indent>base_indent){
            size_t old=p;
            if(!statement())return false;
            if(p==old)return fail("compiler made no progress in during body");
        }
        if(p==body_start)return fail("during requires an indented body");
        size_t back=jmp(0xE9);patch_to(back,loop_head);
        patch_here(exit_jump);
        return true;
    }

    bool parse_constant_decl(){
        p++; if(cur().type!=T_IDENT||cur().text!="var")return fail("expected 'var' after 'constant'");p++;
        if(cur().type!=T_STRING)return fail("expected quoted variable name after 'var'");std::string name=cur().text;p++;
        if(!expect(T_LPAREN,"'(' after constant name")||!expect(T_RPAREN,"')' after constant name")||!expect(T_EQUALS,"'=' after constant declaration"))return false;
        if(!expr())return false; if(cur().type==T_SEMI)p++;
        int idx=declare_var(name,true);if(idx<0)return false;store(idx);return true;
    }
    bool statement(){
        if(cur().type==T_IDENT&&p+2<t.size()&&t[p+1].type==T_EQUALS&&t[p+2].type==T_LBRACE)return parse_table_decl();
        if(skip_example())return true;
        if(cur().type==T_IDENT&&find_table(cur().text)>=0&&p+1<t.size()&&t[p+1].type==T_PLUS){
            std::vector<int> terms;
            while(true){
                if(cur().type!=T_IDENT||find_table(cur().text)<0)return fail("expected a data table name in expression");
                terms.push_back(find_table(cur().text));p++;
                if(cur().type==T_PLUS){p++;continue;}
                break;
            }
            if(cur().type==T_SEMI)p++;
            return emit_eval(terms);
        }
        if(cur().type==T_NUMBER && p+1<t.size() && t[p+1].type==T_COMMA){
            // A comma-separated numeric row in a child body is a sequence of values
            // to feed through the registered Character Translation-style function.
            int row_line=cur().line;
            std::vector<unsigned long> values;
            while(cur().line==row_line && cur().type==T_NUMBER){
                char *end=nullptr;
                unsigned long v=strtoul(cur().text.c_str(),&end,10);
                if(!end || *end!='\0' || v>0xffffffffUL)return fail("invalid value in character sequence");
                values.push_back(v);p++;
                if(cur().type==T_COMMA && cur().line==row_line){p++;continue;}
                break;
            }
            if(values.empty())return fail("expected values in character sequence");
            // /n marks the end of a sequence row; it is not an arithmetic expression here.
            if(cur().type==T_SLASH && cur().line==row_line){p++;if(cur().type==T_IDENT && cur().text=="n" && cur().line==row_line)p++;}
            if(!sequence_mode)return fail("comma-separated character sequences are only valid in child functions");
            int idx=find_var("char");if(idx<0)return fail("character sequence requires 'char' in the primary function");
            auto it=named_functions.find("Character Translation");
            if(it==named_functions.end())return fail("character sequence requires func(\"Character Translation\"); in the primary function");
            for(size_t vi=0;vi<values.size();vi++){
                if(vi>0)write_string(" ");
                put8(code,0xB8);put32(code,(uint32_t)values[vi]);store(idx);
                put8(code,0xE8);named_calls.push_back({code.size(),"Character Translation"});put32(code,0);
            }
            // The /n at the end of this source row ends the output row too.
            write_string("\n");
            return true;
        }
        if(cur().type==T_LBRACE){
            // A braced multiword name followed by ';' invokes a function registered by func("name");.
            size_t close=p+1;bool only_names=true;std::string name;
            while(close<t.size()&&t[close].type!=T_RBRACE){
                if(t[close].type!=T_IDENT){only_names=false;break;}
                if(!name.empty())name+=' ';name+=t[close].text;close++;
            }
            if(only_names&&close<t.size()&&t[close].type==T_RBRACE&&close+1<t.size()&&t[close+1].type==T_SEMI&&!name.empty()){
                p=close+2;
                if(sequence_mode && name=="Character Translation")return true;
                put8(code,0xE8);named_calls.push_back({code.size(),name});put32(code,0);return true;
            }
            p++;while(cur().type!=T_RBRACE){if(cur().type==T_EOF)return fail("missing '}'");if(!statement())return false;}p++;return true;
        }
        if(cur().type==T_FUNC){
            p++;if(!expect(T_LPAREN,"'(' after func")||cur().type!=T_STRING)return fail("func expects a quoted function name");
            std::string name=cur().text;p++;if(!expect(T_RPAREN,"')' after function name")||!expect(T_SEMI,"';' after func declaration"))return false;
            if(named_functions.find(name)!=named_functions.end())return fail("function name is already registered");
            named_functions[name]=code.size();in_named_function=true;return true;
        }
        if(cur().type==T_IF)return if_statement();
        if(cur().type==T_DURING)return during_statement();
        if(cur().type==T_ELSE)return fail("'else' without a matching 'if'");
        if(cur().type==T_ELIF)return fail("'elif' without a matching 'if'");
        if(cur().type==T_EXEC_PARENT){p++;return expect(T_SEMI,"';' after execparentcode");}
        if(cur().type==T_CONSTANT)return parse_constant_decl();
        if(cur().type==T_VAR){
            p++; if(cur().type!=T_IDENT)return fail("expected a variable name after 'var'");
            std::string name=cur().text;p++;if(!expect(T_EQUALS,"'=' after variable name"))return false;
            if(cur().type==T_MALLOC){p++;int idx=declare_var(name,false);if(idx<0)return false;symbols[idx].pointer=true;put8(code,0x31);put8(code,0xC0);store(idx);if(cur().type==T_SEMI)p++;return true;}
            if(!expr())return false;if(cur().type==T_SEMI)p++;int idx=declare_var(name,false);if(idx<0)return false;store(idx);return true;
        }
        if(cur().type==T_SIZE){
            p++;if(!expect(T_OF,"'of' after 'size'")||cur().type!=T_IDENT)return fail("expected a pointer name after 'size of'");
            std::string name=cur().text;int idx=find_var(name);if(idx<0){std::string m="unknown pointer '"+name+"' in size-of allocation";return fail(m.c_str());}if(!symbols[idx].pointer)return fail("'size of' requires a pointer declared with 'malloc'");p++;
            if(cur().type==T_SEMI)p++;
            // Allocate one pointer-sized (8-byte) writable region with Linux mmap.
            put8(code,0xB8);put32(code,9);                    // eax = mmap
            put8(code,0x31);put8(code,0xFF);                  // rdi = NULL
            put8(code,0xBE);put32(code,8);                    // rsi = 8 bytes
            put8(code,0xBA);put32(code,3);                    // rdx = PROT_READ|PROT_WRITE
            put8(code,0x41);put8(code,0xBA);put32(code,0x22); // r10 = MAP_PRIVATE|MAP_ANONYMOUS
            put8(code,0x49);put8(code,0xC7);put8(code,0xC0);put32(code,0xFFFFFFFF); // r8 = -1
            put8(code,0x45);put8(code,0x31);put8(code,0xC9); // r9 = 0
            syscall();
            // Linux reports mmap failure as a negative errno; normalize it to NULL.
            put8(code,0x48);put8(code,0x3D);put32(code,0xFFFFF001); // cmp rax,-4095 (low 32-bit immediate)
            size_t ok=jmp(0x0F,0x82);                           // jb: successful addresses are positive
            put8(code,0x31);put8(code,0xC0);                  // xor eax,eax
            patch_here(ok);
            store(idx);return true;
        }
        if(cur().type==T_ABSOLVE){
            p++;if(cur().type!=T_LBRACE)return fail("expected '{pointer}' after 'absolve'");p++;
            if(cur().type!=T_IDENT)return fail("expected pointer name inside 'absolve' braces");std::string name=cur().text;int idx=find_var(name);if(idx<0){std::string m="unknown pointer '"+name+"' in absolve";return fail(m.c_str());}if(!symbols[idx].pointer)return fail("'absolve' requires a pointer allocated with 'malloc'");p++;if(!expect(T_RBRACE,"'}' after pointer name"))return false;if(cur().type==T_SEMI)p++;
            // munmap(pointer, 8); skip the syscall when the pointer is NULL.
            load(idx);put8(code,0x48);put8(code,0x85);put8(code,0xC0);size_t skip=jmp(0x0F,0x84);
            put8(code,0x48);put8(code,0x89);put8(code,0xC7);put8(code,0xBE);put32(code,8);put8(code,0xB8);put32(code,11);syscall();patch_here(skip);return true;
        }
        if(cur().type==T_MODIFY_ADDRESS){
            p++;if(!expect(T_SEMI,"';' after modify_address"))return false;
            modify_address_var=-1;for(int i=(int)symbols.size()-1;i>=0;i--)if(symbols[i].scope==active_scope&&symbols[i].volatile_u32){modify_address_var=i;break;}
            if(modify_address_var<0)return fail("modify_address requires a preceding volatile_uin_t32 address declaration");return true;
        }
        if(cur().type==T_IDENT&&cur().text=="volatile_uin_t32"){
            p++;if(cur().type!=T_IDENT)return fail("expected a variable name after volatile_uin_t32");std::string name=cur().text;p++;if(!expect(T_EQUALS,"'=' after address variable")||!expect(T_LPAREN,"'(' before address value")||!expr()||!expect(T_RPAREN,"')' after address value"))return false;if(cur().type==T_SEMI)p++;
            int idx=declare_var(name,false);if(idx<0)return false;symbols[idx].volatile_u32=true;store(idx);return true;
        }
        if(cur().type==T_LPAREN&&modify_address_var>=0){
            p++;if(!expr()||!expect(T_RPAREN,"')' after memory value"))return false;if(cur().type==T_SEMI)p++;
            push();load(modify_address_var);mov_rcx_rax();pop();put8(code,0x89);put8(code,0x01);modify_address_var=-1;return true;
        }
        if(cur().type==T_IDENT&&p+1<t.size()&&t[p+1].type==T_EQUALS){
            std::string name=cur().text;int idx=find_var(name);if(idx>=0){p+=2;if(!expect(T_LPAREN,"'(' before assigned value"))return false;bool is_null=(cur().type==T_NULL);if(!expr()||!expect(T_RPAREN,"')' after assigned value"))return false;if(cur().type==T_SEMI)p++;
                // NULL assignment clears the pointer variable. Other assignments through a malloc pointer write its contents.
                if(symbols[idx].pointer&&!is_null){push();load(idx);mov_rcx_rax();pop();put8(code,0x48);put8(code,0x89);put8(code,0x01);}
                else if(symbols[idx].volatile_u32){push();load(idx);mov_rcx_rax();pop();put8(code,0x89);put8(code,0x01);}
                else store(idx);
                return true;
            }
        }
        if(cur().type==T_IDENT&&cur().text=="printf"){
            p++;if(!expect(T_LPAREN,"'(' after printf"))return false;
            if(cur().type!=T_STRING)return fail("printf expects a string literal");std::string s=cur().text;p++;
            if(!expect(T_RPAREN,"')' after printf"))return false;
            if(cur().type==T_SEMI)p++;
            return print_interpolated(s);
        }
        if(cur().type==T_RETURN){
            p++;
            if(cur().type==T_SEMI||cur().type==T_RBRACE||cur().type==T_EOF){put8(code,0x31);put8(code,0xC0);}
            else if(!expr())return false;
            if(cur().type==T_SEMI)p++;
            if(in_named_function){put8(code,0xC3);} // ret from a named func
            else {put8(code,0x48);put8(code,0x89);put8(code,0xC7);put8(code,0xB8);put32(code,60);syscall();}
            return true;
        }
        if(cur().type==T_IDENT&&p+3<t.size()&&t[p+1].type==T_LPAREN&&t[p+2].type==T_RPAREN&&t[p+3].type==T_PLUS_EQ){
            std::string name=cur().text;p+=4;int idx=find_var(name);
            if(idx<0){std::string m="cannot edit undefined variable '"+name+"'";return fail(m.c_str());}
            if(symbols[idx].constant){std::string m="cannot edit constant '"+name+"'";return fail(m.c_str());}
            if(!expr())return false;push();load(idx);mov_rcx_rax();pop();put8(code,0x48);put8(code,0x01);put8(code,0xC8);if(cur().type==T_SEMI)p++;store(idx);return true;
        }
        if(cur().type==T_IDENT&&p+3<t.size()&&t[p+1].type==T_LPAREN&&t[p+2].type==T_RPAREN&&t[p+3].type==T_EQUALS){
            std::string name=cur().text;p+=4;if(!expr())return false;if(cur().type==T_SEMI)p++;
            int existing=find_current_var(name);if(existing>=0)return fail("variable is already declared in this int");
            int idx=declare_var(name,false);if(idx<0)return false;store(idx);return true;
        }
        if(cur().type==T_IDENT&&cur().text.rfind("edit_",0)==0&&p+3<t.size()&&t[p+1].type==T_LPAREN&&t[p+2].type==T_RPAREN&&t[p+3].type==T_COLON){
            std::string name=cur().text.substr(5);p+=4;int idx=find_var(name);
            if(idx<0){std::string m="cannot edit undefined variable '"+name+"'";return fail(m.c_str());}
            if(symbols[idx].constant){std::string m="cannot edit constant '"+name+"'";return fail(m.c_str());}
            if(!expr())return false;if(cur().type==T_SEMI)p++;store(idx);return true;
        }
        if(cur().type==T_IDENT){std::string m="unknown statement or function '"+cur().text+"'";return fail(m.c_str());}
        return fail("unexpected token in int body");
    }
    bool compile_body(const std::vector<Token>&body,int scope){active_scope=scope;t=body;p=0;while(cur().type!=T_EOF){if(!statement())return false;}return true;}
    static std::vector<Token> body_tokens(const std::vector<Token>&all,size_t open,size_t close){std::vector<Token> v;for(size_t i=open+1;i<close;i++)v.push_back(all[i]);v.push_back({T_EOF,"",all[close].line,0});return v;}
    bool program(){
        std::vector<Token> all=t;std::vector<Declaration> decls;size_t i=0;int primary=-1;
        while(i<all.size()&&all[i].type!=T_EOF){
            if(all[i].type!=T_INT)return fail("expected 'int' declaration at top level");int line=all[i].line;i++;
            if(i>=all.size()||all[i].type!=T_IDENT)return fail("expected a name after 'int'");std::string name=all[i++].text;
            if(i>=all.size()||all[i].type!=T_LPAREN)return fail("expected '(' after int name");i++;
            DeclRole role=ROLE_PRIMARY;
            if(i<all.size()&&all[i].type==T_IDENT){std::string r=all[i++].text;if(r=="child")role=ROLE_CHILD;else if(r=="subint")role=ROLE_SUBINT;else if(r=="subchild")role=ROLE_SUBCHILD;else return fail("expected 'child', 'subint', or 'subchild' inside int parentheses");}
            if(i>=all.size()||all[i].type!=T_RPAREN)return fail("expected ')' after int declaration role");i++;
            if(i>=all.size()||all[i].type!=T_EQUALS)return fail("expected '=' after int declaration");i++;
            if(i>=all.size()||all[i].type!=T_LBRACE)return fail("expected '{' to begin int body");size_t open=i++;int depth=1;
            while(i<all.size()&&depth){if(all[i].type==T_LBRACE)depth++;else if(all[i].type==T_RBRACE)depth--;i++;}
            if(depth)return fail("missing '}' in int declaration");size_t close=i-1;
            Declaration d;d.name=name;d.role=role;d.body=body_tokens(all,open,close);d.scope=-1;d.line=line;decls.push_back(d);
            if(role==ROLE_PRIMARY){if(primary>=0)return fail("only one primary int with empty parentheses is allowed");primary=(int)decls.size()-1;}
        }
        if(primary<0)return fail("missing primary int declaration such as 'int main() = { ... }'");
        const std::string primary_name=decls[primary].name;
        {int tdepth=0;for(size_t k=0;k<all.size();k++){
            if(tdepth==0&&k+2<all.size()&&all[k].type==T_IDENT&&all[k+1].type==T_EQUALS&&all[k+2].type==T_LBRACE){tdepth=1;k+=2;continue;}
            if(tdepth>0){if(all[k].type==T_LBRACE)tdepth++;else if(all[k].type==T_RBRACE)tdepth--;continue;}
            if(all[k].type==T_COMMA){sequence_mode=true;break;}
        }}
        for(size_t d=0;d<decls.size();d++){
            if((int)d==primary)continue;
            if(decls[d].role==ROLE_CHILD&&decls[d].name!=primary_name){std::string m="child int name must match primary int '"+primary_name+"'";return fail(m.c_str());}
            if(decls[d].role==ROLE_SUBCHILD){bool found=false;for(const auto &candidate:decls)if(candidate.role==ROLE_SUBINT&&candidate.name==decls[d].name)found=true;if(!found){std::string m="subchild '"+decls[d].name+"' needs a matching subint";return fail(m.c_str());}}
        }
        // Assign lexical scopes. Children and subints can see primary variables;
        // subchildren can also see variables declared by their matching subint.
        scopes.push_back({-1});decls[primary].scope=0;
        for(size_t d=0;d<decls.size();d++)if((int)d!=primary&&decls[d].role!=ROLE_SUBCHILD){decls[d].scope=(int)scopes.size();int parent=0;scopes.push_back({parent});}
        for(size_t d=0;d<decls.size();d++)if(decls[d].role==ROLE_SUBCHILD){int parent=-1;for(size_t j=0;j<decls.size();j++)if(decls[j].role==ROLE_SUBINT&&decls[j].name==decls[d].name){parent=decls[j].scope;break;}decls[d].scope=(int)scopes.size();scopes.push_back({parent});}
        // A primary body containing func("name"); is compiled as a callable function.
        // Startup skips that function body and enters the child body, which can call it by name.
        bool primary_has_named_func=false;
        for(size_t k=0;k+1<decls[primary].body.size();k++)if(decls[primary].body[k].type==T_FUNC){primary_has_named_func=true;break;}
        put8(code,0x55);put8(code,0x48);put8(code,0x89);put8(code,0xE5);put8(code,0x48);put8(code,0x81);put8(code,0xEC);frame_patch=code.size();put32(code,0);
        size_t skip_primary=0;
        if(primary_has_named_func)skip_primary=jmp(0xE9);
        if(!compile_body(decls[primary].body,decls[primary].scope))return false;
        if(primary_has_named_func){put8(code,0xC3);patch_here(skip_primary);in_named_function=false;}
        for(size_t d=0;d<decls.size();d++)if(decls[d].role==ROLE_CHILD)if(!compile_body(decls[d].body,decls[d].scope))return false;
        for(size_t d=0;d<decls.size();d++)if(decls[d].role==ROLE_SUBINT){if(!compile_body(decls[d].body,decls[d].scope))return false;for(size_t j=0;j<decls.size();j++)if(decls[j].role==ROLE_SUBCHILD&&decls[j].name==decls[d].name)if(!compile_body(decls[j].body,decls[j].scope))return false;}
        exit_code(0);
        size_t helper=code.size();
        // Print signed RAX as decimal using a 32-byte stack buffer, then return.
        const uint8_t helper_code[]={
            0x53,                         // push rbx
            0x41,0x54,                    // push r12
            0x41,0x55,                    // push r13
            0x48,0x83,0xEC,0x20,          // sub rsp,32
            0x49,0x89,0xC4,                // mov r12,rax
            0x45,0x31,0xED,                // xor r13d,r13d
            0x4D,0x85,0xE4,                // test r12,r12
            0x79,0x09,                     // jns positive (9 bytes ahead)
            0x41,0xBD,0x01,0x00,0x00,0x00,// mov r13d,1
            0x49,0xF7,0xDC,                // neg r12
            0x48,0x8D,0x74,0x24,0x1F,     // lea rsi,[rsp+31]
            0xBB,0x0A,0x00,0x00,0x00,     // mov ebx,10
            0x45,0x31,0xC0,                // xor r8d,r8d
            0x4C,0x89,0xE0,                // loop: mov rax,r12
            0x31,0xD2,                     // xor edx,edx
            0x48,0xF7,0xF3,                // div rbx
            0x80,0xC2,0x30,                // add dl,'0'
            0x88,0x16,                     // mov [rsi],dl
            0x48,0xFF,0xCE,                // dec rsi
            0x49,0xFF,0xC0,                // inc r8
            0x49,0x89,0xC4,                // mov r12,rax
            0x4D,0x85,0xE4,                // test r12,r12
            0x75,0xE5,                     // jnz loop
            0x45,0x85,0xED,                // test r13d,r13d
            0x74,0x09,                     // jz no_sign
            0xC6,0x06,0x2D,                // mov byte [rsi],'-'
            0x48,0xFF,0xCE,                // dec rsi
            0x49,0xFF,0xC0,                // inc r8
            0x48,0xFF,0xC6,                // no_sign: inc rsi
            0xB8,0x01,0x00,0x00,0x00,     // mov eax,1
            0xBF,0x01,0x00,0x00,0x00,     // mov edi,1
            0x4C,0x89,0xC2,                // mov rdx,r8
            0x0F,0x05,                     // syscall
            0x48,0x83,0xC4,0x20,          // add rsp,32
            0x41,0x5D,                     // pop r13
            0x41,0x5C,                     // pop r12
            0x5B,0xC3                      // pop rbx; ret
        };
        code.insert(code.end(),helper_code,helper_code+sizeof(helper_code));
        for(const CallPatch &cp:int_calls){int32_t rel=(int32_t)(helper-(cp.at+4));for(int k=0;k<4;k++)code[cp.at+k]=(uint8_t)((uint32_t)rel>>(8*k));}
        auto emit_helper=[&](const uint8_t*b,size_t n,const std::vector<CallPatch>&calls){
            if(calls.empty())return;
            size_t at=code.size();code.insert(code.end(),b,b+n);
            for(const CallPatch &cp:calls){int32_t rel=(int32_t)(at-(cp.at+4));for(int k=0;k<4;k++)code[cp.at+k]=(uint8_t)((uint32_t)rel>>(8*k));}
        };
        emit_helper(readline_code,sizeof(readline_code),rl_calls);
        emit_helper(match_lit_code,sizeof(match_lit_code),ml_calls);
        emit_helper(match_set_code,sizeof(match_set_code),ms_calls);
        for(const NamedCallPatch &cp:named_calls){auto it=named_functions.find(cp.name);if(it==named_functions.end()){std::string m="call to unregistered function '"+cp.name+"'";return fail(m.c_str());}int32_t rel=(int32_t)(it->second-(cp.at+4));for(int k=0;k<4;k++)code[cp.at+k]=(uint8_t)((uint32_t)rel>>(8*k));}
        uint32_t frame=(uint32_t)(((symbols.size()*8+15)/16)*16);for(int k=0;k<4;k++)code[frame_patch+k]=(uint8_t)(frame>>(8*k));
        return true;
    }
    Bytes build_elf(){size_t input_off=0;if(uses_input){input_off=data.size();data.resize(data.size()+INPUT_MAX,0);}uint64_t code_addr=BASE+HDR,data_addr=code_addr+code.size();for(const Patch&pt:patches){size_t doff=(pt.data_off==INPUT_BUF)?input_off:pt.data_off;int32_t d=(int32_t)((data_addr+doff)-(code_addr+pt.at+4));for(int i=0;i<4;i++)code[pt.at+i]=(uint8_t)((uint32_t)d>>(8*i));}uint64_t total=HDR+code.size()+data.size();Bytes e;const uint8_t ident[16]={0x7F,'E','L','F',2,1,1,0,0,0,0,0,0,0,0,0};e.insert(e.end(),ident,ident+16);put16(e,2);put16(e,0x3E);put32(e,1);put64(e,code_addr);put64(e,64);put64(e,0);put32(e,0);put16(e,64);put16(e,56);put16(e,1);put16(e,64);put16(e,0);put16(e,0);put32(e,1);put32(e,uses_input?7:5);put64(e,0);put64(e,BASE);put64(e,BASE);put64(e,total);put64(e,total);put64(e,0x1000);e.insert(e.end(),code.begin(),code.end());e.insert(e.end(),data.begin(),data.end());return e;}
};

int main(int argc,char**argv){if(argc==2){fprintf(stderr,"2BC: quote the output separator: ./2BC source.2b '>>' output\n");return 1;}if(argc!=4||strcmp(argv[2],">>")){fprintf(stderr,"usage: ./2BC <source.2b> '>>' <output>\n");return 1;}const char*src_path=argv[1],*out_path=argv[3];size_t len=strlen(src_path);if(len<3||strcmp(src_path+len-3,".2b")){fprintf(stderr,"2BC: source file must have a .2b extension\n");return 1;}std::string src;if(!read_file(src_path,src)){fprintf(stderr,"2BC: cannot open '%s'\n",src_path);return 1;}LexResult lr=lex(src,src_path);if(lr.errors){fprintf(stderr,"%d error(s); not compiling.\n",lr.errors);return 1;}Compiler c(lr.tokens,src_path);if(!c.program())return 1;Bytes elf=c.build_elf();FILE*f=fopen(out_path,"wb");if(!f){fprintf(stderr,"2BC: cannot write '%s'\n",out_path);return 1;}size_t wrote=fwrite(elf.data(),1,elf.size(),f);fclose(f);if(wrote!=elf.size()){fprintf(stderr,"2BC: write failed\n");return 1;}chmod(out_path,0755);printf("2BC: %s -> %s (%zu bytes)\n",src_path,out_path,elf.size());return 0;}