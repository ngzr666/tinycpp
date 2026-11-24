#pragma once
#include "common.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tc {

struct Type;
struct StructDecl;
struct FuncDecl;
struct VarSym;
struct Expr;
struct Stmt;
struct Field;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

enum class TypeKind {
  Void, Bool, Char, Short, Int, Long, LongLong, Float, Double,
  Ptr, Array, Struct
};

struct Type {
  TypeKind kind = TypeKind::Void;
  bool isUnsigned = false;
  bool isRef = false;
  Type* elem = nullptr;      // Ptr / Array element, or pointee of a reference
  long long arrayLen = 0;
  StructDecl* sdecl = nullptr;
};

class TypeContext {
public:
  TypeContext();

  Type* builtin(TypeKind k, bool isUnsigned = false);
  Type* ptrTo(Type* t);
  Type* refTo(Type* t);
  Type* arrayOf(Type* t, long long n);
  Type* voidType() { return builtin(TypeKind::Void); }
  Type* boolType() { return builtin(TypeKind::Bool); }
  Type* intType() { return builtin(TypeKind::Int); }
  Type* charType() { return builtin(TypeKind::Char); }
  Type* longType() { return builtin(TypeKind::Long); }
  Type* doubleType() { return builtin(TypeKind::Double); }
  Type* sizeType() { return builtin(TypeKind::Long, true); }

private:
  std::vector<std::unique_ptr<Type>> pool_;
  std::map<std::pair<int, bool>, Type*> prim_;
};

bool sameType(const Type* a, const Type* b);
bool isPointerLike(const Type* t);
Type* pointeeOf(Type* t);

struct Field {
  std::string name;
  Type* type = nullptr;
  ExprPtr defaultInit;
  SourceLoc loc;
};

struct VarSym {
  std::string name;
  Type* type = nullptr;
  bool isGlobal = false;
  bool isParam = false;
  bool isField = false;
  bool isRef = false;
  bool isStatic = false;
  bool isExtern = false;
  StructDecl* owner = nullptr;
  ExprPtr defaultArg;
  ExprPtr init;                    // global initializer
  std::vector<ExprPtr> initList;   // brace initializer
  FuncDecl* funcRef = nullptr;     // holds lambda/function reference
  SourceLoc loc;
};

struct InitItem {
  Field* field = nullptr;
  ExprPtr value;
};

struct FuncDecl {
  std::string name;       // logical name, possibly qualified
  std::string shortName;  // unqualified name
  Type* ret = nullptr;
  std::vector<VarSym*> params;
  bool variadic = false;
  bool isMethod = false;
  bool isCtor = false;
  bool isDtor = false;
  bool isExternC = false;
  bool isDefined = false;
  bool isStatic = false;
  bool isVirtual = false;
  int vslot = -1;
  StructDecl* owner = nullptr;
  StmtPtr body;
  std::vector<InitItem> initList;
  StructDecl* baseInit = nullptr;
  std::vector<ExprPtr> baseInitArgs;
  SourceLoc loc;
};

struct StructDecl {
  std::string name;
  std::vector<Field> fields;
  std::vector<FuncDecl*> methods;
  std::vector<FuncDecl*> ctors;
  FuncDecl* dtor = nullptr;
  FuncDecl* ctor() { return ctors.empty() ? nullptr : ctors.front(); }
  StructDecl* base = nullptr;
  bool complete = false;
  bool hasVtable = false;
  std::vector<FuncDecl*> vtable;
  SourceLoc loc;

  Field* findField(const std::string& n) {
    for (auto& f : fields)
      if (f.name == n) return &f;
    if (base) return base->findField(n);
    return nullptr;
  }
  FuncDecl* findMethod(const std::string& n) {
    for (auto* m : methods)
      if (m->shortName == n) return m;
    if (base) return base->findMethod(n);
    return nullptr;
  }
  std::vector<FuncDecl*> findMethods(const std::string& n) {
    std::vector<FuncDecl*> r;
    for (auto* m : methods)
      if (m->shortName == n) r.push_back(m);
    if (base) {
      auto br = base->findMethods(n);
      r.insert(r.end(), br.begin(), br.end());
    }
    return r;
  }
};

enum class ExprKind {
  Int, Float, Str, Char, Bool, Null, Ident, This,
  Unary, Binary, Assign, Ternary, Call, Index, Member,
  New, Delete, Cast, Sizeof, Comma, Postfix, Construct
};

struct Expr {
  ExprKind kind = ExprKind::Int;
  Type* type = nullptr;
  bool lvalue = false;
  std::string op;
  long long ival = 0;
  double fval = 0;
  std::vector<ExprPtr> args;
  ExprPtr self;
  Type* targetType = nullptr;
  SourceLoc loc;

  // resolved symbols
  VarSym* var = nullptr;
  FuncDecl* func = nullptr;
  StructDecl* sdecl = nullptr;
  Field* field = nullptr;
  bool implicitThis = false;
  bool arrow = false;
  bool isArrayNew = false;
  bool globalScope = false;
  FuncDecl* funcRef = nullptr;     // function reference (lambda)
};

enum class StmtKind {
  Block, Expr, Decl, If, While, DoWhile, For, Return,
  Break, Continue, Switch, Case, Default, Goto, Label, Empty
};

struct DeclItem {
  VarSym* var = nullptr;
  ExprPtr init;
  std::vector<ExprPtr> ctorArgs;
  bool hasCtorArgs = false;
  bool isCtor = false;
  bool isListInit = false;
  std::vector<ExprPtr> list;
};

struct Stmt {
  StmtKind kind = StmtKind::Empty;
  std::vector<StmtPtr> kids;
  ExprPtr expr;
  ExprPtr cond;
  ExprPtr step;
  StmtPtr initStmt;
  StmtPtr body;
  StmtPtr elseBody;
  std::vector<DeclItem> decls;
  long long caseValue = 0;
  std::string label;
  SourceLoc loc;
};

struct Program {
  TypeContext types;
  std::vector<std::unique_ptr<StructDecl>> structs;
  std::vector<std::unique_ptr<FuncDecl>> funcs;
  std::vector<std::unique_ptr<VarSym>> vars;
  std::vector<std::unique_ptr<Type>> extraTypes;

  StructDecl* findStruct(const std::string& name) const {
    for (auto& s : structs)
      if (s->name == name) return s.get();
    return nullptr;
  }
  FuncDecl* findFunc(const std::string& name) const {
    for (auto& f : funcs)
      if (f->name == name) return f.get();
    return nullptr;
  }
};

} // namespace tc
