#include "parser.h"
#include "literal.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <iostream>

namespace tc {

bool sameType(const Type* a, const Type* b) {
  if (a == b) return true;
  if (!a || !b) return false;
  if (a->kind != b->kind || a->isUnsigned != b->isUnsigned || a->isRef != b->isRef)
    return false;
  if (a->kind == TypeKind::Ptr || a->kind == TypeKind::Array) {
    if (a->kind == TypeKind::Array && a->arrayLen != b->arrayLen) return false;
    return sameType(a->elem, b->elem);
  }
  if (a->kind == TypeKind::Struct) return a->sdecl == b->sdecl;
  return true;
}

bool isPointerLike(const Type* t) {
  return t && (t->kind == TypeKind::Ptr || t->kind == TypeKind::Array || t->isRef);
}

Type* pointeeOf(Type* t) {
  if (!t) return nullptr;
  if (t->kind == TypeKind::Ptr || t->kind == TypeKind::Array || t->isRef)
    return t->elem;
  return nullptr;
}

TypeContext::TypeContext() = default;

Type* TypeContext::builtin(TypeKind k, bool isUnsigned) {
  auto key = std::make_pair(static_cast<int>(k), isUnsigned);
  auto it = prim_.find(key);
  if (it != prim_.end()) return it->second;
  auto t = std::make_unique<Type>();
  t->kind = k;
  t->isUnsigned = isUnsigned;
  Type* p = t.get();
  pool_.push_back(std::move(t));
  prim_[key] = p;
  return p;
}

Type* TypeContext::ptrTo(Type* t) {
  auto p = std::make_unique<Type>();
  p->kind = TypeKind::Ptr;
  p->elem = t;
  Type* r = p.get();
  pool_.push_back(std::move(p));
  return r;
}

Type* TypeContext::refTo(Type* t) {
  auto p = std::make_unique<Type>();
  p->kind = TypeKind::Ptr;
  p->isRef = true;
  p->elem = t;
  Type* r = p.get();
  pool_.push_back(std::move(p));
  return r;
}

Type* TypeContext::arrayOf(Type* t, long long n) {
  auto p = std::make_unique<Type>();
  p->kind = TypeKind::Array;
  p->elem = t;
  p->arrayLen = n;
  Type* r = p.get();
  pool_.push_back(std::move(p));
  return r;
}

static long long alignUp(long long n, long long a) {
  if (a <= 0) return n;
  return (n + a - 1) / a * a;
}

static long long typeSizeOf(Type* t) {
  if (!t) return 1;
  switch (t->kind) {
    case TypeKind::Void: return 1;
    case TypeKind::Bool: return 1;
    case TypeKind::Char: return 1;
    case TypeKind::Short: return 2;
    case TypeKind::Int: return 4;
    case TypeKind::Long: return static_cast<long long>(sizeof(long));
    case TypeKind::LongLong: return 8;
    case TypeKind::Float: return 4;
    case TypeKind::Double: return 8;
    case TypeKind::Ptr: return static_cast<long long>(sizeof(void*));
    case TypeKind::Array: return t->arrayLen * typeSizeOf(t->elem);
    case TypeKind::Struct: {
      long long off = 0;
      long long maxAlign = 1;
      for (auto& f : t->sdecl->fields) {
        long long sz = typeSizeOf(f.type);
        long long al = sz > 8 ? 8 : sz;
        if (al < 1) al = 1;
        maxAlign = std::max(maxAlign, al);
        off = alignUp(off, al);
        off += sz;
      }
      return alignUp(off, maxAlign);
    }
  }
  return 1;
}

Parser::Parser(SourceManager& sm, Diag& diag, std::vector<Token> toks)
    : sm_(sm), diag_(diag), toks_(std::move(toks)) {
  scopes_.emplace_back();
}

const Token& Parser::peek(int n) const {
  static Token eof;
  size_t i = pos_ + static_cast<size_t>(n);
  return i < toks_.size() ? toks_[i] : eof;
}

Token Parser::take() {
  if (pos_ < toks_.size()) return toks_[pos_++];
  return Token{};
}

bool Parser::at(const char* s) const {
  const Token& t = peek();
  return (t.kind == Tok::Punct || t.kind == Tok::Ident) && t.text == s;
}

bool Parser::atIdent(const char* s) const { return isIdent(peek(), s); }

bool Parser::eat(const char* s) {
  if (!at(s)) return false;
  ++pos_;
  return true;
}

void Parser::expect(const char* s) {
  if (!at(s)) diag_.error(peek().loc, std::string("expected '") + s + "', got '" + peek().text + "'");
  ++pos_;
}

std::string Parser::expectIdent() {
  if (peek().kind != Tok::Ident)
    diag_.error(peek().loc, "expected identifier, got '" + peek().text + "'");
  return take().text;
}

bool Parser::atEof() const { return peek().kind == Tok::Eof; }

void Parser::skipBalanced(const char* open, const char* close) {
  expect(open);
  int depth = 1;
  while (depth > 0) {
    if (atEof()) diag_.error(peek().loc, "unexpected end of file");
    if (at(open)) ++depth;
    else if (at(close)) --depth;
    take();
  }
}

void Parser::skipAttributes() {
  while (at("__attribute__") || at("__attribute") || at("__declspec") ||
         at("__asm") || at("__asm__") || at("__extension__")) {
    if (eat("__extension__")) continue;
    take();
    if (at("(")) skipBalanced("(", ")");
  }
}

void Parser::pushScope() { scopes_.emplace_back(); }
void Parser::popScope() { scopes_.pop_back(); }

void Parser::declareVar(VarSym* v) {
  if (!v) return;
  if (v->isGlobal)
    globals_[v->name] = v;
  else
    scopes_.back()[v->name] = v;
}

VarSym* Parser::lookupVar(const std::string& name) {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    auto f = it->find(name);
    if (f != it->end()) return f->second;
  }
  auto g = globals_.find(name);
  if (g != globals_.end()) return g->second;
  return nullptr;
}

void Parser::registerFunc(FuncDecl* f) {
  funcs_.insert({f->name, f});
}

static int typeDistance(Type* param, Type* arg) {
  if (!param || !arg) return -1;
  if (param->isRef) param = param->elem;
  if (arg->isRef) arg = arg->elem;
  if (sameType(param, arg)) return 0;
  if (param->kind == TypeKind::Ptr && arg->kind == TypeKind::Ptr && arg->elem && arg->elem->kind == TypeKind::Void) return 1;
  if (param->kind == TypeKind::Ptr && arg->kind == TypeKind::Ptr && param->elem && param->elem->kind == TypeKind::Void) return 2;
  if (isPointerLike(param) && isPointerLike(arg)) {
    Type* pa = pointeeOf(param), *aa = pointeeOf(arg);
    if (pa && aa && sameType(pa, aa)) return 1;
    if (pa && aa && pa->kind == TypeKind::Void) return 2;
  }
  if (isPointerLike(param) || isPointerLike(arg)) return -1;
  if (param->kind == TypeKind::Double && (arg->kind == TypeKind::Float ||
      arg->kind == TypeKind::Int || arg->kind == TypeKind::Long ||
      arg->kind == TypeKind::Short || arg->kind == TypeKind::Char ||
      arg->kind == TypeKind::Bool)) return 2;
  if (param->kind == TypeKind::Float && (arg->kind == TypeKind::Int ||
      arg->kind == TypeKind::Short || arg->kind == TypeKind::Char ||
      arg->kind == TypeKind::Bool)) return 2;
  if ((param->kind == TypeKind::Int || param->kind == TypeKind::Long ||
       param->kind == TypeKind::LongLong || param->kind == TypeKind::Short ||
       param->kind == TypeKind::Char || param->kind == TypeKind::Bool) &&
      (arg->kind == TypeKind::Int || arg->kind == TypeKind::Long ||
       arg->kind == TypeKind::LongLong || arg->kind == TypeKind::Short ||
       arg->kind == TypeKind::Char || arg->kind == TypeKind::Bool)) return 1;
  return -1;
}

int Parser::overloadScore(FuncDecl* f, const std::vector<ExprPtr>& args) {
  size_t fixed = f->params.size();
  if (args.size() > fixed && !f->variadic) return -1;
  if (args.size() < fixed) {
    bool ok = true;
    for (size_t i = args.size(); i < fixed; ++i)
      if (!f->params[i]->defaultArg) ok = false;
    if (!ok) return -1;
  }
  int score = 0;
  for (size_t i = 0; i < args.size() && i < fixed; ++i) {
    int d = typeDistance(f->params[i]->type, args[i]->type);
    if (d < 0) return -1;
    score += d;
  }
  if (args.size() > fixed) score += 3;
  return score;
}

FuncDecl* Parser::lookupFunc(const std::string& name) {
  if (currentClass_) {
    if (auto* m = currentClass_->findMethod(name)) return m;
  }
  auto tryName = [&](const std::string& n) -> FuncDecl* {
    auto range = funcs_.equal_range(qualify(n));
    for (auto it = range.first; it != range.second; ++it) return it->second;
    for (const auto& ns : usingNamespaces_) {
      auto r2 = funcs_.equal_range(ns + "::" + n);
      for (auto it = r2.first; it != r2.second; ++it) return it->second;
    }
    auto r3 = funcs_.equal_range(n);
    for (auto it = r3.first; it != r3.second; ++it) return it->second;
    return nullptr;
  };
  if (auto* f = tryName(name)) return f;
  if (name.rfind("std::", 0) == 0) if (auto* f = tryName(name.substr(5))) return f;
  return nullptr;
}

FuncDecl* Parser::lookupFunc(const std::string& name, const std::vector<ExprPtr>& args) {
  std::vector<FuncDecl*> cands;
  auto collectAll = [&](const std::string& n) {
    if (currentClass_) {
      for (auto* m : currentClass_->findMethods(n)) cands.push_back(m);
    }
    auto collect = [&](const std::string& qn) {
      auto r = funcs_.equal_range(qn);
      for (auto it = r.first; it != r.second; ++it) cands.push_back(it->second);
    };
    collect(qualify(n));
    for (const auto& ns : usingNamespaces_) collect(ns + "::" + n);
    collect(n);
  };
  collectAll(name);
  if (cands.empty() && name.rfind("std::", 0) == 0) collectAll(name.substr(5));

  FuncDecl* best = nullptr;
  int bestScore = 1 << 30;
  bool ambig = false;
  for (auto* f : cands) {
    int sc = overloadScore(f, args);
    if (sc < 0) continue;
    if (!best || sc < bestScore) { best = f; bestScore = sc; ambig = false; }
    else if (sc == bestScore && best != f) ambig = true;
  }
  if (ambig) diag_.error(peek().loc, "ambiguous call to '" + name + "'");
  if (best) return best;
  // try template instantiation
  for (auto& tf : templates_) {
    if (tf.name == name || qualify(tf.name) == qualify(name)) {
      FuncDecl* inst = instantiateTemplate(tf, args, peek().loc);
      if (inst) return inst;
    }
  }
  return lookupFunc(name);
}

StructDecl* Parser::lookupStruct(const std::string& name) {
  auto it = structs_.find(qualify(name));
  if (it != structs_.end()) return it->second;
  it = structs_.find(name);
  if (it != structs_.end()) return it->second;
  return nullptr;
}

Type* Parser::lookupType(const std::string& name) {
  auto it = typedefs_.find(qualify(name));
  if (it != typedefs_.end()) return it->second;
  it = typedefs_.find(name);
  if (it != typedefs_.end()) return it->second;
  if (auto* s = lookupStruct(name)) return structType(s);
  return nullptr;
}

Type* Parser::structType(StructDecl* sd) {
  auto t = std::make_unique<Type>();
  t->kind = TypeKind::Struct;
  t->sdecl = sd;
  Type* p = t.get();
  prog_.extraTypes.push_back(std::move(t));
  return p;
}

std::string Parser::qualify(const std::string& name) const {
  if (nsPrefix_.empty()) return name;
  return nsPrefix_ + "::" + name;
}

VarSym* Parser::newVar(const std::string& name, Type* type, SourceLoc loc) {
  auto v = std::make_unique<VarSym>();
  v->name = name;
  v->type = type;
  v->isRef = type && type->isRef;
  v->loc = loc;
  VarSym* p = v.get();
  prog_.vars.push_back(std::move(v));
  return p;
}

FuncDecl* Parser::newFunc(const std::string& name, Type* ret, SourceLoc loc) {
  auto f = std::make_unique<FuncDecl>();
  f->name = name;
  f->shortName = name;
  f->ret = ret;
  f->loc = loc;
  FuncDecl* p = f.get();
  prog_.funcs.push_back(std::move(f));
  return p;
}

StructDecl* Parser::getOrCreateStruct(const std::string& name, SourceLoc loc) {
  auto it = structs_.find(name);
  if (it != structs_.end()) return it->second;

  auto s = std::make_unique<StructDecl>();
  s->name = name;
  s->loc = loc;
  StructDecl* p = s.get();
  prog_.structs.push_back(std::move(s));
  structs_[name] = p;
  return p;
}

// ---------------------------------------------------------------------------
// Builtins
// ---------------------------------------------------------------------------

void Parser::registerBuiltins() {
  Type* voidT = prog_.types.voidType();
  Type* intT = prog_.types.intType();
  Type* charT = prog_.types.charType();
  Type* doubleT = prog_.types.doubleType();
  Type* longT = prog_.types.longType();
  Type* ulongT = prog_.types.builtin(TypeKind::Long, true);
  Type* uintT = prog_.types.builtin(TypeKind::Int, true);
  Type* voidPtr = prog_.types.ptrTo(voidT);
  Type* charPtr = prog_.types.ptrTo(charT);
  Type* doublePtr = prog_.types.ptrTo(doubleT);

  typedefs_["size_t"] = ulongT;
  typedefs_["ssize_t"] = longT;
  typedefs_["ptrdiff_t"] = longT;
  typedefs_["intptr_t"] = longT;
  typedefs_["uintptr_t"] = ulongT;
  typedefs_["int8_t"] = prog_.types.builtin(TypeKind::Char);
  typedefs_["uint8_t"] = prog_.types.builtin(TypeKind::Char, true);
  typedefs_["int16_t"] = prog_.types.builtin(TypeKind::Short);
  typedefs_["uint16_t"] = prog_.types.builtin(TypeKind::Short, true);
  typedefs_["int32_t"] = intT;
  typedefs_["uint32_t"] = uintT;
  typedefs_["int64_t"] = prog_.types.builtin(TypeKind::LongLong);
  typedefs_["uint64_t"] = prog_.types.builtin(TypeKind::LongLong, true);
  typedefs_["wchar_t"] = intT;

  enumConsts_["NULL"] = 0;
  enumConsts_["EOF"] = -1;

  auto add = [&](const std::string& name, Type* ret, std::vector<Type*> ps,
                 bool variadic = false) {
    FuncDecl* f = newFunc(name, ret, SourceLoc{});
    f->isExternC = true;
    f->variadic = variadic;
    for (size_t i = 0; i < ps.size(); ++i) {
      VarSym* p = newVar("arg" + std::to_string(i), ps[i], SourceLoc{});
      p->isParam = true;
      f->params.push_back(p);
    }
    registerFunc(f);
  };

  add("printf", intT, {charPtr}, true);
  add("fprintf", intT, {voidPtr, charPtr}, true);
  add("sprintf", intT, {charPtr, charPtr}, true);
  add("snprintf", intT, {charPtr, ulongT, charPtr}, true);
  add("scanf", intT, {charPtr}, true);
  add("sscanf", intT, {charPtr, charPtr}, true);
  add("puts", intT, {charPtr});
  add("putchar", intT, {intT});
  add("fputs", intT, {charPtr, voidPtr});
  add("fgets", charPtr, {charPtr, intT, voidPtr});
  add("perror", voidT, {charPtr});
  add("malloc", voidPtr, {ulongT});
  add("calloc", voidPtr, {ulongT, ulongT});
  add("realloc", voidPtr, {voidPtr, ulongT});
  add("free", voidT, {voidPtr});
  add("memset", voidPtr, {voidPtr, intT, ulongT});
  add("memcpy", voidPtr, {voidPtr, voidPtr, ulongT});
  add("memmove", voidPtr, {voidPtr, voidPtr, ulongT});
  add("strlen", ulongT, {charPtr});
  add("strcmp", intT, {charPtr, charPtr});
  add("strcpy", charPtr, {charPtr, charPtr});
  add("exit", voidT, {intT});
  add("abort", voidT, {});
  add("atoi", intT, {charPtr});
  add("atof", doubleT, {charPtr});
  add("sqrt", doubleT, {doubleT});
  add("pow", doubleT, {doubleT, doubleT});
  add("sin", doubleT, {doubleT});
  add("cos", doubleT, {doubleT});
  add("tan", doubleT, {doubleT});
  add("fabs", doubleT, {doubleT});
  add("floor", doubleT, {doubleT});
  add("ceil", doubleT, {doubleT});
  add("fmod", doubleT, {doubleT, doubleT});
  add("rand", intT, {});
  add("srand", voidT, {uintT});
  add("time", longT, {longT});
  add("fopen", voidPtr, {charPtr, charPtr});
  add("fclose", intT, {voidPtr});
  add("qsort", voidT, {voidPtr, ulongT, ulongT, voidPtr});
  add("abs", intT, {intT});
  (void)doublePtr;
}

// ---------------------------------------------------------------------------
// Top level
// ---------------------------------------------------------------------------

Program Parser::parse() {
  registerBuiltins();
  while (!atEof()) {
    size_t save = pos_;
    std::string saveNs = nsPrefix_;
    StructDecl* saveClass = currentClass_;
    FuncDecl* saveFunc = currentFunc_;
    bool saveExtern = externC_;
    try {
      parseTopLevel();
    } catch (const CompileError&) {
      nsPrefix_ = saveNs;
      currentClass_ = saveClass;
      currentFunc_ = saveFunc;
      externC_ = saveExtern;
      if (pos_ == save) take();
      int depth = 0;
      while (!atEof()) {
        if (at("{")) { depth++; take(); continue; }
        if (at("}")) { if (depth==0) break; depth--; take(); continue; }
        if (at(";") && depth==0) { take(); break; }
        take();
      }
    }
  }
  return std::move(prog_);
}

void Parser::parseTopLevel() {
  skipAttributes();
  if (eat(";")) return;
  if (at("[") && peek(1).text == "[") {
    take();
    take();
    while (!atEof() && !(at("]") && peek(1).text == "]")) take();
    eat("]");
    eat("]");
    return;
  }
  if (at("namespace")) { parseNamespace(); return; }
  if (at("extern") && peek(1).kind == Tok::String) { parseExternC(); return; }
  if (at("using")) { parseUsing(); return; }
  if (at("typedef")) { parseTypedef(); return; }
  if (at("enum")) { parseEnum(); return; }
  if (at("struct") || at("class") || at("union")) { parseStruct(); return; }
  if (at("template")) { parseTemplateFunc(); return; }
  if (at("static_assert")) { parseStaticAssert(); return; }
  if (at("friend")) { take(); return; }
  if (at("asm") || at("__asm__")) { skipBalanced("(", ")"); eat(";"); return; }

  // Out-of-line constructor/destructor: Foo::Foo(...) / Foo::~Foo().
  if (peek().kind == Tok::Ident && peek(1).text == "::") {
    size_t save = pos_;
    std::string first = take().text;
    take(); // ::
    bool dtor = eat("~");
    std::string second = expectIdent();
    if (structs_.count(first) && second == first && at("(")) {
      StructDecl* sd = structs_[first];
      FuncDecl* fn = dtor ? sd->dtor : (sd->ctors.empty()?nullptr:sd->ctors.front());
      if (!fn) {
        fn = newFunc(first + "::" + (dtor ? "~" + second : second),
                     prog_.types.voidType(), peek().loc);
        fn->shortName = dtor ? "~" + second : second;
        fn->isMethod = true;
        fn->isCtor = !dtor;
        fn->isDtor = dtor;
        fn->owner = sd;
        if (dtor) sd->dtor = fn;
        else sd->ctors.push_back(fn);
        sd->methods.push_back(fn);
        registerFunc(fn);
      }
      StructDecl* saveClass = currentClass_;
      FuncDecl* saveFunc = currentFunc_;
      currentClass_ = sd;
      currentFunc_ = fn;
      pushScope();
      bool variadic = false;
      fn->params = parseParams(variadic);
      fn->variadic = variadic;
      for (auto* p : fn->params) declareVar(p);
      if (!dtor && at(":")) {
        take();
        while (true) {
          std::string fname = expectIdent();
          Field* f = sd->findField(fname);
          if (!f) {
            StructDecl* base = sd->base;
            bool isBase = base && (fname == base->name || fname == base->name.substr(base->name.rfind("::")+1));
            if (isBase) {
              fn->baseInit = base;
              if (eat("(")) { while(!at(")")&&!atEof()){ fn->baseInitArgs.push_back(parseExpression()); if(!eat(",")) break; } expect(")"); }
              else if (eat("{")) { while(!at("}")&&!atEof()){ fn->baseInitArgs.push_back(parseExpression()); if(!eat(",")) break; } expect("}"); }
              if (eat(",")) continue; break;
            }
            diag_.error(peek().loc, "unknown field '" + fname + "' in constructor");
          }
          ExprPtr val;
          if (eat("(")) { val = parseExpression(); expect(")"); }
          else if (eat("{")) { val = parseExpression(); expect("}"); }
          else diag_.error(peek().loc, "expected initializer for field '" + fname + "'");
          fn->initList.push_back(InitItem{f, std::move(val)});
          if (eat(",")) continue;
          break;
        }
      }
      popScope();
      if (at("{")) {
        fn->body = parseFunctionBody(fn);
        fn->isDefined = true;
      } else {
        expect(";");
      }
      currentClass_ = saveClass;
      currentFunc_ = saveFunc;
      return;
    }
    pos_ = save;
  }

  parseDeclaration(true);
}

void Parser::parseTemplateFunc() {
  SourceLoc loc = peek().loc;
  expect("template");
  if (!at("<")) { diag_.warning(loc, "skipping malformed template"); return; }
  // parse template params
  std::vector<std::string> tparams;
  take(); // <
  int depth = 1;
  std::string cur;
  while (!atEof() && depth > 0) {
    if (at("<")) { depth++; take(); continue; }
    if (at(">")) { depth--; if (depth==0) { take(); break; } take(); continue; }
    if (depth == 1 && (at("typename") || at("class"))) { take(); if (peek().kind==Tok::Ident) { tparams.push_back(take().text); } if (eat(",")) continue; continue; }
    if (depth == 1 && peek().kind == Tok::Ident) { take(); if (eat(",")) continue; continue; }
    take();
  }
  // capture following decl tokens
  std::vector<Token> toks;
  int bdepth = 0;
  bool gotName = false; std::string fname;
  // skip leading specifiers and get to function
  size_t startPos = pos_;
  // capture all tokens until ; or } at depth 0
  while (!atEof()) {
    Token t = peek();
    if (at("{")) { bdepth++; toks.push_back(take()); continue; }
    if (at("}")) { toks.push_back(take()); if(bdepth>0) bdepth--; if(bdepth==0) break; continue; }
    if (at(";") && bdepth==0) { toks.push_back(take()); break; }
    toks.push_back(take());
  }
  // extract function name: look for ident before (
  for (size_t i=0;i+1<toks.size();++i) {
    if (toks[i].kind==Tok::Ident && i+1<toks.size() && toks[i+1].text=="(") {
      fname = toks[i].text;
    }
  }
  if (fname.empty()) { diag_.warning(loc, "skipping unsupported template"); return; }
  TemplateFunc tf; tf.name=fname; tf.params=tparams; tf.tokens=std::move(toks); tf.loc=loc;
  templates_.push_back(std::move(tf));
}

FuncDecl* Parser::instantiateTemplate(const TemplateFunc& tf, const std::vector<ExprPtr>& args, SourceLoc loc) {
  if (tf.params.empty()) return nullptr;
  std::string tp = tf.params[0];
  std::string concrete = "int";
  if (!args.empty() && args[0]->type) {
    Type* at = args[0]->type; if (at->isRef) at = at->elem;
    switch(at->kind){
      case TypeKind::Bool: concrete="bool"; break;
      case TypeKind::Char: concrete=at->isUnsigned?"unsigned char":"signed char"; break;
      case TypeKind::Short: concrete=at->isUnsigned?"unsigned short":"short"; break;
      case TypeKind::Int: concrete=at->isUnsigned?"unsigned int":"int"; break;
      case TypeKind::Long: concrete=at->isUnsigned?"unsigned long":"long"; break;
      case TypeKind::LongLong: concrete=at->isUnsigned?"unsigned long long":"long long"; break;
      case TypeKind::Float: concrete="float"; break;
      case TypeKind::Double: concrete="double"; break;
      default: concrete="int"; break;
    }
  }
  std::vector<Token> sub;
  for (auto t : tf.tokens) {
    if (t.kind == Tok::Ident && t.text == tp) { t.text = concrete; }
    sub.push_back(std::move(t));
  }
  // inject at current position
  toks_.insert(toks_.begin() + (long)pos_, sub.begin(), sub.end());
  size_t savedPos = pos_;
  try { parseTopLevel(); } catch(...) {}
  // find instantiated function
  FuncDecl* fn = lookupFunc(tf.name, args);
  if (fn) return fn;
  return lookupFunc(tf.name);
}

void Parser::parseNamespace() {
  expect("namespace");
  std::string name;
  if (peek().kind == Tok::Ident) name = take().text;
  else name = "anon";
  std::string save = nsPrefix_;
  nsPrefix_ = qualify(name);
  expect("{");
  try {
    while (!at("}") && !atEof()) parseTopLevel();
    expect("}");
  } catch (...) {
    nsPrefix_ = save;
    throw;
  }
  nsPrefix_ = save;
}

void Parser::parseExternC() {
  expect("extern");
  std::string linkage = peek().kind == Tok::String ? take().text : "";
  bool save = externC_;
  bool isC = linkage.find("C") != std::string::npos && linkage.find("C++") == std::string::npos;
  externC_ = save || isC;
  auto skipDecl = [&]() {
    int depth = 0;
    while (!atEof()) {
      if (at("{")) { depth++; take(); continue; }
      if (at("}")) { if (depth==0) break; depth--; take(); continue; }
      if (at(";") && depth==0) { take(); break; }
      take();
    }
  };
  try {
    if (eat("{")) {
      while (!at("}") && !atEof()) {
        size_t savePos = pos_;
        try {
          parseTopLevel();
        } catch (const CompileError&) {
          if (pos_ == savePos) take();
          skipDecl();
        }
      }
      expect("}");
    } else {
      size_t savePos = pos_;
      try {
        parseDeclaration(true);
      } catch (const CompileError&) {
        if (pos_ == savePos) take();
        skipDecl();
      }
    }
  } catch (...) {
    externC_ = save;
    throw;
  }
  externC_ = save;
}

void Parser::parseUsing() {
  expect("using");
  if (eat("namespace")) {
    std::string name = expectIdent();
    while (eat("::")) name += "::" + expectIdent();
    expect(";");
    usingNamespaces_.push_back(qualify(name));
    return;
  }
  if (at("enum")) { take(); return; }
  std::string name = expectIdent();
  while (eat("::")) name += "::" + expectIdent();
  if (eat("=")) {
    Type* t = parseType();
    expect(";");
    typedefs_[qualify(name)] = t;
    return;
  }
  while (!at(";") && !atEof()) take();
  expect(";");
}

void Parser::parseTypedef() {
  expect("typedef");
  Type* base = parseType();
  if (eat(";")) return;
  while (true) {
    std::string name;
    Type* t = parseDeclarator(base, name);
    typedefs_[qualify(name)] = t;
    if (eat(",")) continue;
    break;
  }
  expect(";");
}

void Parser::parseEnum() {
  expect("enum");
  bool scoped = false;
  if (at("class") || at("struct")) {
    take();
    scoped = true;
  }
  std::string name;
  if (peek().kind == Tok::Ident) name = take().text;
  if (eat(":")) parseType();
  long long value = 0;
  if (eat("{")) {
    while (!at("}") && !atEof()) {
      std::string item = expectIdent();
      if (eat("=")) {
        ExprPtr e = parseAssign();
        value = evalConst(e.get());
      }
      std::string full = name.empty() ? item : qualify(name) + "::" + item;
      enumConsts_[full] = value;
      if (!scoped) enumConsts_[qualify(item)] = value;
      ++value;
      if (eat(",")) continue;
      break;
    }
    expect("}");
  }
  if (!name.empty()) typedefs_[qualify(name)] = prog_.types.intType();
  expect(";");
}

void Parser::parseStaticAssert() {
  expect("static_assert");
  skipBalanced("(", ")");
  expect(";");
}

void Parser::parseDeclaration(bool global) {
  bool isStatic = false;
  bool isVirtual = false;
  bool isExtern = false;
  while (true) {
    if (eat("static")) { isStatic = true; continue; }
    if (eat("virtual")) { isVirtual = true; continue; }
    if (eat("extern")) { isExtern = true; continue; }
    if (eat("inline") || eat("constexpr") || eat("explicit") || eat("register") ||
        eat("thread_local") || eat("mutable")) continue;
    if (eat("__extension__")) continue;
    if (at("__attribute__") || at("__attribute") || at("__declspec") || at("__asm__") || at("__asm")) { skipAttributes(); continue; }
    if (at("const") || at("volatile") || at("typename")) { take(); continue; }
    break;
  }

  Type* base = parseType();
  if (eat(";")) return;

  while (true) {
    SourceLoc loc = peek().loc;
    std::string name;
    Type* t = parseDeclarator(base, name);

    if (at("(")) {
      parseFunctionRest(t, name, loc, false, isStatic, isVirtual);
      return;
    }

    VarSym* v = newVar(name, t, loc);
    v->isGlobal = global;
    v->isStatic = isStatic;
    v->isExtern = isExtern;
    if (eat("=")) {
      if (at("{")) {
        take();
        while (!at("}") && !atEof()) {
          v->initList.push_back(parseAssign());
          if (!eat(",")) break;
        }
        expect("}");
      } else {
        v->init = parseAssign();
      }
    } else if (at("(")) {
      diag_.error(loc, "global objects with constructor arguments are not supported");
    } else if (at("{")) {
      take();
      while (!at("}") && !atEof()) {
        v->initList.push_back(parseAssign());
        if (!eat(",")) break;
      }
      expect("}");
    }

    declareVar(v);
    if (eat(",")) continue;
    break;
  }
  expect(";");
}

// ---------------------------------------------------------------------------
// Types and declarators
// ---------------------------------------------------------------------------

bool Parser::isTypeStart() const {
  const Token& t = peek();
  if (t.kind != Tok::Ident) return false;
  static const char* kws[] = {
    "const", "volatile", "unsigned", "signed", "short", "long", "void",
    "bool", "char", "int", "float", "double", "struct", "class", "union",
    "enum", "typename", "auto", "static", "extern", "register", "constexpr",
    "mutable", "thread_local", "size_t", "ssize_t", "ptrdiff_t", nullptr
  };
  for (int i = 0; kws[i]; ++i)
    if (t.text == kws[i]) return true;
  if (typedefs_.count(t.text) || structs_.count(t.text)) return true;
  return false;
}

Type* Parser::parseType() {
  bool isUnsigned = false;
  bool isSigned = false;
  int longCount = 0;
  bool isShort = false;
  bool isConst = false;
  (void)isConst;

  while (true) {
    if (at("const") || at("volatile") || at("typename") || at("register") ||
        at("mutable") || at("constexpr") || at("thread_local") || at("static") ||
        at("extern") || at("virtual") || at("inline") || at("explicit")) {
      take();
      continue;
    }
    if (eat("unsigned")) { isUnsigned = true; continue; }
    if (eat("signed")) { isSigned = true; continue; }
    if (eat("short")) { isShort = true; continue; }
    if (eat("long")) { ++longCount; continue; }
    break;
  }

  Type* base = nullptr;
  if (eat("void")) base = prog_.types.voidType();
  else if (eat("bool")) base = prog_.types.boolType();
  else if (eat("char")) base = prog_.types.builtin(TypeKind::Char, isUnsigned);
  else if (eat("int")) base = prog_.types.builtin(TypeKind::Int, isUnsigned);
  else if (eat("float")) base = prog_.types.builtin(TypeKind::Float);
  else if (eat("double")) base = prog_.types.builtin(TypeKind::Double);
  else if (eat("struct") || eat("class") || eat("union")) {
    std::string name = expectIdent();
    while (eat("::")) name += "::" + expectIdent();
    StructDecl* sd = lookupStruct(name);
    if (!sd) sd = getOrCreateStruct(qualify(name), peek().loc);
    base = structType(sd);
  } else if (eat("enum")) {
    if (peek().kind == Tok::Ident) take();
    base = prog_.types.intType();
  } else if (peek().kind == Tok::Ident && !at("__attribute__") && !at("__attribute") && !at("__declspec") && !at("__extension__")) {
    std::string name = peek().text;
    Type* t = lookupType(name);
    if (t) {
      take();
      while (eat("::")) name += "::" + expectIdent();
      base = lookupType(name) ? lookupType(name) : t;
    } else if (isUnsigned || isSigned || isShort || longCount > 0) {
      base = prog_.types.intType();
    } else {
      take();
      while (eat("::")) name += "::" + expectIdent();
      diag_.error(peek().loc, "unknown type name '" + name + "'");
    }
  } else if (at("__attribute__") || at("__attribute") || at("__declspec") || at("__extension__")) {
    skipAttributes();
    base = parseType();
  } else if (isUnsigned || isSigned || isShort || longCount > 0) {
    base = prog_.types.intType();
  } else {
    diag_.error(peek().loc, "expected a type");
  }

  if (isShort) {
    if (base->kind == TypeKind::Int || base->kind == TypeKind::Short)
      base = prog_.types.builtin(TypeKind::Short, isUnsigned);
  } else if (longCount > 0) {
    if (base->kind == TypeKind::Double) {
      // long double is treated as double
    } else if (longCount >= 2) {
      base = prog_.types.builtin(TypeKind::LongLong, isUnsigned);
    } else {
      base = prog_.types.builtin(TypeKind::Long, isUnsigned);
    }
  } else if (isUnsigned && base->kind == TypeKind::Int) {
    base = prog_.types.builtin(TypeKind::Int, true);
  }
  (void)isSigned;

  while (true) {
    if (eat("*")) { base = prog_.types.ptrTo(base); continue; }
    if (eat("&")) { base = prog_.types.refTo(base); continue; }
    if (at("const") || at("volatile") || at("__restrict") || at("__restrict__") ||
        at("restrict")) { take(); continue; }
    if (at("__attribute__") || at("__attribute") || at("__declspec")) { skipAttributes(); continue; }
    break;
  }
  return base;
}

Type* Parser::parseDeclarator(Type* base, std::string& name) {
  Type* t = base;
  while (true) {
    if (eat("*")) { t = prog_.types.ptrTo(t); continue; }
    if (eat("&")) { t = prog_.types.refTo(t); continue; }
    if (at("const") || at("volatile") || at("__restrict") || at("__restrict__") ||
        at("restrict")) { take(); continue; }
    if (at("__attribute__") || at("__attribute") || at("__declspec")) { skipAttributes(); continue; }
    break;
  }
  skipAttributes();
  if (at("(") && peek(1).text == "*")
    diag_.error(peek().loc, "function pointer declarators are not supported");
  if (at("__attribute__") || at("__attribute") || at("__declspec")) skipAttributes();
  name = expectIdent();
  if (name == "operator") {
    // operator overload name
    if (at("[")) { take(); expect("]"); name = "operator[]"; }
    else if (at("(")) { take(); expect(")"); name = "operator()"; }
    else { name = "operator" + take().text; }
  }
  while (eat("::")) name += "::" + expectIdent();
  skipAttributes();
  while (eat("[")) {
    long long n = 0;
    if (!at("]")) {
      ExprPtr e = parseExpression();
      n = evalConst(e.get());
    }
    expect("]");
    t = prog_.types.arrayOf(t, n);
  }
  return t;
}

// ---------------------------------------------------------------------------
// Functions and classes
// ---------------------------------------------------------------------------

std::vector<VarSym*> Parser::parseParams(bool& variadic) {
  variadic = false;
  expect("(");
  std::vector<VarSym*> params;
  if (eat(")")) return params;
  if (at("void") && peek(1).text == ")") {
    take();
    take();
    return params;
  }

  pushScope();
  int index = 0;
  while (true) {
    if (eat("...")) {
      variadic = true;
      break;
    }
    Type* t = parseType();
    std::string pname;
    if (peek().kind == Tok::Ident && !at(",") && !at(")") && !at("="))
      pname = take().text;
    while (eat("[")) {
      if (!at("]")) {
        ExprPtr e = parseExpression();
        (void)e;
      }
      expect("]");
      t = prog_.types.ptrTo(t);
    }
    if (pname.empty()) pname = "arg" + std::to_string(index);
    ++index;
    VarSym* v = newVar(pname, t, peek().loc);
    v->isParam = true;
    if (eat("=")) v->defaultArg = parseAssign();
    declareVar(v);
    params.push_back(v);
    if (eat(",")) continue;
    break;
  }
  popScope();
  expect(")");
  skipAttributes();
  return params;
}

StmtPtr Parser::parseFunctionBody(FuncDecl* fn) {
  pushScope();
  for (auto* p : fn->params) declareVar(p);
  try {
    StmtPtr body = parseBlock();
    popScope();
    return body;
  } catch (...) {
    popScope();
    throw;
  }
}

void Parser::parseFunctionRest(Type* ret, const std::string& name, SourceLoc loc,
                               bool isMethod, bool isStatic, bool isVirtual) {
  StructDecl* owner = nullptr;
  std::string qn;

  size_t sep = name.rfind("::");
  if (sep != std::string::npos) {
    std::string cls = name.substr(0, sep);
    std::string meth = name.substr(sep + 2);
    owner = lookupStruct(cls);
    if (!owner) diag_.error(loc, "unknown class '" + cls + "' in '" + name + "'");
    isMethod = true;
    qn = owner->name + "::" + meth;
  } else if (isMethod) {
    owner = currentClass_;
    qn = owner ? owner->name + "::" + name : qualify(name);
  } else {
    qn = qualify(name);
  }

  bool variadic = false;
  std::vector<VarSym*> params = parseParams(variadic);

  auto sameSig = [&](FuncDecl* f) {
    if (f->shortName != (isMethod ? (sep!=std::string::npos ? name.substr(sep+2) : name) : name)) return false;
    if (f->variadic != variadic) return false;
    if (f->params.size() != params.size()) return false;
    for (size_t i = 0; i < params.size(); ++i) {
      Type* a = f->params[i]->type;
      Type* b = params[i]->type;
      if (a && a->isRef) a = a->elem;
      if (b && b->isRef) b = b->elem;
      if (!sameType(a, b)) return false;
      bool ar = f->params[i]->type && f->params[i]->type->isRef;
      bool br = params[i]->type && params[i]->type->isRef;
      if (ar != br) return false;
    }
    return true;
  };

  FuncDecl* fn = nullptr;
  if (isMethod && owner) {
    for (auto* m : owner->findMethods(sep!=std::string::npos ? name.substr(sep+2) : name))
      if (sameSig(m)) { fn = m; break; }
  } else {
    auto r = funcs_.equal_range(qn);
    for (auto it = r.first; it != r.second; ++it)
      if (sameSig(it->second)) { fn = it->second; break; }
  }

  if (!fn) {
    if (sep != std::string::npos)
      diag_.error(loc, "no matching member function '" + name + "' declared in class");
    fn = newFunc(qn, ret, loc);
    fn->shortName = name;
    fn->isMethod = isMethod;
    fn->owner = owner;
    fn->isExternC = externC_;
    fn->isStatic = isStatic;
    fn->isVirtual = isVirtual;
    if (isMethod && owner) owner->methods.push_back(fn);
    registerFunc(fn);
  } else {
    fn->ret = ret;
    fn->isExternC = externC_ || fn->isExternC;
    fn->isStatic = fn->isStatic || isStatic;
  }

  fn->params = params;
  fn->variadic = variadic;

  if (eat("const")) {}
  if (eat("noexcept")) {
    if (at("(")) skipBalanced("(", ")");
  }
  if (eat("override") || eat("final")) {}
  if (eat("=")) {
    if (peek().text == "0") {
      take();
      expect(";");
      return;
    }
    if (at("default") || at("delete")) {
      take();
      expect(";");
      return;
    }
  }

  if (at("{")) {
    StructDecl* saveClass = currentClass_;
    FuncDecl* saveFunc = currentFunc_;
    if (isMethod && owner) currentClass_ = owner;
    currentFunc_ = fn;
    fn->body = parseFunctionBody(fn);
    fn->isDefined = true;
    currentClass_ = saveClass;
    currentFunc_ = saveFunc;
  } else {
    expect(";");
  }
}

void Parser::parseStruct() {
  bool isClass = at("class");
  take(); // struct/class/union
  skipAttributes();
  std::string name;
  if (peek().kind == Tok::Ident && !at("__attribute__") && !at("__attribute")) name = take().text;
  if (name.empty()) name = "anon_" + std::to_string(prog_.structs.size());
  std::string full = qualify(name);

  StructDecl* sd = getOrCreateStruct(full, peek().loc);
  if (eat(";")) return;

  if (eat(":")) {
    while (at("public") || at("private") || at("protected") || at("virtual")) take();
    std::string baseName = expectIdent();
    while (eat("::")) baseName += "::" + expectIdent();
    StructDecl* base = lookupStruct(baseName);
    if (!base) diag_.error(peek().loc, "unknown base class '" + baseName + "'");
    sd->base = base;
    if (base->hasVtable) sd->hasVtable = true;
    // flatten base fields
    if (sd->fields.empty()) {
      for (auto& f : base->fields) sd->fields.push_back(Field{f.name, f.type, nullptr, f.loc});
    }
    if (eat(",")) { while(!at("{") && !atEof()) take(); }
  }
  expect("{");

  StructDecl* saveClass = currentClass_;
  currentClass_ = sd;
  std::vector<std::pair<FuncDecl*, size_t>> bodies;
  try {
    while (!at("}") && !atEof()) {
      if (eat(";")) continue;
      if (at("public") || at("private") || at("protected")) {
        take();
        expect(":");
        continue;
      }
      if (at("template")) { diag_.warning(peek().loc, "skipping template in struct"); take(); if(at("<")) skipBalanced("<",">"); int d=0; while(!atEof()){ if(at("{")){d++;take();} else if(at("}")){take(); if(d>0)d--; if(d==0)break;} else if(at(";")&&d==0){take();break;} else take(); } continue; }
      if (at("virtual") && peek(1).text == "~") { take(); parseDtor(sd, bodies, true); continue; }
    if (at("~")) { parseDtor(sd, bodies, false); continue; }
      if (peek().kind == Tok::Ident && peek().text == name && peek(1).text == "(") {
        parseCtor(sd, bodies);
        continue;
      }
      if (at("using")) { parseUsing(); continue; }
      if (at("struct") || at("class") || at("enum")) {
        diag_.warning(peek().loc, "skipping nested type");
        take(); if(peek().kind==Tok::Ident) take(); if(at("{")) skipBalanced("{","}"); eat(";"); continue;
      }
      parseMember(sd, bodies);
    }
    expect("}");
  } catch (...) {
    currentClass_ = saveClass;
    throw;
  }

  // vtable setup
  if (sd->base && sd->base->hasVtable) {
    sd->hasVtable = true;
    sd->vtable = sd->base->vtable;
  }
  for (auto* m : sd->methods) {
    if (m->isVirtual) sd->hasVtable = true;
  }
  if (sd->hasVtable) {
    for (auto* m : sd->methods) {
      if (!m->isVirtual) {
        FuncDecl* baseM = sd->base ? sd->base->findMethod(m->shortName) : nullptr;
        if (m->isDtor && sd->base && sd->base->dtor && sd->base->dtor->isVirtual) baseM = sd->base->dtor;
        if (baseM && baseM->isVirtual) m->isVirtual = true;
      }
    }
    for (auto* m : sd->methods) {
      if (!m->isVirtual) continue;
      FuncDecl* baseM = sd->base ? sd->base->findMethod(m->shortName) : nullptr;
      if (m->isDtor && sd->base && sd->base->dtor && sd->base->dtor->isVirtual) baseM = sd->base->dtor;
      if (baseM && baseM->vslot >= 0) {
        m->vslot = baseM->vslot;
        if (m->vslot < (int)sd->vtable.size()) sd->vtable[m->vslot] = m;
      } else {
        m->vslot = (int)sd->vtable.size();
        sd->vtable.push_back(m);
      }
    }
    if (!(sd->base && sd->base->hasVtable)) {
      // save init field names before insert invalidates pointers
      std::vector<std::pair<FuncDecl*, std::vector<std::string>>> initNames;
      for (auto* m : sd->methods) {
        std::vector<std::string> names;
        for (auto& ii : m->initList) names.push_back(ii.field ? ii.field->name : "");
        initNames.push_back({m, std::move(names)});
      }
      Field vp;
      vp.name = "__vptr";
      vp.type = prog_.types.ptrTo(prog_.types.ptrTo(prog_.types.voidType()));
      vp.loc = sd->loc;
      sd->fields.insert(sd->fields.begin(), std::move(vp));
      for (auto& p : initNames) {
        auto* m = p.first;
        for (size_t i=0; i<m->initList.size() && i<p.second.size(); ++i) {
          if (!p.second[i].empty()) m->initList[i].field = sd->findField(p.second[i]);
        }
      }
    }
  }

  sd->complete = true;

  expect(";");

  currentClass_ = saveClass;

  size_t save = pos_;
  for (auto& br : bodies) {
    pos_ = br.second;
    StructDecl* sc = currentClass_;
    FuncDecl* sf = currentFunc_;
    currentClass_ = sd;
    currentFunc_ = br.first;
    br.first->body = parseFunctionBody(br.first);
    br.first->isDefined = true;
    currentClass_ = sc;
    currentFunc_ = sf;
  }
  pos_ = save;
  (void)isClass;
}

void Parser::parseCtor(StructDecl* sd, std::vector<std::pair<FuncDecl*, size_t>>& bodies) {
  SourceLoc loc = peek().loc;
  std::string name = expectIdent();
  FuncDecl* fn = newFunc(sd->name + "::" + name, prog_.types.voidType(), loc);
  fn->shortName = name;
  fn->isMethod = true;
  fn->isCtor = true;
  fn->owner = sd;
  sd->ctors.push_back(fn);
  sd->methods.push_back(fn);
  registerFunc(fn);

  bool variadic = false;
  fn->params = parseParams(variadic);
  fn->variadic = variadic;

  pushScope();
  for (auto* p : fn->params) declareVar(p);
  if (eat(":")) {
    while (true) {
      std::string fname = expectIdent();
      Field* f = sd->findField(fname);
      if (!f) {
        // base class init?
        StructDecl* base = sd->base;
        bool isBase = base && (fname == base->name || fname == base->name.substr(base->name.rfind("::")+1));
        if (isBase) {
          fn->baseInit = base;
          if (eat("(")) {
            while (!at(")") && !atEof()) { fn->baseInitArgs.push_back(parseExpression()); if(!eat(",")) break; }
            expect(")");
          } else if (eat("{")) {
            while (!at("}") && !atEof()) { fn->baseInitArgs.push_back(parseExpression()); if(!eat(",")) break; }
            expect("}");
          }
          if (eat(",")) continue;
          break;
        }
        diag_.error(peek().loc, "unknown field '" + fname + "' in constructor");
      }
      ExprPtr val;
      if (eat("(")) {
        val = parseExpression();
        expect(")");
      } else if (eat("{")) {
        val = parseExpression();
        expect("}");
      } else {
        diag_.error(peek().loc, "expected initializer for field '" + fname + "'");
      }
      fn->initList.push_back(InitItem{f, std::move(val)});
      if (eat(",")) continue;
      break;
    }
  }
  popScope();

  if (at("{")) {
    size_t start = pos_;
    skipBalanced("{", "}");
    bodies.push_back({fn, start});
  } else {
    expect(";");
  }
}

void Parser::parseDtor(StructDecl* sd, std::vector<std::pair<FuncDecl*, size_t>>& bodies, bool isVirtual) {
  SourceLoc loc = peek().loc;
  expect("~");
  std::string name = expectIdent();
  if (name != sd->name && sd->name.find("::" + name) == std::string::npos)
    diag_.error(loc, "destructor name does not match class");
  FuncDecl* fn = newFunc(sd->name + "::~" + name, prog_.types.voidType(), loc);
  fn->shortName = "~" + name;
  fn->isMethod = true;
  fn->isDtor = true;
  fn->isVirtual = isVirtual;
  fn->owner = sd;
  if (sd->dtor) diag_.error(loc, "destructor already declared");
  sd->dtor = fn;
  sd->methods.push_back(fn);
  registerFunc(fn);
  expect("(");
  expect(")");
  if (at("{")) {
    size_t start = pos_;
    skipBalanced("{", "}");
    bodies.push_back({fn, start});
  } else {
    expect(";");
  }
}

void Parser::parseMember(StructDecl* sd,
                         std::vector<std::pair<FuncDecl*, size_t>>& bodies) {
  bool isStatic = false;
  bool isVirtual = false;
  bool isFriend = false;
  while (true) {
    if (eat("static")) { isStatic = true; continue; }
    if (eat("virtual")) { isVirtual = true; continue; }
    if (eat("inline") || eat("constexpr") || eat("explicit") || eat("mutable")) continue;
    if (eat("friend")) { isFriend = true; continue; }
    if (at("const") || at("volatile") || at("typename") || at("register") ||
        at("thread_local")) { take(); continue; }
    break;
  }
  if (isFriend) {
    while (!at(";") && !atEof()) take();
    expect(";");
    return;
  }

  Type* base = parseType();
  if (eat(";")) return;

  SourceLoc loc = peek().loc;
  std::string name;
  Type* t = parseDeclarator(base, name);
  if (at("(")) {
    parseMemberMethod(sd, t, name, loc, isStatic, isVirtual, bodies);
    return;
  }
  if (isStatic) diag_.error(loc, "static data members are not supported");

  // Field list.
  while (true) {
    if (sd->findField(name))
      diag_.error(loc, "duplicate field '" + name + "'");
    Field f;
    f.name = name;
    f.type = t;
    f.loc = loc;
    if (t->isRef) diag_.error(loc, "reference fields are not supported");
    if (eat("=")) f.defaultInit = parseAssign();
    else if (at("{")) {
      take();
      if (!at("}")) f.defaultInit = parseAssign();
      expect("}");
    }
    if (eat(":")) {
      ExprPtr bits = parseAssign();
      (void)bits;
    }
    sd->fields.push_back(std::move(f));
    if (eat(",")) {
      name.clear();
      t = parseDeclarator(base, name);
      loc = peek().loc;
      continue;
    }
    break;
  }
  expect(";");
}

void Parser::parseMemberMethod(StructDecl* sd, Type* ret, const std::string& name,
                               SourceLoc loc, bool isStatic, bool isVirtual,
                               std::vector<std::pair<FuncDecl*, size_t>>& bodies) {
  FuncDecl* fn = newFunc(sd->name + "::" + name, ret, loc);
  fn->shortName = name;
  fn->isMethod = true;
  fn->owner = sd;
  fn->isStatic = isStatic;
  fn->isVirtual = isVirtual;
  sd->methods.push_back(fn);
  registerFunc(fn);

  bool variadic = false;
  fn->params = parseParams(variadic);
  fn->variadic = variadic;

  if (eat("const")) {}
  if (eat("noexcept")) {
    if (at("(")) skipBalanced("(", ")");
  }
  if (eat("override") || eat("final")) {}
  if (eat("=")) {
    if (peek().text == "0") { expect(";"); return; }
    if (at("default") || at("delete")) {
      take();
      expect(";");
      return;
    }
  }
  if (at("{")) {
    size_t start = pos_;
    skipBalanced("{", "}");
    bodies.push_back({fn, start});
  } else {
    expect(";");
  }
}

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

StmtPtr Parser::parseStatement() {
  SourceLoc loc = peek().loc;
  if (at("{")) return parseBlock();
  if (eat(";")) {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Empty;
    s->loc = loc;
    return s;
  }
  if (at("if")) return parseIf();
  if (at("while")) return parseWhile();
  if (at("do")) return parseDoWhile();
  if (at("for")) return parseFor();
  if (at("switch")) return parseSwitch();
  if (at("case")) return parseCase(false);
  if (at("default")) return parseCase(true);
  if (at("return")) return parseReturn();
  if (at("break")) {
    take();
    expect(";");
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Break;
    s->loc = loc;
    return s;
  }
  if (at("continue")) {
    take();
    expect(";");
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Continue;
    s->loc = loc;
    return s;
  }
  if (at("goto")) {
    take();
    std::string name = expectIdent();
    expect(";");
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Goto;
    s->label = name;
    s->loc = loc;
    return s;
  }
  if (at("try") || at("catch") || at("throw"))
    diag_.error(loc, "exceptions are not supported");
  if (at("using")) {
    parseUsing();
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Empty;
    s->loc = loc;
    return s;
  }
  if (isCoutStart()) return parseCoutStmt();
  if (peek().kind == Tok::Ident && peek(1).text == ":" && !at("case") && !at("default")) {
    std::string name = take().text;
    take(); // :
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Label;
    s->label = name;
    s->loc = loc;
    s->kids.push_back(parseStatement());
    return s;
  }
  if (isTypeStart()) return parseDeclStmt();

  ExprPtr e = parseExpression();
  expect(";");
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::Expr;
  s->expr = std::move(e);
  s->loc = loc;
  return s;
}

StmtPtr Parser::parseBlock() {
  SourceLoc loc = peek().loc;
  expect("{");
  pushScope();
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::Block;
  s->loc = loc;
  while (!at("}") && !atEof()) {
    size_t save = pos_;
    try {
      s->kids.push_back(parseStatement());
    } catch (const CompileError&) {
      if (pos_ == save) take();
      int depth = 0;
      while (!atEof()) {
        if (at("{")) { depth++; take(); continue; }
        if (at("}")) { if (depth==0) break; depth--; take(); continue; }
        if (at(";") && depth==0) { take(); break; }
        take();
      }
    }
  }
  expect("}");
  popScope();
  return s;
}

StmtPtr Parser::parseIf() {
  SourceLoc loc = peek().loc;
  expect("if");
  expect("(");
  ExprPtr cond = parseExpression();
  expect(")");
  StmtPtr body = parseStatement();
  StmtPtr elseBody;
  if (eat("else")) elseBody = parseStatement();
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::If;
  s->cond = std::move(cond);
  s->body = std::move(body);
  s->elseBody = std::move(elseBody);
  s->loc = loc;
  return s;
}

StmtPtr Parser::parseWhile() {
  SourceLoc loc = peek().loc;
  expect("while");
  expect("(");
  ExprPtr cond = parseExpression();
  expect(")");
  StmtPtr body = parseStatement();
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::While;
  s->cond = std::move(cond);
  s->body = std::move(body);
  s->loc = loc;
  return s;
}

StmtPtr Parser::parseDoWhile() {
  SourceLoc loc = peek().loc;
  expect("do");
  StmtPtr body = parseStatement();
  expect("while");
  expect("(");
  ExprPtr cond = parseExpression();
  expect(")");
  expect(";");
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::DoWhile;
  s->cond = std::move(cond);
  s->body = std::move(body);
  s->loc = loc;
  return s;
}

StmtPtr Parser::parseFor() {
  SourceLoc loc = peek().loc;
  expect("for");
  expect("(");
  pushScope();
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::For;
  s->loc = loc;
  if (eat(";")) {
    // no init
  } else if (isTypeStart()) {
    s->initStmt = parseDeclStmt();
  } else {
    ExprPtr init = parseExpression();
    expect(";");
    auto is = std::make_unique<Stmt>();
    is->kind = StmtKind::Expr;
    is->expr = std::move(init);
    is->loc = loc;
    s->initStmt = std::move(is);
  }
  if (!at(";")) s->cond = parseExpression();
  expect(";");
  if (!at(")")) s->step = parseExpression();
  expect(")");
  s->body = parseStatement();
  popScope();
  return s;
}

StmtPtr Parser::parseSwitch() {
  SourceLoc loc = peek().loc;
  expect("switch");
  expect("(");
  ExprPtr e = parseExpression();
  expect(")");
  StmtPtr body = parseStatement();
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::Switch;
  s->expr = std::move(e);
  s->body = std::move(body);
  s->loc = loc;
  return s;
}

StmtPtr Parser::parseCase(bool isDefault) {
  SourceLoc loc = peek().loc;
  auto s = std::make_unique<Stmt>();
  s->loc = loc;
  if (isDefault) {
    expect("default");
    s->kind = StmtKind::Default;
  } else {
    expect("case");
    ExprPtr e = parseExpression();
    s->caseValue = evalConst(e.get());
    s->kind = StmtKind::Case;
  }
  expect(":");
  while (!at("case") && !at("default") && !at("}") && !atEof())
    s->kids.push_back(parseStatement());
  return s;
}

StmtPtr Parser::parseReturn() {
  SourceLoc loc = peek().loc;
  expect("return");
  ExprPtr e;
  if (!at(";")) e = parseExpression();
  expect(";");
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::Return;
  s->expr = std::move(e);
  s->loc = loc;
  return s;
}

bool Parser::isCoutStart() const {
  size_t i = pos_;
  if (i < toks_.size() && toks_[i].kind == Tok::Ident && toks_[i].text == "std" &&
      i+2 < toks_.size() && toks_[i+1].text == "::" && toks_[i+2].kind == Tok::Ident &&
      (toks_[i+2].text == "cout" || toks_[i+2].text == "cerr")) i += 3;
  else if (i < toks_.size() && toks_[i].kind == Tok::Ident &&
           (toks_[i].text == "cout" || toks_[i].text == "cerr")) i += 1;
  else return false;
  return i < toks_.size() && toks_[i].text == "<<";
}

StmtPtr Parser::parseCoutStmt() {
  SourceLoc loc = peek().loc;
  // consume cout/cerr
  if (atIdent("std")) { take(); eat("::"); }
  bool isCerr = atIdent("cerr");
  take(); // cout/cerr

  auto makeCallStmt = [&](ExprPtr call) {
    auto s = std::make_unique<Stmt>();
    s->kind = StmtKind::Expr;
    s->expr = std::move(call);
    s->loc = loc;
    return s;
  };

  FuncDecl* printfFn = lookupFunc("printf");
  if (!printfFn) {
    // register a fallback
    printfFn = newFunc("printf", prog_.types.intType(), loc);
    printfFn->isExternC = true;
    printfFn->variadic = true;
    VarSym* p = newVar("fmt", prog_.types.ptrTo(prog_.types.charType()), loc);
    p->isParam = true;
    printfFn->params.push_back(p);
    registerFunc(printfFn);
  }

  auto block = std::make_unique<Stmt>();
  block->kind = StmtKind::Block;
  block->loc = loc;

  while (eat("<<")) {
    // handle std::endl
    if ((atIdent("std") && peek(1).text == "::" && peek(2).kind == Tok::Ident && peek(2).text == "endl") || atIdent("endl")) {
      if (atIdent("std")) { take(); eat("::"); }
      take(); // endl
      auto e = makeExpr(ExprKind::Call, loc);
      e->func = printfFn;
      e->type = printfFn->ret;
      auto fmt = makeExpr(ExprKind::Str, loc);
      fmt->op = "\"\\n\"";
      fmt->type = prog_.types.ptrTo(prog_.types.charType());
      e->args.push_back(std::move(fmt));
      block->kids.push_back(makeCallStmt(std::move(e)));
      continue;
    }

    ExprPtr val = parseBinary(9);
    Type* t = val->type;
    std::string fmtStr = "%d";
    bool needCast = false;
    if (t) {
      Type* bt = t->isRef ? t->elem : t;
      if (bt->kind == TypeKind::Ptr && bt->elem && bt->elem->kind == TypeKind::Char) fmtStr = "%s";
      else if (bt->kind == TypeKind::Bool) { fmtStr = "%d"; }
      else if (bt->kind == TypeKind::Char) fmtStr = "%c";
      else if (bt->kind == TypeKind::Float || bt->kind == TypeKind::Double) fmtStr = "%g";
      else if (bt->kind == TypeKind::LongLong) { fmtStr = "%lld"; needCast = true; }
      else if (bt->kind == TypeKind::Long) { fmtStr = "%ld"; needCast = true; }
      else if (bt->isUnsigned) { fmtStr = "%u"; }
      else fmtStr = "%d";
      if (bt->kind == TypeKind::Ptr && bt->elem && bt->elem->kind == TypeKind::Char) needCast = false;
    }
    auto e = makeExpr(ExprKind::Call, loc);
    e->func = printfFn;
    e->type = printfFn->ret;
    auto fmt = makeExpr(ExprKind::Str, loc);
    fmt->op = "\"" + fmtStr + "\"";
    fmt->type = prog_.types.ptrTo(prog_.types.charType());
    e->args.push_back(std::move(fmt));
    if (needCast) {
      auto c = makeExpr(ExprKind::Cast, loc);
      c->type = t;
      c->targetType = t;
      c->args.push_back(std::move(val));
      e->args.push_back(std::move(c));
    } else {
      e->args.push_back(std::move(val));
    }
    block->kids.push_back(makeCallStmt(std::move(e)));
  }
  expect(";");
  (void)isCerr;
  return block;
}

StmtPtr Parser::parseDeclStmt() {
  SourceLoc loc = peek().loc;
  bool isStatic = false;
  while (eat("static") || eat("register") || eat("thread_local")) { isStatic = true; }
  bool isAuto = at("auto");
  Type* base = isAuto ? prog_.types.intType() : parseType();
  if (isAuto) take();
  auto s = std::make_unique<Stmt>();
  s->kind = StmtKind::Decl;
  s->loc = loc;
  if (eat(";")) return s;

  while (true) {
    std::string name;
    Type* t = parseDeclarator(base, name);
    VarSym* v = newVar(name, t, loc);
    v->isStatic = isStatic;
    DeclItem item;
    item.var = v;

    if (eat("=")) {
      if (at("{")) {
        item.isListInit = true;
        take();
        while (!at("}") && !atEof()) {
          item.list.push_back(parseAssign());
          if (!eat(",")) break;
        }
        expect("}");
      } else {
        item.init = parseAssign();
      }
    } else if (at("(")) {
      item.isCtor = true;
      item.hasCtorArgs = true;
      item.ctorArgs = parseCallArgs();
    } else if (at("{")) {
      item.isListInit = true;
      take();
      while (!at("}") && !atEof()) {
        item.list.push_back(parseAssign());
        if (!eat(",")) break;
      }
      expect("}");
    }

    if (isAuto) {
      if (item.init) t = item.init->type ? item.init->type : t;
      else if (item.isListInit && !item.list.empty() && item.list[0]->type) t = item.list[0]->type;
      v->type = t;
      item.var->type = t;
    }
    if (t->isRef && !item.init)
      diag_.error(loc, "reference '" + name + "' must be initialized");
    if (t->kind == TypeKind::Array && t->arrayLen == 0 && item.isListInit)
      t->arrayLen = static_cast<long long>(item.list.size());

    declareVar(v);
    s->decls.push_back(std::move(item));
    if (eat(",")) continue;
    break;
  }
  expect(";");
  return s;
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

static ExprPtr cloneExpr(const Expr* e) {
  if (!e) return nullptr;
  auto c = std::make_unique<Expr>();
  c->kind = e->kind;
  c->type = e->type;
  c->lvalue = e->lvalue;
  c->op = e->op;
  c->ival = e->ival;
  c->fval = e->fval;
  c->loc = e->loc;
  c->var = e->var;
  c->func = e->func;
  c->sdecl = e->sdecl;
  c->field = e->field;
  c->implicitThis = e->implicitThis;
  c->arrow = e->arrow;
  c->isArrayNew = e->isArrayNew;
  c->globalScope = e->globalScope;
  for (const auto& a : e->args) c->args.push_back(cloneExpr(a.get()));
  c->self = cloneExpr(e->self.get());
  return c;
}

ExprPtr Parser::makeExpr(ExprKind k, SourceLoc loc) {
  auto e = std::make_unique<Expr>();
  e->kind = k;
  e->loc = loc;
  return e;
}

ExprPtr Parser::makeInt(long long v, SourceLoc loc) {
  auto e = makeExpr(ExprKind::Int, loc);
  e->ival = v;
  e->type = prog_.types.intType();
  return e;
}

ExprPtr Parser::makeUnary(const std::string& op, ExprPtr operand, SourceLoc loc) {
  auto e = makeExpr(ExprKind::Unary, loc);
  e->op = op;
  Type* t = operand->type;
  if (op == "!") {
    e->type = prog_.types.boolType();
  } else if (op == "*") {
    Type* p = pointeeOf(t);
    if (!p) diag_.error(loc, "cannot dereference a non-pointer");
    e->type = p;
    e->lvalue = true;
  } else if (op == "&") {
    Type* target = t && t->isRef ? t->elem : t;
    e->type = prog_.types.ptrTo(target);
  } else {
    e->type = t;
    if (op == "++" || op == "--") e->lvalue = true;
  }
  e->args.push_back(std::move(operand));
  return e;
}

static int binaryPrec(const std::string& op) {
  if (op == "||") return 1;
  if (op == "&&") return 2;
  if (op == "|") return 3;
  if (op == "^") return 4;
  if (op == "&") return 5;
  if (op == "==" || op == "!=") return 6;
  if (op == "<" || op == "<=" || op == ">" || op == ">=") return 7;
  if (op == "<<" || op == ">>") return 8;
  if (op == "+" || op == "-") return 9;
  if (op == "*" || op == "/" || op == "%") return 10;
  return 0;
}

ExprPtr Parser::makeBinary(const std::string& op, ExprPtr a, ExprPtr b, SourceLoc loc) {
  auto e = makeExpr(ExprKind::Binary, loc);
  e->op = op;
  if (op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" ||
      op == ">=" || op == "&&" || op == "||") {
    e->type = prog_.types.boolType();
  } else if (op == "<<" || op == ">>") {
    e->type = a->type;
  } else if (isPointerLike(a->type) && !isPointerLike(b->type)) {
    e->type = a->type;
  } else if (!isPointerLike(a->type) && isPointerLike(b->type) &&
             (op == "+" || op == "-")) {
    e->type = b->type;
  } else {
    e->type = usualType(a->type, b->type);
  }
  e->args.push_back(std::move(a));
  e->args.push_back(std::move(b));
  return e;
}

Type* Parser::usualType(Type* a, Type* b) {
  if (!a) return b;
  if (!b) return a;
  if (a->kind == TypeKind::Double || b->kind == TypeKind::Double)
    return prog_.types.doubleType();
  if (a->kind == TypeKind::Float || b->kind == TypeKind::Float)
    return prog_.types.builtin(TypeKind::Float);
  if (isPointerLike(a)) return a;
  if (isPointerLike(b)) return b;
  if (a->kind == TypeKind::LongLong || b->kind == TypeKind::LongLong)
    return prog_.types.builtin(TypeKind::LongLong,
                               a->isUnsigned || b->isUnsigned);
  if (a->kind == TypeKind::Long || b->kind == TypeKind::Long)
    return prog_.types.builtin(TypeKind::Long, a->isUnsigned || b->isUnsigned);
  return prog_.types.builtin(TypeKind::Int, a->isUnsigned || b->isUnsigned);
}

ExprPtr Parser::parseExpression() {
  ExprPtr e = parseAssign();
  while (eat(",")) {
    ExprPtr rhs = parseAssign();
    auto c = makeExpr(ExprKind::Comma, e->loc);
    c->type = rhs->type;
    c->args.push_back(std::move(e));
    c->args.push_back(std::move(rhs));
    e = std::move(c);
  }
  return e;
}

ExprPtr Parser::parseAssign() {
  ExprPtr lhs = parseTernary();
  std::string op = peek().text;
  if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" ||
      op == "%=" || op == "<<=" || op == ">>=" || op == "&=" || op == "|=" ||
      op == "^=") {
    SourceLoc loc = peek().loc;
    take();
    ExprPtr rhs = parseAssign();
    if (!lhs->lvalue) diag_.error(loc, "left-hand side of assignment is not an lvalue");
    auto e = makeExpr(ExprKind::Assign, loc);
    e->op = op;
    e->type = lhs->type;
    e->args.push_back(std::move(lhs));
    e->args.push_back(std::move(rhs));
    return e;
  }
  return lhs;
}

ExprPtr Parser::parseTernary() {
  ExprPtr cond = parseBinary(1);
  if (eat("?")) {
    ExprPtr a = parseAssign();
    expect(":");
    ExprPtr b = parseAssign();
    auto e = makeExpr(ExprKind::Ternary, cond->loc);
    e->type = usualType(a->type, b->type);
    e->args.push_back(std::move(cond));
    e->args.push_back(std::move(a));
    e->args.push_back(std::move(b));
    return e;
  }
  return cond;
}

ExprPtr Parser::parseBinary(int minPrec) {
  ExprPtr lhs = parseUnary();
  while (true) {
    std::string op = peek().text;
    int prec = binaryPrec(op);
    if (prec < minPrec || prec == 0) break;
    SourceLoc loc = peek().loc;
    take();
    ExprPtr rhs = parseBinary(prec + 1);
    // operator overload?
    Type* lt = lhs->type;
    if (lt && lt->isRef) lt = lt->elem;
    if (lt && lt->kind == TypeKind::Struct) {
      std::string opName = "operator" + op;
      auto methods = lt->sdecl->findMethods(opName);
      if (!methods.empty()) {
        std::vector<ExprPtr> args;
        args.push_back(std::move(rhs));
        FuncDecl* fn = nullptr; int bestScore=1<<30; bool ambig=false;
        for (auto* m : methods) { int sc=overloadScore(m,args); if(sc<0) continue; if(!fn||sc<bestScore){fn=m;bestScore=sc;ambig=false;} else if(sc==bestScore&&fn!=m) ambig=true; }
        if (fn) { lhs = parseCall(fn, std::move(lhs), false, false, loc, std::move(args)); continue; }
      }
    }
    lhs = makeBinary(op, std::move(lhs), std::move(rhs), loc);
  }
  return lhs;
}

ExprPtr Parser::parseUnary() {
  SourceLoc loc = peek().loc;
  if (at("++") || at("--")) {
    std::string op = take().text;
    ExprPtr operand = parseUnary();
    if (!operand->lvalue) diag_.error(loc, "operand of " + op + " is not an lvalue");
    return makeUnary(op, std::move(operand), loc);
  }
  if (at("+") || at("-") || at("!") || at("~")) {
    std::string op = take().text;
    ExprPtr operand = parseUnary();
    return makeUnary(op, std::move(operand), loc);
  }
  if (at("*") || at("&")) {
    std::string op = take().text;
    ExprPtr operand = parseUnary();
    if (op == "*" && !isPointerLike(operand->type))
      diag_.error(loc, "cannot dereference a non-pointer");
    if (op == "&" && !operand->lvalue && operand->kind != ExprKind::Ident)
      diag_.error(loc, "cannot take the address of an rvalue");
    return makeUnary(op, std::move(operand), loc);
  }
  if (at("sizeof")) return parseSizeof();
  if (at("new")) return parseNew();
  if (at("delete")) return parseDelete();
  if (at("static_cast") || at("reinterpret_cast") || at("const_cast") ||
      at("dynamic_cast")) {
    std::string kind = take().text;
    expect("<");
    Type* t = parseType();
    expect(">");
    expect("(");
    ExprPtr operand = parseExpression();
    expect(")");
    auto e = makeExpr(ExprKind::Cast, loc);
    e->type = t;
    e->args.push_back(std::move(operand));
    if (kind == "dynamic_cast")
      diag_.error(loc, "dynamic_cast requires RTTI, which is not supported");
    return e;
  }
  return parsePostfix();
}

ExprPtr Parser::parseSizeof() {
  SourceLoc loc = peek().loc;
  expect("sizeof");
  if (at("(")) {
    size_t save = pos_;
    take();
    if (isTypeStart()) {
      Type* t = parseType();
      if (eat(")")) {
        auto e = makeExpr(ExprKind::Sizeof, loc);
        e->type = prog_.types.sizeType();
        e->targetType = t;
        e->sdecl = t->kind == TypeKind::Struct ? t->sdecl : nullptr;
        e->ival = typeSizeOf(t);
        e->op = "type";
        return e;
      }
    }
    pos_ = save;
  }
  ExprPtr operand = parseUnary();
  auto e = makeExpr(ExprKind::Sizeof, loc);
  e->type = prog_.types.sizeType();
  e->args.push_back(std::move(operand));
  return e;
}

ExprPtr Parser::parseNew() {
  SourceLoc loc = peek().loc;
  expect("new");
  if (at("(")) diag_.error(loc, "placement new is not supported");
  Type* t = parseType();
  auto e = makeExpr(ExprKind::New, loc);
  e->sdecl = t->kind == TypeKind::Struct ? t->sdecl : nullptr;
  e->type = prog_.types.ptrTo(t);

  if (eat("[")) {
    e->isArrayNew = true;
    e->args.push_back(parseExpression());
    expect("]");
    return e;
  }
  if (at("(")) e->args = parseCallArgs();

  if (t->kind == TypeKind::Struct) {
    StructDecl* sd = t->sdecl;
    if (!sd->ctors.empty()) {
      FuncDecl* best = nullptr; int bestScore = 1<<30; bool ambig=false;
      for (auto* c : sd->ctors) { int sc=overloadScore(c,e->args); if(sc<0) continue; if(!best||sc<bestScore){best=c;bestScore=sc;ambig=false;} else if(sc==bestScore&&best!=c) ambig=true; }
      if (!best) diag_.error(loc, "no matching constructor for '" + sd->name + "'");
      if (ambig) diag_.error(loc, "ambiguous constructor for '" + sd->name + "'");
      e->func = best;
      size_t fixed = best->params.size();
      for (size_t i = e->args.size(); i < fixed; ++i) {
        if (!best->params[i]->defaultArg)
          diag_.error(loc, "constructor of '" + sd->name + "' requires more arguments");
        e->args.push_back(cloneExpr(best->params[i]->defaultArg.get()));
      }
    } else if (!e->args.empty()) {
      diag_.error(loc, "'" + sd->name + "' has no constructor");
    }
  } else if (!e->args.empty()) {
    diag_.error(loc, "arguments to new are only supported for class types");
  }
  return e;
}

ExprPtr Parser::parseDelete() {
  SourceLoc loc = peek().loc;
  expect("delete");
  bool array = false;
  if (eat("[")) {
    expect("]");
    array = true;
  }
  ExprPtr operand = parseUnary();
  auto e = makeExpr(ExprKind::Delete, loc);
  e->type = prog_.types.voidType();
  e->isArrayNew = array;
  e->args.push_back(std::move(operand));
  return e;
}

ExprPtr Parser::parsePostfix() {
  ExprPtr e = parsePrimary();
  while (true) {
    if (at("[")) {
      SourceLoc loc = peek().loc;
      take();
      ExprPtr idx = parseExpression();
      expect("]");
      Type* t = e->type;
      Type* elem = pointeeOf(t);
      if (!elem) diag_.error(loc, "subscript of a non-pointer");
      auto m = makeExpr(ExprKind::Index, loc);
      m->type = elem;
      m->lvalue = true;
      m->args.push_back(std::move(e));
      m->args.push_back(std::move(idx));
      e = std::move(m);
      continue;
    }
    if (at(".") || at("->")) {
      bool arrow = at("->");
      SourceLoc loc = peek().loc;
      take();
      std::string name = expectIdent();
      Type* baseType = e->type;
      Type* structType = nullptr;
      if (arrow) {
        structType = pointeeOf(baseType);
      } else {
        if (baseType && baseType->isRef) structType = baseType->elem;
        else if (baseType && baseType->kind == TypeKind::Struct) structType = baseType;
      }
      if (!structType || structType->kind != TypeKind::Struct)
        diag_.error(loc, "member access on a non-class type");
      StructDecl* sd = structType->sdecl;
      if (Field* f = sd->findField(name)) {
        auto m = makeExpr(ExprKind::Member, loc);
        m->type = f->type;
        m->field = f;
        m->arrow = arrow;
        m->lvalue = true;
        m->args.push_back(std::move(e));
        e = std::move(m);
        continue;
      }
      {
        auto methods = sd->findMethods(name);
        if (!methods.empty()) {
          if (!at("(")) diag_.error(loc, "method '" + name + "' must be called");
          std::vector<ExprPtr> args = parseCallArgs();
          FuncDecl* fn = nullptr;
          int bestScore = 1 << 30;
          bool ambig = false;
          for (auto* m : methods) {
            int sc = overloadScore(m, args);
            if (sc < 0) continue;
            if (!fn || sc < bestScore) { fn = m; bestScore = sc; ambig = false; }
            else if (sc == bestScore && fn != m) ambig = true;
          }
          if (!fn) fn = methods.front();
          if (ambig) diag_.error(loc, "ambiguous call to '" + name + "'");
          e = parseCall(fn, std::move(e), false, arrow, loc, std::move(args));
          continue;
        }
      }
      diag_.error(loc, "no member named '" + name + "' in '" + sd->name + "'");
    }
    if (at("++") || at("--")) {
      SourceLoc loc = peek().loc;
      std::string op = take().text;
      if (!e->lvalue) diag_.error(loc, "operand of " + op + " is not an lvalue");
      auto p = makeExpr(ExprKind::Postfix, loc);
      p->op = op;
      p->type = e->type;
      p->args.push_back(std::move(e));
      e = std::move(p);
      continue;
    }
    if (at("(")) {
      diag_.error(peek().loc, "calling a non-function expression is not supported");
    }
    break;
  }
  return e;
}

ExprPtr Parser::parsePrimary() {
  SourceLoc loc = peek().loc;
  if (peek().kind == Tok::Number) return parseNumber();
  if (peek().kind == Tok::String) return parseString();
  if (peek().kind == Tok::Char) return parseChar();

  if (at("(")) {
    size_t save = pos_;
    take();
    if (isTypeStart()) {
      Type* t = parseType();
      if (eat(")")) {
        ExprPtr operand = parseUnary();
        auto e = makeExpr(ExprKind::Cast, loc);
        e->type = t;
        e->args.push_back(std::move(operand));
        return e;
      }
    }
    pos_ = save;
    take();
    ExprPtr e = parseExpression();
    expect(")");
    return e;
  }
  if (at("::")) {
    take();
    ExprPtr e = parseIdentExpr();
    e->globalScope = true;
    return e;
  }
  if (peek().kind == Tok::Ident) return parseIdentExpr();
  diag_.error(loc, "unexpected token in expression: '" + peek().text + "'");
}

ExprPtr Parser::parseNumber() {
  Token t = take();
  std::string s = t.text;
  bool isHex = s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
  bool isFloat = s.find('.') != std::string::npos;
  if (!isHex) {
    for (char c : s) {
      if (c == 'e' || c == 'E') isFloat = true;
    }
  }
  for (char c : s) {
    if (c == 'p' || c == 'P') isFloat = true;
  }
  if (isFloat) {
    FloatLiteral f = parseFloatLiteral(s);
    if (!f.ok) diag_.error(t.loc, "invalid floating literal: " + s);
    auto e = makeExpr(ExprKind::Float, t.loc);
    e->fval = f.value;
    e->type = f.isFloat ? prog_.types.builtin(TypeKind::Float) : prog_.types.doubleType();
    return e;
  }
  IntLiteral i = parseIntLiteral(s);
  if (!i.ok) diag_.error(t.loc, "invalid integer literal: " + s);
  auto e = makeExpr(ExprKind::Int, t.loc);
  e->ival = static_cast<long long>(i.value);
  if (i.isLongLong)
    e->type = prog_.types.builtin(TypeKind::LongLong, i.isUnsigned);
  else if (i.isLong)
    e->type = prog_.types.builtin(TypeKind::Long, i.isUnsigned);
  else if (i.isUnsigned || i.value > 0x7fffffffULL)
    e->type = i.isUnsigned ? prog_.types.builtin(TypeKind::Int, true)
                           : prog_.types.builtin(TypeKind::LongLong);
  else
    e->type = prog_.types.intType();
  return e;
}

ExprPtr Parser::parseString() {
  SourceLoc loc = peek().loc;
  std::string text;
  while (peek().kind == Tok::String) text += take().text;
  auto e = makeExpr(ExprKind::Str, loc);
  e->op = text;
  e->type = prog_.types.ptrTo(prog_.types.charType());
  return e;
}

ExprPtr Parser::parseChar() {
  Token t = take();
  bool ok = false;
  long long v = parseCharLiteral(t.text, ok);
  if (!ok) diag_.error(t.loc, "invalid character literal");
  auto e = makeExpr(ExprKind::Char, t.loc);
  e->ival = v;
  e->type = prog_.types.intType();
  return e;
}

std::vector<ExprPtr> Parser::parseCallArgs() {
  std::vector<ExprPtr> args;
  expect("(");
  if (!at(")")) {
    while (true) {
      args.push_back(parseAssign());
      if (eat(",")) continue;
      break;
    }
  }
  expect(")");
  return args;
}

ExprPtr Parser::parseCall(FuncDecl* fn, ExprPtr self, bool implicitThis,
                          bool arrow, SourceLoc loc, std::vector<ExprPtr> args) {
  auto e = makeExpr(ExprKind::Call, loc);
  e->func = fn;
  e->self = std::move(self);
  e->implicitThis = implicitThis;
  e->arrow = arrow;
  e->type = fn->ret;
  e->args = std::move(args);

  size_t fixed = fn->params.size();
  if (e->args.size() > fixed && !fn->variadic)
    diag_.error(loc, "too many arguments to '" + fn->name + "'");
  for (size_t i = e->args.size(); i < fixed; ++i) {
    if (!fn->params[i]->defaultArg)
      diag_.error(loc, "too few arguments to '" + fn->name + "'");
    e->args.push_back(cloneExpr(fn->params[i]->defaultArg.get()));
  }
  for (size_t i = 0; i < e->args.size() && i < fixed; ++i) {
    if (fn->params[i]->type && fn->params[i]->type->isRef && !e->args[i]->lvalue)
      diag_.error(e->args[i]->loc, "reference parameter requires an lvalue argument");
  }
  return e;
}

ExprPtr Parser::parseIdentExpr() {
  SourceLoc loc = peek().loc;
  std::string name = expectIdent();
  while (eat("::")) {
    if (at("~")) {
      take();
      name += "::~" + expectIdent();
    } else {
      name += "::" + expectIdent();
    }
  }

  if (name == "this") {
    if (!currentClass_ || !currentFunc_ || !currentFunc_->isMethod || currentFunc_->isStatic)
      diag_.error(loc, "'this' is only valid inside a non-static member function");
    auto e = makeExpr(ExprKind::This, loc);
    e->type = prog_.types.ptrTo(structType(currentClass_));
    return e;
  }
  if (name == "true") {
    auto e = makeExpr(ExprKind::Bool, loc);
    e->ival = 1;
    e->type = prog_.types.boolType();
    return e;
  }
  if (name == "false") {
    auto e = makeExpr(ExprKind::Bool, loc);
    e->ival = 0;
    e->type = prog_.types.boolType();
    return e;
  }
  if (name == "nullptr") {
    auto e = makeExpr(ExprKind::Null, loc);
    e->type = prog_.types.ptrTo(prog_.types.voidType());
    return e;
  }

  if (at("(")) {
    StructDecl* sd = lookupStruct(name);
    if (!sd) {
      Type* tt = lookupType(name);
      if (tt && tt->kind == TypeKind::Struct) sd = tt->sdecl;
    }
    if (sd) {
      auto e = makeExpr(ExprKind::Construct, loc);
      e->sdecl = sd;
      e->type = structType(sd);
      e->args = parseCallArgs();
      if (!sd->ctors.empty()) {
        FuncDecl* best = nullptr; int bestScore = 1<<30; bool ambig=false;
        for (auto* c : sd->ctors) { int sc=overloadScore(c,e->args); if(sc<0) continue; if(!best||sc<bestScore){best=c;bestScore=sc;ambig=false;} else if(sc==bestScore&&best!=c) ambig=true; }
        if (!best) diag_.error(loc, "no matching constructor for '" + sd->name + "'");
        if (ambig) diag_.error(loc, "ambiguous constructor for '" + sd->name + "'");
        e->func = best;
        size_t fixed = best->params.size();
        for (size_t i = e->args.size(); i < fixed; ++i) {
          if (!best->params[i]->defaultArg)
            diag_.error(loc, "constructor of '" + sd->name + "' requires more arguments");
          e->args.push_back(cloneExpr(best->params[i]->defaultArg.get()));
        }
      } else if (!e->args.empty()) {
        diag_.error(loc, "'" + sd->name + "' has no constructor");
      }
      return e;
    }
    std::vector<ExprPtr> args = parseCallArgs();
    FuncDecl* fn = lookupFunc(name, args);
    if (!fn) diag_.error(loc, "unknown function '" + name + "'");
    bool implicit = fn->isMethod && !fn->isStatic && currentClass_ && fn->owner == currentClass_;
    return parseCall(fn, nullptr, implicit, false, loc, std::move(args));
  }

  auto ec = enumConsts_.find(name);
  if (ec != enumConsts_.end()) return makeInt(ec->second, loc);

  VarSym* v = lookupVar(name);
  if (v) {
    auto e = makeExpr(ExprKind::Ident, loc);
    e->var = v;
    e->type = v->type;
    e->lvalue = true;
    return e;
  }

  if (currentClass_) {
    if (Field* f = currentClass_->findField(name)) {
      auto e = makeExpr(ExprKind::Ident, loc);
      e->field = f;
      e->implicitThis = true;
      e->type = f->type;
      e->lvalue = true;
      return e;
    }
  }

  if (lookupFunc(name))
    diag_.error(loc, "function '" + name + "' used as a value (function pointers are not supported)");

  if (lookupType(name))
    diag_.error(loc, "type '" + name + "' used as an expression");

  diag_.error(loc, "unknown identifier '" + name + "'");
}

long long Parser::evalConst(Expr* e) {
  if (!e) diag_.error(SourceLoc{}, "expected a constant expression");
  switch (e->kind) {
    case ExprKind::Int:
    case ExprKind::Bool:
    case ExprKind::Char:
      return e->ival;
    case ExprKind::Unary: {
      long long v = evalConst(e->args[0].get());
      if (e->op == "-") return -v;
      if (e->op == "+") return v;
      if (e->op == "!") return v == 0;
      if (e->op == "~") return ~v;
      break;
    }
    case ExprKind::Binary: {
      long long a = evalConst(e->args[0].get());
      long long b = evalConst(e->args[1].get());
      const std::string& op = e->op;
      if (op == "+") return a + b;
      if (op == "-") return a - b;
      if (op == "*") return a * b;
      if (op == "/") return b ? a / b : 0;
      if (op == "%") return b ? a % b : 0;
      if (op == "<<") return a << (b & 63);
      if (op == ">>") return a >> (b & 63);
      if (op == "&") return a & b;
      if (op == "|") return a | b;
      if (op == "^") return a ^ b;
      if (op == "==") return a == b;
      if (op == "!=") return a != b;
      if (op == "<") return a < b;
      if (op == "<=") return a <= b;
      if (op == ">") return a > b;
      if (op == ">=") return a >= b;
      if (op == "&&") return a && b;
      if (op == "||") return a || b;
      break;
    }
    case ExprKind::Ternary:
      return evalConst(e->args[0].get()) ? evalConst(e->args[1].get())
                                         : evalConst(e->args[2].get());
    case ExprKind::Cast:
      return evalConst(e->args[0].get());
    case ExprKind::Comma:
      return evalConst(e->args[1].get());
    case ExprKind::Sizeof:
      if (!e->args.empty() && e->args[0]->type) return typeSizeOf(e->args[0]->type);
      return e->ival;
    default:
      break;
  }
  diag_.error(e->loc, "expression is not a constant");
  return 0;
}

} // namespace tc

