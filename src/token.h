#pragma once
#include "common.h"
#include <string>
#include <vector>

namespace tc {

enum class Tok {
  Eof,
  Newline,
  Ident,
  Number,
  String,
  Char,
  Punct,
  Placemarker
};

struct Token {
  Tok kind = Tok::Eof;
  std::string text;
  SourceLoc loc;
  bool bol = false;
  bool space = false;
  bool fromMacro = false;
  std::vector<std::string> hide;
};

inline bool isPunct(const Token& t, const char* s) {
  return t.kind == Tok::Punct && t.text == s;
}

inline bool isIdent(const Token& t, const char* s) {
  return t.kind == Tok::Ident && t.text == s;
}

} // namespace tc
