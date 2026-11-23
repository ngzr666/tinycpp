#pragma once
#include "common.h"
#include "token.h"
#include <string>
#include <vector>

namespace tc {

class Lexer {
public:
  Lexer(SourceManager& sm, Diag& diag, int fileId);
  std::vector<Token> tokenize();

private:
  char cur() const;
  char peek(int n = 1) const;
  void adv();
  bool splice();
  SourceLoc here() const { return SourceLoc{fileId_, line_, col_}; }

  Token lexQuoted(char quote);
  Token lexRawString(int prefixLen);
  Token lexNumber();
  Token lexIdent();

  SourceManager& sm_;
  Diag& diag_;
  int fileId_;
  std::string src_;
  size_t pos_ = 0;
  int line_ = 1;
  int col_ = 1;
  bool bol_ = true;
  bool space_ = false;
};

} // namespace tc
