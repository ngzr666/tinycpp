#pragma once
#include "ast.h"
#include "common.h"
#include <sstream>
#include <string>
#include <vector>
#include <set>

namespace tc {

class CodeGen {
public:
  CodeGen(Program& prog, Diag& diag);
  void setAbortAssert(bool v) { abortAssert_ = v; }
  std::string generate();

private:
  void emitPrelude();
  void emitStructForward();
  void emitStructDefs();
  void emitPrototypes();
  void emitGlobals();
  void emitFunctions();
  void emitHelpers();
  void emitVtables();

  std::string ctype(Type* t);
  std::string decl(Type* t, const std::string& name);
  std::string paramDecl(Type* t, const std::string& name);
  std::string structName(StructDecl* s);
  std::string funcName(FuncDecl* f);
  std::string varName(VarSym* v);
  std::string fieldName(Field* f);
  std::string sanitize(const std::string& s);
  std::string ind(int n);

  std::string funcSig(FuncDecl* f, bool withNames);
  std::string helperNewName(StructDecl* s, FuncDecl* ctor = nullptr);
  std::string helperDelName(StructDecl* s);
  std::string helperMakeName(StructDecl* s, FuncDecl* ctor = nullptr);

  std::string expr(Expr* e);
  std::string pointerExpr(Expr* e);
  void emitStmt(Stmt* s, int indent);
  void emitBlock(Stmt* s, int indent, bool newScope = true);
  void emitScoped(Stmt* s, int indent);
  void emitLine(int indent, const std::string& s);

  void pushCleanup();
  void popCleanup(int indent);
  void registerCleanup(VarSym* v);
  void emitDtorsFrom(size_t depth, int indent);
  void emitFieldDefaults(StructDecl* s, const std::string& ptr, int indent);
  void emitCtorCall(StructDecl* s, const std::string& ptr,
                    const std::vector<ExprPtr>& args, int indent);
  bool hasCleanup() const;

  bool isLibc(const std::string& name);

  Program& prog_;
  Diag& diag_;
  std::ostringstream out_;
  std::vector<std::vector<VarSym*>> cleanupScopes_;
  std::vector<size_t> breakDepths_;
  FuncDecl* curFunc_ = nullptr;
  bool abortAssert_ = false;
  int temp_ = 0;
  std::string promotedGlobals_;
  std::set<std::string> emittedPromoted_;
};

} // namespace tc
