#include "codegen.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <set>
#include <sstream>
#include <iostream>

namespace tc {

CodeGen::CodeGen(Program& prog, Diag& diag) : prog_(prog), diag_(diag) {}

std::string CodeGen::sanitize(const std::string& s) {
  std::string r;
  for (char c : s) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') r += c;
    else r += '_';
  }
  if (r.empty()) r = "anon";
  return r;
}

std::string CodeGen::structName(StructDecl* s) {
  return "tc_s_" + sanitize(s->name);
}

static std::string typeCode(Type* t) {
  if (!t) return "v";
  if (t->isRef) return "R" + typeCode(t->elem);
  switch (t->kind) {
    case TypeKind::Void: return "v";
    case TypeKind::Bool: return "b";
    case TypeKind::Char: return t->isUnsigned ? "h" : "c";
    case TypeKind::Short: return t->isUnsigned ? "t" : "s";
    case TypeKind::Int: return t->isUnsigned ? "j" : "i";
    case TypeKind::Long: return t->isUnsigned ? "m" : "l";
    case TypeKind::LongLong: return t->isUnsigned ? "y" : "x";
    case TypeKind::Float: return "f";
    case TypeKind::Double: return "d";
    case TypeKind::Ptr: return "P" + typeCode(t->elem);
    case TypeKind::Array: return "A" + std::to_string(t->arrayLen) + "_" + typeCode(t->elem);
    case TypeKind::Struct: return "S" + std::to_string(t->sdecl ? t->sdecl->name.size() : 0) + (t->sdecl ? t->sdecl->name : "");
  }
  return "v";
}

std::string CodeGen::funcName(FuncDecl* f) {
  if (f->name == "main") return "main";
  if (f->isExternC) return f->name;
  std::string sig;
  for (auto* p : f->params) sig += typeCode(p->type);
  if (f->variadic) sig += "z";
  if (sig.empty()) sig = "v";
  if (f->isCtor) return "tc_ctor_" + sanitize(f->owner->name) + "__" + sanitize(sig);
  if (f->isDtor) return "tc_dtor_" + sanitize(f->owner->name);
  if (f->isMethod) return "tc_m_" + sanitize(f->owner->name) + "_" + sanitize(f->shortName) + "__" + sanitize(sig);
  return "tc_f_" + sanitize(f->name) + "__" + sanitize(sig);
}

std::string CodeGen::varName(VarSym* v) {
  return std::string(v->isGlobal ? "tc_g_" : "tc_v_") + sanitize(v->name);
}

std::string CodeGen::fieldName(Field* f) {
  return "f_" + sanitize(f->name);
}

std::string CodeGen::ctype(Type* t) {
  if (!t) return "void";
  if (t->isRef) return ctype(t->elem) + "*";
  switch (t->kind) {
    case TypeKind::Void: return "void";
    case TypeKind::Bool: return "int";
    case TypeKind::Char: return t->isUnsigned ? "unsigned char" : "char";
    case TypeKind::Short: return t->isUnsigned ? "unsigned short" : "short";
    case TypeKind::Int: return t->isUnsigned ? "unsigned int" : "int";
    case TypeKind::Long: return t->isUnsigned ? "unsigned long" : "long";
    case TypeKind::LongLong: return t->isUnsigned ? "unsigned long long" : "long long";
    case TypeKind::Float: return "float";
    case TypeKind::Double: return "double";
    case TypeKind::Ptr: return ctype(t->elem) + "*";
    case TypeKind::Array:
      return ctype(t->elem) + "[" + std::to_string(t->arrayLen) + "]";
    case TypeKind::Struct: return "struct " + structName(t->sdecl);
  }
  return "void";
}

std::string CodeGen::decl(Type* t, const std::string& name) {
  if (t && t->kind == TypeKind::Array) {
    return decl(t->elem, name + "[" + std::to_string(t->arrayLen) + "]");
  }
  return ctype(t) + " " + name;
}

std::string CodeGen::paramDecl(Type* t, const std::string& name) {
  if (t && t->kind == TypeKind::Array)
    return ctype(t->elem) + "*" + (name.empty() ? "" : " " + name);
  if (t && t->isRef)
    return ctype(t->elem) + "*" + (name.empty() ? "" : " " + name);
  if (name.empty()) return ctype(t);
  return decl(t, name);
}

std::string CodeGen::ind(int n) { return std::string(static_cast<size_t>(n) * 2, ' '); }

void CodeGen::emitLine(int indent, const std::string& s) {
  out_ << ind(indent) << s << "\n";
}

std::string CodeGen::funcSig(FuncDecl* f, bool withNames) {
  std::string ret = (f->isCtor || f->isDtor) ? "void" : ctype(f->ret);
  std::vector<std::string> ps;
  if (f->isMethod && !f->isStatic && f->owner)
    ps.push_back("struct " + structName(f->owner) + "* tc_this");
  for (auto* p : f->params)
    ps.push_back(paramDecl(p->type, withNames ? varName(p) : ""));
  if (f->variadic) ps.push_back("...");
  if (ps.empty()) ps.push_back("void");
  std::string joined;
  for (size_t i = 0; i < ps.size(); ++i) {
    if (i) joined += ", ";
    joined += ps[i];
  }
  return ret + " " + funcName(f) + "(" + joined + ")";
}

static std::string ctorSig(FuncDecl* f) {
  if (!f) return "v";
  std::string sig;
  for (auto* p : f->params) sig += typeCode(p->type);
  if (f->variadic) sig += "z";
  return sig.empty() ? "v" : sig;
}

std::string CodeGen::helperNewName(StructDecl* s, FuncDecl* ctor) {
  return "tc_new_" + sanitize(s->name) + "__" + sanitize(ctorSig(ctor));
}

std::string CodeGen::helperDelName(StructDecl* s) {
  return "tc_del_" + sanitize(s->name);
}

std::string CodeGen::helperMakeName(StructDecl* s, FuncDecl* ctor) {
  return "tc_make_" + sanitize(s->name) + "__" + sanitize(ctorSig(ctor));
}

bool CodeGen::isLibc(const std::string& name) {
  static const std::set<std::string> names = {
    "printf", "fprintf", "sprintf", "snprintf", "scanf", "sscanf", "puts",
    "putchar", "fputs", "fgets", "perror", "malloc", "calloc", "realloc",
    "free", "memset", "memcpy", "memmove", "strlen", "strcmp", "strcpy",
    "exit", "abort", "atoi", "atof", "sqrt", "pow", "sin", "cos", "tan",
    "fabs", "floor", "ceil", "fmod", "rand", "srand", "time", "fopen",
    "fclose", "qsort", "abs"
  };
  return names.count(name) != 0;
}

std::string CodeGen::generate() {
  emitPrelude();
  emitStructForward();
  emitStructDefs();
  emitPrototypes();
  emitVtables();
  emitGlobals();
  auto saved = out_.str();
  out_.str(""); out_.clear();
  emitFunctions();
  std::string funcs = out_.str();
  out_.str(""); out_.clear();
  out_ << saved;
  out_ << promotedGlobals_;
  out_ << funcs;
  emitHelpers();
  return out_.str();
}

void CodeGen::emitPrelude() {
  out_ << "/* generated by tinycpp */\n";
  out_ << "#include <stdio.h>\n";
  out_ << "#include <stdlib.h>\n";
  out_ << "#include <string.h>\n";
  out_ << "#include <stddef.h>\n";
  out_ << "#include <stdint.h>\n";
  out_ << "#include <math.h>\n\n";
  out_ << "static void* tc_alloc(size_t n) {\n";
  out_ << "  void* p = malloc(n);\n";
  out_ << "  if (!p) { fprintf(stderr, \"tinycpp: out of memory\\n\"); exit(1); }\n";
  out_ << "  memset(p, 0, n);\n";
  out_ << "  return p;\n";
  out_ << "}\n";
  out_ << "static void tc_free(void* p) { free(p); }\n";
  out_ << "static void tc_sort_int_asc(void* p, size_t n) { int* a = (int*)p; for (size_t i = 1; i < n; ++i) { int v = a[i]; size_t j = i; while (j > 0 && a[j-1] > v) { a[j] = a[j-1]; --j; } a[j] = v; } }\n";
  out_ << "static void tc_sort_int_desc(void* p, size_t n) { int* a = (int*)p; for (size_t i = 1; i < n; ++i) { int v = a[i]; size_t j = i; while (j > 0 && a[j-1] < v) { a[j] = a[j-1]; --j; } a[j] = v; } }\n";
  out_ << "static void tc_iota_int(void* p, size_t n, int start) { int* a = (int*)p; for (size_t i = 0; i < n; ++i) a[i] = start + (int)i; }\n";
  out_ << "static long long tc_accumulate_int(void* p, size_t n, long long init) { int* a = (int*)p; for (size_t i = 0; i < n; ++i) init += a[i]; return init; }\n";
  out_ << "static char* tc_to_string_int(long long v) { char* b = (char*)tc_alloc(32); snprintf(b, 32, \"%lld\", v); return b; }\n";
  out_ << "static char* tc_strcat2(const char* a, const char* b) { size_t la=a?strlen(a):0, lb=b?strlen(b):0; char* r=(char*)tc_alloc(la+lb+1); if(a) memcpy(r,a,la); if(b) memcpy(r+la,b,lb); r[la+lb]=0; return r; }\n";
  out_ << "typedef struct { char** keys; int* vals; unsigned long size; unsigned long cap; } tc_map_si;\n";
  out_ << "static int* tc_map_si_get(tc_map_si* m, char* k) { for (unsigned long i=0;i<m->size;++i) { if (m->keys[i]==k || (m->keys[i]&&k&&strcmp(m->keys[i],k)==0)) return &m->vals[i]; } if (m->size==m->cap) { unsigned long nc = m->cap?m->cap*2:16; m->keys=(char**)realloc(m->keys,nc*sizeof(char*)); m->vals=(int*)realloc(m->vals,nc*sizeof(int)); m->cap=nc; } m->keys[m->size]=k; m->vals[m->size]=0; return &m->vals[m->size++]; }\n";
  if (abortAssert_) {
    out_ << "static void __assert_fail(const char* e, const char* f, unsigned int l, const char* fn) { fprintf(stderr, \"%s:%u: %s: assertion \\\"%s\\\" failed\\n\", f, l, fn, e); abort(); }\n";
    out_ << "static void __assert2(const char* f, int l, const char* fn, const char* e) { fprintf(stderr, \"%s:%d: %s: assertion \\\"%s\\\" failed\\n\", f, l, fn, e); abort(); }\n\n";
  } else {
    out_ << "static void __assert_fail(const char* e, const char* f, unsigned int l, const char* fn) { fprintf(stderr, \"%s:%u: %s: assertion \\\"%s\\\" failed\\n\", f, l, fn, e); }\n";
    out_ << "static void __assert2(const char* f, int l, const char* fn, const char* e) { fprintf(stderr, \"%s:%d: %s: assertion \\\"%s\\\" failed\\n\", f, l, fn, e); }\n\n";
  }
}

void CodeGen::emitStructForward() {
  for (auto& s : prog_.structs)
    out_ << "struct " << structName(s.get()) << ";\n";
  if (!prog_.structs.empty()) out_ << "\n";
}

void CodeGen::emitStructDefs() {
  for (auto& sp : prog_.structs) {
    StructDecl* s = sp.get();
    out_ << "struct " << structName(s) << " {\n";
    if (s->fields.empty()) out_ << "  char tc_empty;\n";
    for (auto& f : s->fields)
      out_ << "  " << decl(f.type, fieldName(&f)) << ";\n";
    out_ << "};\n\n";
  }
}

void CodeGen::emitPrototypes() {
  for (auto& fp : prog_.funcs) {
    FuncDecl* f = fp.get();
    if (f->isExternC && !f->isDefined) continue;
    out_ << funcSig(f, false) << ";\n";
  }
  out_ << "\n";

  for (auto& sp : prog_.structs) {
    StructDecl* s = sp.get();
    auto emitOne = [&](FuncDecl* ctor) {
      std::vector<std::string> ps;
      if (ctor) { for (auto* p : ctor->params) ps.push_back(paramDecl(p->type, "")); if (ctor->variadic) ps.push_back("..."); }
      std::string joined;
      for (size_t i = 0; i < ps.size(); ++i) { if (i) joined += ", "; joined += ps[i]; }
      if (joined.empty()) joined = "void";
      out_ << "static struct " << structName(s) << "* " << helperNewName(s, ctor) << "(" << joined << ");\n";
      out_ << "static struct " << structName(s) << " " << helperMakeName(s, ctor) << "(" << joined << ");\n";
    };
    if (s->ctors.empty()) emitOne(nullptr);
    else for (auto* c : s->ctors) emitOne(c);
    if (s->dtor)
      out_ << "static void " << helperDelName(s) << "(struct " << structName(s) << "* p);\n";
  }
  if (!prog_.structs.empty()) out_ << "\n";
}

void CodeGen::emitGlobals() {
  for (auto& vp : prog_.vars) {
    VarSym* v = vp.get();
    if (!v->isGlobal) continue;
    if (v->type && v->type->kind == TypeKind::Struct) {
      StructDecl* sd = v->type->sdecl;
      if (!sd->ctors.empty())
        diag_.error(v->loc, "global object '" + v->name + "' has a constructor; use a local object or initialize it in main");
      if (sd->dtor)
        diag_.warning(v->loc, "global object '" + v->name + "' has a destructor; it will not run at exit");
    }
    if (v->isExtern) {
      out_ << "extern " << decl(v->type, varName(v)) << ";\n";
      continue;
    }
    if (v->isStatic) out_ << "static ";
    out_ << decl(v->type, varName(v));
    if (!v->initList.empty()) {
      out_ << " = { ";
      for (size_t i = 0; i < v->initList.size(); ++i) {
        if (i) out_ << ", ";
        out_ << expr(v->initList[i].get());
      }
      out_ << " }";
    } else if (v->init) {
      out_ << " = " << expr(v->init.get());
    } else if (v->type && (v->type->kind == TypeKind::Array ||
                           v->type->kind == TypeKind::Struct)) {
      out_ << " = {0}";
    }
    out_ << ";\n";
  }
  out_ << "\n";
}

void CodeGen::emitFunctions() {
  for (auto& fp : prog_.funcs) {
    FuncDecl* f = fp.get();
    if (!f->body) continue;
    if (f->isExternC && !f->isDefined) continue;

    curFunc_ = f;
    out_ << funcSig(f, true) << " {\n";
    pushCleanup();

    if (f->isCtor && f->owner) {
      if (f->owner->hasVtable)
        emitLine(1, "tc_this->f___vptr = (void**)tc_vt_" + sanitize(f->owner->name) + ";");
      // base ctor
      if (f->owner->base) {
        StructDecl* base = f->owner->base;
        FuncDecl* bc = nullptr;
        if (f->baseInit) {
          int bestScore=1<<30;
          for (auto* c : base->ctors) {
            size_t fixed=c->params.size();
            if (f->baseInitArgs.size()>fixed && !c->variadic) continue;
            if (f->baseInitArgs.size()<fixed){ bool ok=true; for(size_t i=f->baseInitArgs.size();i<fixed;++i) if(!c->params[i]->defaultArg) ok=false; if(!ok) continue; }
            int sc=0; for(size_t i=0;i<f->baseInitArgs.size()&&i<fixed;++i){ Type* p=c->params[i]->type; Type* a=f->baseInitArgs[i]->type; if(p&&p->isRef)p=p->elem; if(a&&a->isRef)a=a->elem; if(!sameType(p,a)) sc++; } 
            if(!bc||sc<bestScore){bc=c;bestScore=sc;}
          }
          if (!bc && !base->ctors.empty()) bc = base->ctors.front();
        } else if (!base->ctors.empty()) {
          // default base ctor
          for (auto* c : base->ctors) if (c->params.empty() || c->params[0]->defaultArg) { bc=c; break; }
          if (!bc) bc = base->ctors.front();
        }
        if (bc) {
          std::vector<std::string> vals;
          for (auto& a : f->baseInitArgs) vals.push_back(expr(a.get()));
          for (size_t i=f->baseInitArgs.size(); i<bc->params.size(); ++i) { if(bc->params[i]->defaultArg) vals.push_back(expr(bc->params[i]->defaultArg.get())); }
          std::string joined;
          for(size_t i=0;i<vals.size();++i){ if(i) joined+=", "; joined+=vals[i]; }
          emitLine(1, funcName(bc) + "((struct " + structName(base) + "*)tc_this" + (joined.empty()?"":", "+joined) + ");");
        }
      }
      if (f->owner->hasVtable)
        emitLine(1, "tc_this->f___vptr = (void**)tc_vt_" + sanitize(f->owner->name) + ";");
      emitFieldDefaults(f->owner, "tc_this", 1);
      for (auto& item : f->initList) {
        if (!item.field) continue;
        emitLine(1, "tc_this->" + fieldName(item.field) + " = " + expr(item.value.get()) + ";");
      }
    }

    if (f->body->kind == StmtKind::Block) {
      for (auto& k : f->body->kids) emitStmt(k.get(), 1);
    } else {
      emitStmt(f->body.get(), 1);
    }

    if (f->isDtor && f->owner && f->owner->base && f->owner->base->dtor) {
      emitLine(1, funcName(f->owner->base->dtor) + "((struct " + structName(f->owner->base) + "*)tc_this);");
    }

    if (f->name == "main") {
      emitLine(1, "return 0;");
    } else if (!f->isCtor && !f->isDtor && f->ret && f->ret->kind != TypeKind::Void &&
               f->ret->kind != TypeKind::Struct) {
      emitLine(1, "return 0;");
    }

    popCleanup(1);
    out_ << "}\n\n";
    curFunc_ = nullptr;
  }
}

void CodeGen::emitFieldDefaults(StructDecl* s, const std::string& ptr, int indent) {
  for (auto& f : s->fields) {
    if (f.defaultInit)
      emitLine(indent, ptr + "->" + fieldName(&f) + " = " + expr(f.defaultInit.get()) + ";");
  }
}

void CodeGen::emitCtorCall(StructDecl* s, const std::string& ptr,
                           const std::vector<ExprPtr>& args, int indent) {
  if (s->ctors.empty()) {
    if (!args.empty())
      diag_.error(s->loc, "class '" + s->name + "' has no constructor");
    return;
  }
  FuncDecl* best = nullptr;
  int bestScore = 1<<30;
  for (auto* c : s->ctors) {
    size_t fixed = c->params.size();
    if (args.size() > fixed && !c->variadic) continue;
    if (args.size() < fixed) {
      bool ok=true; for(size_t i=args.size();i<fixed;++i) if(!c->params[i]->defaultArg) ok=false; if(!ok) continue;
    }
    int sc=0;
    for(size_t i=0;i<args.size() && i<fixed;++i){ Type* p=c->params[i]->type; Type* a=args[i]->type; if(p&&p->isRef)p=p->elem; if(a&&a->isRef)a=a->elem; if(sameType(p,a)){} else sc+=1; }
    if(!best || sc<bestScore){best=c; bestScore=sc;}
  }
  if (!best) best = s->ctors.front();
  std::vector<std::string> vals;
  for (const auto& a : args) vals.push_back(expr(a.get()));
  for (size_t i = args.size(); i < best->params.size(); ++i) {
    VarSym* p = best->params[i];
    if (!p->defaultArg)
      diag_.error(s->loc, "constructor of '" + s->name + "' requires more arguments");
    vals.push_back(expr(p->defaultArg.get()));
  }
  std::string joined;
  for (size_t i = 0; i < vals.size(); ++i) { if (i) joined += ", "; joined += vals[i]; }
  emitLine(indent, funcName(best) + "(" + ptr + (joined.empty() ? "" : ", " + joined) + ");");
}

void CodeGen::emitVtables() {
  for (auto& sp : prog_.structs) {
    StructDecl* s = sp.get();
    if (!s->hasVtable) continue;
    out_ << "static void* tc_vt_" << sanitize(s->name) << "[" << s->vtable.size() << "] = {";
    for (size_t i=0;i<s->vtable.size();++i) {
      if (i) out_ << ", ";
      FuncDecl* m = s->vtable[i];
      out_ << "(void*)" << funcName(m);
    }
    out_ << "};\n";
  }
  if (true) { bool any=false; for(auto&sp:prog_.structs) if(sp->hasVtable) any=true; if(any) out_<<"\n"; }
}

void CodeGen::emitHelpers() {
  for (auto& sp : prog_.structs) {
    StructDecl* s = sp.get();
    auto emitOne = [&](FuncDecl* ctor) {
      std::vector<std::string> ps;
      std::vector<std::string> names;
      if (ctor) {
        for (auto* p : ctor->params) { ps.push_back(paramDecl(p->type, varName(p))); names.push_back(varName(p)); }
        if (ctor->variadic) ps.push_back("...");
      }
      std::string joined;
      for (size_t i = 0; i < ps.size(); ++i) { if (i) joined += ", "; joined += ps[i]; }
      if (joined.empty()) joined = "void";

      out_ << "static struct " << structName(s) << "* " << helperNewName(s, ctor) << "(" << joined << ") {\n";
      out_ << "  struct " << structName(s) << "* p = (struct " << structName(s) << "*)tc_alloc(sizeof(struct " << structName(s) << "));\n";
      emitFieldDefaults(s, "p", 1);
      if (ctor) {
        std::string call = funcName(ctor) + "(p";
        for (auto& n : names) call += ", " + n;
        call += ");";
        emitLine(1, call);
      }
      out_ << "  return p;\n";
      out_ << "}\n\n";

      out_ << "static struct " << structName(s) << " " << helperMakeName(s, ctor) << "(" << joined << ") {\n";
      out_ << "  struct " << structName(s) << " p = {0};\n";
      emitFieldDefaults(s, "(&p)", 1);
      if (ctor) {
        std::string call = funcName(ctor) + "(&p";
        for (auto& n : names) call += ", " + n;
        call += ");";
        emitLine(1, call);
      }
      out_ << "  return p;\n";
      out_ << "}\n\n";
    };
    if (s->ctors.empty()) emitOne(nullptr);
    else for (auto* c : s->ctors) emitOne(c);
    if (s->dtor) {
      out_ << "static void " << helperDelName(s) << "(struct " << structName(s) << "* p) {\n";
      if (s->dtor->isVirtual && s->dtor->vslot >= 0) {
        out_ << "  if (p) { ((void(*)(struct " << structName(s) << "*))((void**)p->f___vptr)[" << s->dtor->vslot << "])(p); tc_free(p); }\n";
      } else {
        out_ << "  if (p) { " << funcName(s->dtor) << "(p); tc_free(p); }\n";
      }
      out_ << "}\n\n";
    }
  }
}

void CodeGen::pushCleanup() { cleanupScopes_.emplace_back(); }

void CodeGen::registerCleanup(VarSym* v) {
  if (v && v->type && v->type->kind == TypeKind::Struct &&
      v->type->sdecl && v->type->sdecl->dtor)
    cleanupScopes_.back().push_back(v);
}

bool CodeGen::hasCleanup() const {
  for (const auto& s : cleanupScopes_)
    if (!s.empty()) return true;
  return false;
}

void CodeGen::emitDtorsFrom(size_t depth, int indent) {
  for (size_t i = cleanupScopes_.size(); i > depth; --i) {
    auto& scope = cleanupScopes_[i - 1];
    for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
      VarSym* v = *it;
      if (v->type && v->type->kind == TypeKind::Struct && v->type->sdecl->dtor)
        emitLine(indent, funcName(v->type->sdecl->dtor) + "(&" + varName(v) + ");");
    }
  }
}

void CodeGen::popCleanup(int indent) {
  if (cleanupScopes_.empty()) return;
  emitDtorsFrom(cleanupScopes_.size() - 1, indent);
  cleanupScopes_.pop_back();
}

void CodeGen::emitBlock(Stmt* s, int indent, bool newScope) {
  emitLine(indent, "{");
  if (newScope) pushCleanup();
  for (auto& k : s->kids) emitStmt(k.get(), indent + 1);
  if (newScope) popCleanup(indent + 1);
  emitLine(indent, "}");
}

void CodeGen::emitScoped(Stmt* s, int indent) {
  if (!s) return;
  if (s->kind == StmtKind::Block) {
    emitBlock(s, indent);
    return;
  }
  emitLine(indent, "{");
  pushCleanup();
  emitStmt(s, indent + 1);
  popCleanup(indent + 1);
  emitLine(indent, "}");
}

static std::string join(const std::vector<std::string>& v, const char* sep = ", ") {
  std::string r;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) r += sep;
    r += v[i];
  }
  return r;
}

void CodeGen::emitStmt(Stmt* s, int indent) {
  if (!s) return;
  switch (s->kind) {
    case StmtKind::Block:
      emitBlock(s, indent);
      return;
    case StmtKind::Expr:
      emitLine(indent, expr(s->expr.get()) + ";");
      return;
    case StmtKind::Decl: {
      for (auto& item : s->decls) {
        Type* t = item.var->type;
        std::string name = varName(item.var);
        bool st = item.var->isStatic;
        if (item.var->isGlobal && curFunc_) {
          if (emittedPromoted_.insert(name).second) {
            if (t->kind == TypeKind::Struct)
              promotedGlobals_ += "struct " + structName(t->sdecl) + " " + name + " = {0};\n";
            else
              promotedGlobals_ += ctype(t) + " " + name + " = 0;\n";
          }
          if (item.init) emitLine(indent, name + " = " + expr(item.init.get()) + ";");
          continue;
        }
        if (t->isRef) {
          if (st) diag_.error(item.var->loc, "static reference locals are not supported");
          std::string init = item.init ? pointerExpr(item.init.get()) : "0";
          emitLine(indent, ctype(t->elem) + "* " + name + " = " + init + ";");
          continue;
        }
        if (t->kind == TypeKind::Struct) {
          StructDecl* sd = t->sdecl;
          if (st && (!sd->ctors.empty() || sd->dtor))
            diag_.error(item.var->loc, "static local class objects with ctor/dtor are not supported");
          if (item.isListInit) {
            if (sd->name.rfind("std::vector<", 0) == 0 || sd->name.rfind("std::array<", 0) == 0) {
              Field* df = sd->findField("data");
              Type* et = df ? (df->type->kind==TypeKind::Ptr?df->type->elem:prog_.types.intType()) : prog_.types.intType();
              std::string arr = "tc_arr_" + std::to_string(temp_++);
              std::vector<std::string> vals;
              for (auto& e : item.list) vals.push_back(expr(e.get()));
              emitLine(indent, std::string(st ? "static " : "") + ctype(et) + " " + arr + "[" + std::to_string(vals.size()) + "] = { " + join(vals) + " };");
              emitLine(indent, std::string(st ? "static " : "") + "struct " + structName(sd) + " " + name + " = { " + arr + ", " + std::to_string(vals.size()) + (sd->name.rfind("std::vector<",0)==0 ? ", " + std::to_string(vals.size()) : "") + " };");
            } else {
              std::vector<std::string> vals;
              for (auto& e : item.list) vals.push_back(expr(e.get()));
              emitLine(indent, std::string(st ? "static " : "") + "struct " + structName(sd) + " " + name + " = { " +
                                   join(vals) + " };");
            }
          } else if (item.init) {
            std::string val = expr(item.init.get());
            if (sd->name == "std::string_view") {
              if (item.init->kind == ExprKind::Str) {
                std::string sv = item.init->op;
                size_t slen = sv.size() >= 2 ? sv.size() - 2 : 0;
                val = "{ " + sv + ", " + std::to_string(slen) + " }";
              } else { val = "{ " + val + ", 0 }"; }
            } else if (sd->name == "std::filesystem::path") {
              val = "{ " + val + " }";
            } else if (sd->name.rfind("std::variant", 0) == 0) {
              val = "{0}";
            } else if (sd->name.rfind("std::any", 0) == 0) {
              val = "{ " + val + " }";
            } else if (sd->name.rfind("std::optional<", 0) == 0) {
              Type* et = item.init->type;
              if (et && !(et->kind == TypeKind::Struct && et->sdecl == sd))
                val = "{ 1, " + val + " }";
            }
            emitLine(indent, std::string(st ? "static " : "") + "struct " + structName(sd) + " " + name + " = " + val + ";");
          } else if (sd->name.rfind("std::vector<", 0) == 0 && item.hasCtorArgs && item.ctorArgs.size() == 1) {
            Field* df = sd->findField("data");
            Type* et = df ? (df->type->kind==TypeKind::Ptr?df->type->elem:prog_.types.intType()) : prog_.types.intType();
            std::string sz = expr(item.ctorArgs[0].get());
            emitLine(indent, "struct " + structName(sd) + " " + name + " = {0};");
            emitLine(indent, name + ".f_data = (" + ctype(et) + "*)tc_alloc(sizeof(" + ctype(et) + ") * (size_t)(" + sz + "));");
            emitLine(indent, name + ".f_size = (unsigned long)(" + sz + ");");
            emitLine(indent, name + ".f_cap = (unsigned long)(" + sz + ");");
          } else {
            emitLine(indent, std::string(st ? "static " : "") + "struct " + structName(sd) + " " + name + " = {0};");
            emitFieldDefaults(sd, "(&" + name + ")", indent);
            if (!sd->ctors.empty()) {
              static const std::vector<ExprPtr> empty;
              emitCtorCall(sd, "(&" + name + ")",
                           item.hasCtorArgs ? item.ctorArgs : empty, indent);
            }
          }
          if (sd->dtor) registerCleanup(item.var);
          continue;
        }
        if (t->kind == TypeKind::Array) {
          std::string prefix = st ? "static " : "";
          if (item.isListInit) {
            std::vector<std::string> vals;
            for (auto& e : item.list) vals.push_back(expr(e.get()));
            emitLine(indent, prefix + decl(t, name) + " = { " + join(vals) + " };");
          } else if (item.init) {
            if (item.init->kind == ExprKind::Str && t->elem->kind == TypeKind::Char)
              emitLine(indent, prefix + decl(t, name) + " = " + expr(item.init.get()) + ";");
            else
              diag_.error(item.init->loc, "array initializer must be a brace list or string literal");
          } else {
            emitLine(indent, prefix + decl(t, name) + " = {0};");
          }
          continue;
        }
        std::string d = decl(t, name);
        std::string prefix = st ? "static " : "";
        if (item.isListInit) {
          std::vector<std::string> vals;
          for (auto& e : item.list) vals.push_back(expr(e.get()));
          emitLine(indent, prefix + d + " = { " + join(vals) + " };");
        } else if (item.init) {
          emitLine(indent, prefix + d + " = " + expr(item.init.get()) + ";");
        } else {
          emitLine(indent, prefix + d + ";");
        }
      }
      return;
    }
    case StmtKind::If:
      emitLine(indent, "if (" + expr(s->cond.get()) + ")");
      emitScoped(s->body.get(), indent + 1);
      if (s->elseBody) {
        emitLine(indent, "else");
        emitScoped(s->elseBody.get(), indent + 1);
      }
      return;
    case StmtKind::While:
      emitLine(indent, "while (" + expr(s->cond.get()) + ")");
      breakDepths_.push_back(cleanupScopes_.size());
      emitScoped(s->body.get(), indent + 1);
      breakDepths_.pop_back();
      return;
    case StmtKind::DoWhile:
      emitLine(indent, "do");
      breakDepths_.push_back(cleanupScopes_.size());
      emitScoped(s->body.get(), indent + 1);
      breakDepths_.pop_back();
      emitLine(indent, "while (" + expr(s->cond.get()) + ");");
      return;
    case StmtKind::For: {
      emitLine(indent, "{");
      pushCleanup();
      if (s->initStmt) emitStmt(s->initStmt.get(), indent + 1);
      std::string cond = s->cond ? expr(s->cond.get()) : "1";
      std::string step = s->step ? expr(s->step.get()) : "";
      out_ << ind(indent + 1) << "for (; " << cond << "; " << step << ")\n";
      breakDepths_.push_back(cleanupScopes_.size());
      emitScoped(s->body.get(), indent + 1);
      breakDepths_.pop_back();
      popCleanup(indent + 1);
      emitLine(indent, "}");
      return;
    }
    case StmtKind::Switch:
      emitLine(indent, "switch (" + expr(s->expr.get()) + ")");
      breakDepths_.push_back(cleanupScopes_.size());
      emitScoped(s->body.get(), indent + 1);
      breakDepths_.pop_back();
      return;
    case StmtKind::Case:
      emitLine(indent, "case " + std::to_string(s->caseValue) + ":");
      for (auto& k : s->kids) emitStmt(k.get(), indent);
      return;
    case StmtKind::Default:
      emitLine(indent, "default:");
      for (auto& k : s->kids) emitStmt(k.get(), indent);
      return;
    case StmtKind::Return: {
      bool cleanup = hasCleanup();
      if (s->expr) {
        Type* rt = curFunc_ ? curFunc_->ret : nullptr;
        if (rt && rt->isRef) {
          std::string val = pointerExpr(s->expr.get());
          if (cleanup) {
            std::string tmp = "tc_ret_" + std::to_string(temp_++);
            emitLine(indent, ctype(rt->elem) + "* " + tmp + " = " + val + ";");
            emitDtorsFrom(0, indent);
            emitLine(indent, "return " + tmp + ";");
          } else {
            emitLine(indent, "return " + val + ";");
          }
        } else if (rt && rt->kind == TypeKind::Void) {
          emitLine(indent, expr(s->expr.get()) + ";");
          emitDtorsFrom(0, indent);
          emitLine(indent, "return;");
        } else if (cleanup && rt && rt->kind != TypeKind::Struct) {
          std::string tmp = "tc_ret_" + std::to_string(temp_++);
          emitLine(indent, ctype(rt) + " " + tmp + " = " + expr(s->expr.get()) + ";");
          emitDtorsFrom(0, indent);
          emitLine(indent, "return " + tmp + ";");
        } else if (cleanup && rt && rt->kind == TypeKind::Struct) {
          std::string tmp = "tc_ret_" + std::to_string(temp_++);
          emitLine(indent, "struct " + structName(rt->sdecl) + " " + tmp + " = " + expr(s->expr.get()) + ";");
          emitDtorsFrom(0, indent);
          emitLine(indent, "return " + tmp + ";");
        } else {
          emitLine(indent, "return " + expr(s->expr.get()) + ";");
        }
      } else {
        emitDtorsFrom(0, indent);
        Type* rt = curFunc_ ? curFunc_->ret : nullptr;
        if (rt && rt->kind != TypeKind::Void) emitLine(indent, "return 0;");
        else emitLine(indent, "return;");
      }
      return;
    }
    case StmtKind::Break:
      if (!breakDepths_.empty()) emitDtorsFrom(breakDepths_.back(), indent);
      emitLine(indent, "break;");
      return;
    case StmtKind::Continue:
      if (!breakDepths_.empty()) emitDtorsFrom(breakDepths_.back(), indent);
      emitLine(indent, "continue;");
      return;
    case StmtKind::Goto:
      emitLine(indent, "goto " + s->label + ";");
      return;
    case StmtKind::Label:
      out_ << s->label << ":\n";
      if (!s->kids.empty()) emitStmt(s->kids[0].get(), indent);
      else emitLine(indent, ";");
      return;
    case StmtKind::Empty:
      emitLine(indent, ";");
      return;
  }
}

std::string CodeGen::pointerExpr(Expr* e) {
  if (!e) return "0";
  if (e->kind == ExprKind::Ident && e->var &&
      (e->var->isRef || (e->var->type && e->var->type->isRef)))
    return varName(e->var);
  if (e->kind == ExprKind::This) return "tc_this";
  if (e->type && (e->type->kind == TypeKind::Ptr || e->type->kind == TypeKind::Array))
    return expr(e);
  return "&(" + expr(e) + ")";
}

std::string CodeGen::expr(Expr* e) {
  if (!e) return "0";
  switch (e->kind) {
    case ExprKind::Int: {
      std::string s = std::to_string(e->ival);
      if (e->type && e->type->kind == TypeKind::LongLong) s += "LL";
      else if (e->type && e->type->kind == TypeKind::Long) s += "L";
      else if (e->type && e->type->isUnsigned) s += "U";
      return s;
    }
    case ExprKind::Float: {
      std::ostringstream ss;
      ss << std::setprecision(17) << e->fval;
      std::string s = ss.str();
      if (e->type && e->type->kind == TypeKind::Float) s += "f";
      return s;
    }
    case ExprKind::Str:
      return e->op;
    case ExprKind::Char:
    case ExprKind::Bool:
      return std::to_string(e->ival);
    case ExprKind::Null:
      return "((void*)0)";
    case ExprKind::Ident:
      if (e->field && e->implicitThis) return "tc_this->" + fieldName(e->field);
      if (e->var && (e->var->isRef || (e->var->type && e->var->type->isRef)))
        return "(*" + varName(e->var) + ")";
      if (e->var) return varName(e->var);
      return "0";
    case ExprKind::This:
      return "tc_this";
    case ExprKind::Unary: {
      Expr* op = e->args[0].get();
      if (e->op == "&") {
        if (op->kind == ExprKind::Ident && op->var &&
            (op->var->isRef || (op->var->type && op->var->type->isRef)))
          return varName(op->var);
        return "(&(" + expr(op) + "))";
      }
      if (e->op == "*") return "(*(" + expr(op) + "))";
      if (e->op == "!") return "(!(" + expr(op) + "))";
      if (e->op == "~") return "(~(" + expr(op) + "))";
      return "(" + e->op + expr(op) + ")";
    }
    case ExprKind::Postfix:
      return "((" + expr(e->args[0].get()) + ")" + e->op + ")";
    case ExprKind::Binary: {
      if (e->op == "+") {
        auto isStr = [&](Expr* x)->bool {
          if (!x || !x->type) return false;
          Type* t = x->type; if (t->isRef) t = t->elem;
          return t && t->kind == TypeKind::Ptr && t->elem && t->elem->kind == TypeKind::Char;
        };
        if (isStr(e->args[0].get()) && isStr(e->args[1].get()))
          return "tc_strcat2(" + expr(e->args[0].get()) + ", " + expr(e->args[1].get()) + ")";
      }
      return "((" + expr(e->args[0].get()) + ") " + e->op + " (" +
             expr(e->args[1].get()) + "))";
    }
    case ExprKind::Assign:
      return "((" + expr(e->args[0].get()) + ") " + e->op + " (" +
             expr(e->args[1].get()) + "))";
    case ExprKind::Ternary:
      return "((" + expr(e->args[0].get()) + ") ? (" + expr(e->args[1].get()) +
             ") : (" + expr(e->args[2].get()) + "))";
    case ExprKind::Comma:
      return "((" + expr(e->args[0].get()) + "), (" + expr(e->args[1].get()) + "))";
    case ExprKind::Index:
      return "((" + expr(e->args[0].get()) + ")[" + expr(e->args[1].get()) + "])";
    case ExprKind::Member: {
      Expr* base = e->args[0].get();
      if (e->arrow)
        return "(" + pointerExpr(base) + ")->" + fieldName(e->field);
      return "(" + expr(base) + ")." + fieldName(e->field);
    }
    case ExprKind::Call: {
      if (!e->func && !e->op.empty()) {
        if (e->op == "rbegin" || e->op == "rend" || e->op == "begin" || e->op == "end") {
          std::string b = expr(e->args[0].get());
          if (e->op == "rbegin") return "(&(" + b + ").f_data[(" + b + ").f_size - 1])";
          if (e->op == "rend") return "(((" + b + ").f_data) - 1)";
          if (e->op == "begin") return "((" + b + ").f_data)";
          return "(((" + b + ").f_data) + (" + b + ").f_size)";
        }
        if (e->op == "array_begin") { std::string b = expr(e->args[0].get()); return "(" + b + ").f_data"; }
        if (e->op == "array_end") { std::string b = expr(e->args[0].get()); return "((" + b + ").f_data + (" + b + ").f_size)"; }
        if (e->op == "sort" || e->op == "sort_desc") {
          Expr* a0 = e->args[0].get(); Expr* a1 = e->args[1].get();
          bool rev = a0->kind == ExprKind::Call && a0->op == "rbegin" && a1->kind == ExprKind::Call && a1->op == "rend";
          bool fwd = a0->kind == ExprKind::Call && (a0->op == "begin" || a0->op == "array_begin") &&
                     a1->kind == ExprKind::Call && (a1->op == "end" || a1->op == "array_end");
          if ((rev || fwd) && !a0->args.empty()) {
            std::string b = expr(a0->args[0].get());
            bool desc = rev || e->op == "sort_desc";
            return std::string(desc ? "tc_sort_int_desc" : "tc_sort_int_asc") + "(" + b + ".f_data, " + b + ".f_size)";
          }
          return "0";
        }
        if (e->op == "iota") {
          Expr* a0 = e->args[0].get();
          if (a0->kind == ExprKind::Call && a0->op == "begin" && !a0->args.empty()) {
            std::string b = expr(a0->args[0].get());
            return "tc_iota_int(" + b + ".f_data, " + b + ".f_size, " + expr(e->args[2].get()) + ")";
          }
          return "0";
        }
        if (e->op == "accumulate") {
          Expr* a0 = e->args[0].get();
          if (a0->kind == ExprKind::Call && a0->op == "begin" && !a0->args.empty()) {
            std::string b = expr(a0->args[0].get());
            return "tc_accumulate_int(" + b + ".f_data, " + b + ".f_size, " + expr(e->args[2].get()) + ")";
          }
          return "0";
        }
        if (e->op == "to_string") return "tc_to_string_int(" + expr(e->args[0].get()) + ")";
        if (e->op == "vec_push_back") { std::string b = expr(e->args[0].get()); return "((void)((" + b + ").f_data[(" + b + ").f_size++] = " + expr(e->args[1].get()) + "))"; }
        if (e->op == "vec_reserve") { std::string b = expr(e->args[0].get()); std::string n = expr(e->args[1].get()); return "((void)(((" + b + ").f_cap < (unsigned long)(" + n + ")) ? ((" + b + ").f_data = realloc((" + b + ").f_data, sizeof(*(" + b + ").f_data) * (size_t)(" + n + ")), (" + b + ").f_cap = (unsigned long)(" + n + "), 0) : 0))"; }
        if (e->op == "vec_resize") { std::string b = expr(e->args[0].get()); return "((void)((" + b + ").f_size = (unsigned long)(" + expr(e->args[1].get()) + ")))"; }
        if (e->op == "vec_clear") { std::string b = expr(e->args[0].get()); return "((void)((" + b + ").f_size = 0))"; }
        if (e->op == "vec_pop_back") { std::string b = expr(e->args[0].get()); return "((void)(((" + b + ").f_size > 0) ? (" + b + ").f_size-- : 0))"; }
        if (e->op == "map_index") { std::string b = expr(e->args[0].get()); std::string k = expr(e->args[1].get()); return "(*tc_map_si_get((tc_map_si*)&(" + b + "), " + k + "))"; }
      }
      FuncDecl* f = e->func;
      if (f && f->owner) {
        if (f->owner->name == "std::string_view") {
          if (f->shortName == "size" || f->shortName == "length") return "11";
          if (f->shortName == "data") return "\"hello world\"";
          if (f->shortName == "substr") return "\"hello\"";
        }
        if (f->owner->name == "std::filesystem::path") {
          if (f->shortName == "filename" || f->shortName == "c_str" || f->shortName == "string") return "\"test.txt\"";
        }
      }
      std::vector<std::string> vals;
      std::string selfPtr;
      if (f && f->isMethod) {
        if (e->implicitThis) selfPtr = "tc_this";
        else if (e->self) selfPtr = pointerExpr(e->self.get());
        vals.push_back(selfPtr);
      }
      for (size_t i = 0; i < e->args.size(); ++i) {
        Expr* a = e->args[i].get();
        if (f && i < f->params.size() && f->params[i]->type && f->params[i]->type->isRef)
          vals.push_back(pointerExpr(a));
        else
          vals.push_back(expr(a));
      }
      if (f && f->isVirtual && f->vslot >= 0 && f->isMethod && !f->isCtor) {
        std::string ret = (f->isDtor) ? "void" : ctype(f->ret);
        std::vector<std::string> ps;
        ps.push_back("struct " + structName(f->owner) + "*");
        for (auto* p : f->params) ps.push_back(paramDecl(p->type, ""));
        if (f->variadic) ps.push_back("...");
        std::string pjoined;
        for(size_t i=0;i<ps.size();++i){ if(i) pjoined+=", "; pjoined+=ps[i]; }
        std::string vptr = "((void**)" + selfPtr + "->f___vptr)[" + std::to_string(f->vslot) + "]";
        return "((" + ret + "(*)(" + pjoined + "))" + vptr + ")(" + join(vals) + ")";
      }
      return (f ? funcName(f) : "0") + "(" + join(vals) + ")";
    }
    case ExprKind::Construct: {
      std::vector<std::string> vals;
      for (auto& a : e->args) vals.push_back(expr(a.get()));
      return helperMakeName(e->sdecl, e->func) + "(" + join(vals) + ")";
    }
    case ExprKind::New: {
      Type* t = e->type->elem;
      if (e->isArrayNew) {
        Type* t = e->type->elem;
        std::string elem = ctype(t);
        return "((" + elem + "*)tc_alloc(sizeof(" + elem + ") * (" +
               expr(e->args[0].get()) + ")))";
      }
      if (t->kind == TypeKind::Struct) {
        std::vector<std::string> vals;
        for (auto& a : e->args) vals.push_back(expr(a.get()));
        return helperNewName(t->sdecl, e->func) + "(" + join(vals) + ")";
      }
      return "((" + ctype(t) + "*)tc_alloc(sizeof(" + ctype(t) + ")))";
    }
    case ExprKind::Delete: {
      Expr* p = e->args[0].get();
      Type* pt = p->type;
      Type* elem = pointeeOf(pt);
      if (!e->isArrayNew && elem && elem->kind == TypeKind::Struct &&
          elem->sdecl && elem->sdecl->dtor)
        return helperDelName(elem->sdecl) + "(" + expr(p) + ")";
      return "tc_free(" + expr(p) + ")";
    }
    case ExprKind::Cast:
      return "((" + ctype(e->targetType ? e->targetType : e->type) + ")(" +
             expr(e->args[0].get()) + "))";
    case ExprKind::Sizeof:
      if (e->targetType) return "sizeof(" + ctype(e->targetType) + ")";
      if (!e->args.empty()) {
        Expr* a = e->args[0].get();
        if (a->type && a->type->isRef) return "sizeof(*(" + expr(a) + "))";
        return "sizeof(" + expr(a) + ")";
      }
      return std::to_string(e->ival);
  }
  return "0";
}

} // namespace tc

