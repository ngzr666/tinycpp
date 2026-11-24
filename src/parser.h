#pragma once
#include "ast.h"
#include "common.h"
#include "token.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace tc {

class Parser {
public:
  Parser(SourceManager& sm, Diag& diag, std::vector<Token> toks);

  Program parse();

private:
  // token helpers
  const Token& peek(int n = 0) const;
  Token take();
  bool at(const char* s) const;
  bool atIdent(const char* s) const;
  bool eat(const char* s);
  void expect(const char* s);
  std::string expectIdent();
  bool atEof() const;
  void skipBalanced(const char* open, const char* close, bool quiet = false);
  void skipAttributes();

  // scopes / symbols
  void pushScope();
  void popScope();
  void declareVar(VarSym* v);
  VarSym* lookupVar(const std::string& name);
  FuncDecl* lookupFunc(const std::string& name);
  FuncDecl* lookupFunc(const std::string& name, const std::vector<ExprPtr>& args);
  StructDecl* lookupStruct(const std::string& name);
  Type* lookupType(const std::string& name);
  std::string qualify(const std::string& name) const;

  VarSym* newVar(const std::string& name, Type* type, SourceLoc loc);
  FuncDecl* newFunc(const std::string& name, Type* ret, SourceLoc loc);
  void registerFunc(FuncDecl* f);
  int overloadScore(FuncDecl* f, const std::vector<ExprPtr>& args);
  StructDecl* getOrCreateStruct(const std::string& name, SourceLoc loc);
  Type* structType(StructDecl* sd);

  // top level
  void parseTopLevel();
  void parseNamespace();
  void parseExternC();
  void parseUsing();
  void parseTypedef();
  void parseEnum();
  void parseStruct();
  void parseDeclaration(bool global);
  void parseStaticAssert();

  // types / declarators
  Type* parseType();
  Type* parseDeclarator(Type* base, std::string& name);
  bool isTypeStart() const;
  std::vector<VarSym*> parseParams(bool& variadic);
  void parseFunctionRest(Type* ret, const std::string& name, SourceLoc loc,
                         bool isMethod, bool isStatic, bool isVirtual);
  StmtPtr parseFunctionBody(FuncDecl* fn);

  // class members
  void parseCtor(StructDecl* sd, std::vector<std::pair<FuncDecl*, size_t>>& bodies);
  void parseDtor(StructDecl* sd, std::vector<std::pair<FuncDecl*, size_t>>& bodies, bool isVirtual = false);
  void parseMember(StructDecl* sd, std::vector<std::pair<FuncDecl*, size_t>>& bodies);
  void parseMemberMethod(StructDecl* sd, Type* ret, const std::string& name,
                         SourceLoc loc, bool isStatic, bool isVirtual,
                         std::vector<std::pair<FuncDecl*, size_t>>& bodies);
  void parseFieldList(StructDecl* sd, Type* base);

  // statements
  StmtPtr parseStatement();
  StmtPtr parseBlock();
  StmtPtr parseDeclStmt();
  StmtPtr parseIf();
  StmtPtr parseWhile();
  StmtPtr parseDoWhile();
  StmtPtr parseFor();
  StmtPtr parseSwitch();
  StmtPtr parseCase(bool isDefault);
  StmtPtr parseReturn();
  StmtPtr parseCoutStmt();
  bool isCoutStart() const;

  // expressions
  ExprPtr parseExpression();
  ExprPtr parseAssign();
  ExprPtr parseTernary();
  ExprPtr parseBinary(int minPrec);
  ExprPtr parseUnary();
  ExprPtr parsePostfix();
  ExprPtr parsePrimary();
  ExprPtr parseNumber();
  ExprPtr parseString();
  ExprPtr parseChar();
  ExprPtr parseIdentExpr();
  ExprPtr parseCall(FuncDecl* fn, ExprPtr self, bool implicitThis,
                    bool arrow, SourceLoc loc, std::vector<ExprPtr> args);
  std::vector<ExprPtr> parseCallArgs();
  ExprPtr parseNew();
  ExprPtr parseDelete();
  ExprPtr parseSizeof();

  ExprPtr makeExpr(ExprKind k, SourceLoc loc);
  ExprPtr makeInt(long long v, SourceLoc loc);
  ExprPtr makeUnary(const std::string& op, ExprPtr operand, SourceLoc loc);
  ExprPtr makeBinary(const std::string& op, ExprPtr a, ExprPtr b, SourceLoc loc);

  Type* usualType(Type* a, Type* b);
  long long evalConst(Expr* e);

  void registerBuiltins();

  SourceManager& sm_;
  Diag& diag_;
  std::vector<Token> toks_;
  size_t pos_ = 0;
  Program prog_;

  std::vector<std::map<std::string, VarSym*>> scopes_;
  std::map<std::string, VarSym*> globals_;
  std::multimap<std::string, FuncDecl*> funcs_;
  std::map<std::string, StructDecl*> structs_;
  std::map<std::string, Type*> typedefs_;
  std::map<std::string, long long> enumConsts_;
  std::vector<std::string> usingNamespaces_;
  std::string nsPrefix_;
  StructDecl* currentClass_ = nullptr;
  FuncDecl* currentFunc_ = nullptr;
  bool externC_ = false;
  int lambdaCounter_ = 0;

  struct TemplateFunc { std::string name; std::vector<std::string> params; std::vector<Token> tokens; SourceLoc loc; bool hasPack=false; std::string packName; bool isStruct=false; };
  std::vector<TemplateFunc> templates_;
  std::set<std::string> triedInstantiations_;
  void parseTemplateFunc();
  FuncDecl* instantiateTemplate(const TemplateFunc& tf, const std::vector<ExprPtr>& args, SourceLoc loc);
  StructDecl* instantiateTemplateStruct(const TemplateFunc& tf, const std::vector<Type*>& typeArgs, SourceLoc loc);
  std::string typeKey(Type* t);
};

} // namespace tc
