// 2BC -- BYTEIX-BASIC compiler (Linux x86-64), emitting a small static ELF.
// Usage: ./2BC program.2b '>>' program
#include "2b-lexer.cpp"
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>

static const uint64_t BASE = 0x400000;
static const size_t HDR = 64 + 56;
typedef std::vector<uint8_t> Bytes;
static void put8(Bytes &b, uint8_t v) { b.push_back(v); }
static void put16(Bytes &b, uint16_t v) { for (int i=0;i<2;i++) b.push_back((v>>(8*i))&255); }
static void put32(Bytes &b, uint32_t v) { for (int i=0;i<4;i++) b.push_back((v>>(8*i))&255); }
static void put64(Bytes &b, uint64_t v) { for (int i=0;i<8;i++) b.push_back((v>>(8*i))&255); }
struct Patch { size_t at, data_off; };
struct CallPatch { size_t at; };
struct Symbol { std::string name; int scope; bool constant; };
struct Scope { int parent; };
enum DeclRole { ROLE_PRIMARY, ROLE_CHILD, ROLE_SUBINT, ROLE_SUBCHILD };
struct Declaration { std::string name; DeclRole role; std::vector<Token> body; int scope; int line; };

struct Compiler {
    std::vector<Token> t; const char *file; size_t p=0;
    Bytes code, data; std::vector<Patch> patches; std::vector<CallPatch> int_calls;
    std::vector<Symbol> symbols; std::vector<Scope> scopes; int active_scope=0;
    size_t frame_patch=0;
    Compiler(const std::vector<Token>& toks,const char*f):t(toks),file(f){}
    const Token &cur() const { return t[p < t.size() ? p : t.size()-1]; }
    bool fail(const char *msg) { fprintf(stderr,"%s:%d: error: %s\n",file,cur().line,msg); return false; }
    bool expect(TokType ty,const char *what) { if(cur().type!=ty){std::string m=std::string("expected ")+what;return fail(m.c_str());} p++;return true; }
    void syscall(){put8(code,0x0F);put8(code,0x05);}
    void exit_code(uint32_t s){put8(code,0xBF);put32(code,s);put8(code,0xB8);put32(code,60);syscall();}
    void write_string(const std::string&s){
        if(s.empty())return;
        size_t off=data.size();data.insert(data.end(),s.begin(),s.end());
        put8(code,0xB8);put32(code,1); put8(code,0xBF);put32(code,1);
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
        symbols.push_back({n,active_scope,constant});return (int)symbols.size()-1;
    }
    static int32_t disp(int i){return -8*(i+1);}
    void load(int i){put8(code,0x48);put8(code,0x8B);put8(code,0x85);put32(code,(uint32_t)disp(i));}
    void store(int i){put8(code,0x48);put8(code,0x89);put8(code,0x85);put32(code,(uint32_t)disp(i));}
    void push(){put8(code,0x50);} void pop(){put8(code,0x58);} void mov_rcx_rax(){put8(code,0x48);put8(code,0x89);put8(code,0xC1);}
    size_t jmp(uint8_t op1,uint8_t op2=0){if(op2){put8(code,op1);put8(code,op2);}else put8(code,op1);size_t at=code.size();put32(code,0);return at;}
    void patch_to(size_t at,size_t target){int32_t rel=(int32_t)(target-(at+4));for(int i=0;i<4;i++)code[at+i]=(uint8_t)((uint32_t)rel>>(8*i));}
    void patch_here(size_t at){patch_to(at,code.size());}
    void call_print_int(){put8(code,0xE8);int_calls.push_back({code.size()});put32(code,0);}
    bool factor(){
        if(cur().type==T_NUMBER){unsigned long v=strtoul(cur().text.c_str(),NULL,10);if(v>0x7fffffffUL)return fail("number too large (max 2147483647)");put8(code,0xB8);put32(code,(uint32_t)v);p++;return true;}
        if(cur().type==T_IDENT){std::string n=cur().text;int i=find_var(n);if(i<0){std::string m="undefined variable '"+n+"'";return fail(m.c_str());}p++;if(!expect(T_LPAREN,"'(' after variable name")||!expect(T_RPAREN,"')' after variable name"))return false;load(i);return true;}
        if(cur().type==T_LPAREN){p++;if(!expr())return false;return expect(T_RPAREN,"')'");}
        if(cur().type==T_MINUS){p++;if(!factor())return false;put8(code,0x48);put8(code,0xF7);put8(code,0xD8);return true;}
        return fail("expected a number, variable, or parenthesized expression");
    }
    bool term(){if(!factor())return false;while(cur().type==T_STAR||cur().type==T_SLASH){TokType op=cur().type;p++;push();if(!factor())return false;mov_rcx_rax();pop();if(op==T_STAR){put8(code,0x48);put8(code,0x0F);put8(code,0xAF);put8(code,0xC1);}else{put8(code,0x48);put8(code,0x99);put8(code,0x48);put8(code,0xF7);put8(code,0xF9);}}return true;}
    bool expr(){if(!term())return false;while(cur().type==T_PLUS||cur().type==T_MINUS){TokType op=cur().type;p++;push();if(!term())return false;mov_rcx_rax();pop();put8(code,0x48);if(op==T_PLUS){put8(code,0x01);put8(code,0xC8);}else{put8(code,0x29);put8(code,0xC8);}}return true;}
    size_t emit_false_jump(TokType op){switch(op){case T_EQUALS:case T_EQEQ:return jmp(0x0F,0x85);case T_NE:return jmp(0x0F,0x84);case T_GREATER:return jmp(0x0F,0x8E);case T_LESS:return jmp(0x0F,0x8D);case T_GE:return jmp(0x0F,0x8C);case T_LE:return jmp(0x0F,0x8F);default:return jmp(0x0F,0x85);}}
    bool print_interpolated(const std::string&s){
        size_t pos=0;
        while(pos<s.size()){
            size_t open=s.find('{',pos);
            if(open==std::string::npos){write_string(s.substr(pos));break;}
            size_t close=s.find('}',open+1);
            if(close==std::string::npos){write_string(s.substr(pos));break;}
            write_string(s.substr(pos,open-pos));
            std::string name=s.substr(open+1,close-open-1);
            int idx=find_var(name);
            if(idx<0){std::string m="unknown variable '"+name+"' in string interpolation";return fail(m.c_str());}
            load(idx);call_print_int();pos=close+1;
        }
        return true;
    }
    bool if_statement(){p++;if(!expr())return false;TokType op=cur().type;if(op!=T_EQUALS&&op!=T_EQEQ&&op!=T_NE&&op!=T_GREATER&&op!=T_LESS&&op!=T_GE&&op!=T_LE)return fail("expected comparison operator in if condition");p++;push();if(!expr())return false;mov_rcx_rax();pop();put8(code,0x48);put8(code,0x39);put8(code,0xC8);if(!expect(T_SEMI,"';' after if condition"))return false;size_t jf=emit_false_jump(op);if(!statement())return false;if(cur().type==T_ELSE){p++;if(!expect(T_COLON,"':' after else"))return false;size_t je=jmp(0xE9);patch_here(jf);if(!statement())return false;patch_here(je);}else patch_here(jf);return true;}
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
        if(cur().type==T_LBRACE){p++;while(cur().type!=T_RBRACE){if(cur().type==T_EOF)return fail("missing '}'");if(!statement())return false;}p++;return true;}
        if(cur().type==T_IF)return if_statement();
        if(cur().type==T_DURING)return during_statement();
        if(cur().type==T_ELSE)return fail("'else' without a matching 'if'");
        if(cur().type==T_EXEC_PARENT){p++;return expect(T_SEMI,"';' after execparentcode");}
        if(cur().type==T_CONSTANT)return parse_constant_decl();
        if(cur().type==T_IDENT&&cur().text=="printf"){
            p++;if(!expect(T_LPAREN,"'(' after printf"))return false;
            if(cur().type!=T_STRING)return fail("printf expects a string literal");std::string s=cur().text;p++;
            if(!expect(T_RPAREN,"')' after printf")||!expect(T_SEMI,"';' after printf"))return false;
            return print_interpolated(s);
        }
        if(cur().type==T_RETURN){p++;if(cur().type!=T_SEMI&&!expr())return false;return expect(T_SEMI,"';' after return");}
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
        // Emit in the language's execution order: primary, its children, then each subint and its subchildren.
        put8(code,0x55);put8(code,0x48);put8(code,0x89);put8(code,0xE5);put8(code,0x48);put8(code,0x81);put8(code,0xEC);frame_patch=code.size();put32(code,0);
        if(!compile_body(decls[primary].body,decls[primary].scope))return false;
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
        uint32_t frame=(uint32_t)(((symbols.size()*8+15)/16)*16);for(int k=0;k<4;k++)code[frame_patch+k]=(uint8_t)(frame>>(8*k));
        return true;
    }
    Bytes build_elf(){uint64_t code_addr=BASE+HDR,data_addr=code_addr+code.size();for(const Patch&pt:patches){int32_t d=(int32_t)((data_addr+pt.data_off)-(code_addr+pt.at+4));for(int i=0;i<4;i++)code[pt.at+i]=(uint8_t)((uint32_t)d>>(8*i));}uint64_t total=HDR+code.size()+data.size();Bytes e;const uint8_t ident[16]={0x7F,'E','L','F',2,1,1,0,0,0,0,0,0,0,0,0};e.insert(e.end(),ident,ident+16);put16(e,2);put16(e,0x3E);put32(e,1);put64(e,code_addr);put64(e,64);put64(e,0);put32(e,0);put16(e,64);put16(e,56);put16(e,1);put16(e,64);put16(e,0);put16(e,0);put32(e,1);put32(e,5);put64(e,0);put64(e,BASE);put64(e,BASE);put64(e,total);put64(e,total);put64(e,0x1000);e.insert(e.end(),code.begin(),code.end());e.insert(e.end(),data.begin(),data.end());return e;}
};

int main(int argc,char**argv){if(argc==2){fprintf(stderr,"2BC: quote the output separator: ./2BC source.2b '>>' output\n");return 1;}if(argc!=4||strcmp(argv[2],">>")){fprintf(stderr,"usage: ./2BC <source.2b> '>>' <output>\n");return 1;}const char*src_path=argv[1],*out_path=argv[3];size_t len=strlen(src_path);if(len<3||strcmp(src_path+len-3,".2b")){fprintf(stderr,"2BC: source file must have a .2b extension\n");return 1;}std::string src;if(!read_file(src_path,src)){fprintf(stderr,"2BC: cannot open '%s'\n",src_path);return 1;}LexResult lr=lex(src,src_path);if(lr.errors){fprintf(stderr,"%d error(s); not compiling.\n",lr.errors);return 1;}Compiler c(lr.tokens,src_path);if(!c.program())return 1;Bytes elf=c.build_elf();FILE*f=fopen(out_path,"wb");if(!f){fprintf(stderr,"2BC: cannot write '%s'\n",out_path);return 1;}size_t wrote=fwrite(elf.data(),1,elf.size(),f);fclose(f);if(wrote!=elf.size()){fprintf(stderr,"2BC: write failed\n");return 1;}chmod(out_path,0755);printf("2BC: %s -> %s (%zu bytes)\n",src_path,out_path,elf.size());return 0;}
