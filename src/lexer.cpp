#include "lexer.h"
#include <cctype>

namespace tc {
namespace {

bool identStart(char c) {
  return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}
bool identChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}
bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Longest first.
const char* kPuncts[] = {
  "<<=", ">>=", "<=>", "...", "->*", "::", "->", "++", "--", "<<", ">>",
  "<=", ">=", "==", "!=", "&&", "||", "+=", "-=", "*=", "/=", "%=", "&=",
  "|=", "^=", "##", ".*", nullptr
};

} // namespace

Lexer::Lexer(SourceManager& sm, Diag& diag, int fileId)
    : sm_(sm), diag_(diag), fileId_(fileId) {
  src_ = sm.file(fileId).text;
}

char Lexer::cur() const { return pos_ < src_.size() ? src_[pos_] : '\0'; }

char Lexer::peek(int n) const {
  size_t p = pos_ + static_cast<size_t>(n);
  return p < src_.size() ? src_[p] : '\0';
}

void Lexer::adv() {
  if (pos_ >= src_.size()) return;
  if (src_[pos_] == '\n') {
    line_++;
    col_ = 1;
  } else {
    col_++;
  }
  pos_++;
}

bool Lexer::splice() {
  if (cur() != '\\') return false;
  if (peek(1) == '\n') {
    adv();
    adv();
    return true;
  }
  if (peek(1) == '\r' && peek(2) == '\n') {
    adv();
    adv();
    adv();
    return true;
  }
  return false;
}

Token Lexer::lexIdent() {
  SourceLoc loc = here();
  std::string s;
  while (true) {
    if (splice()) continue;
    if (!identChar(cur())) break;
    s += cur();
    adv();
  }
  return Token{Tok::Ident, s, loc, false, false, false, {}};
}

Token Lexer::lexNumber() {
  SourceLoc loc = here();
  std::string s;
  bool first = true;
  while (true) {
    if (splice()) continue;
    char c = cur();
    if (identChar(c) || c == '.' || c == '\'') {
      s += c;
      adv();
      first = false;
      continue;
    }
    if ((c == '+' || c == '-') && !s.empty()) {
      char p = s.back();
      if (p == 'e' || p == 'E' || p == 'p' || p == 'P') {
        s += c;
        adv();
        first = false;
        continue;
      }
    }
    break;
  }
  (void)first;
  return Token{Tok::Number, s, loc, false, false, false, {}};
}

Token Lexer::lexQuoted(char quote) {
  SourceLoc loc = here();
  std::string s;
  s += quote;
  adv(); // opening quote
  while (true) {
    if (splice()) continue;
    char c = cur();
    if (c == '\0') diag_.error(loc, "unterminated literal");
    if (c == '\n') diag_.error(here(), "newline in literal");
    if (c == quote) {
      s += c;
      adv();
      break;
    }
    if (c == '\\') {
      s += c;
      adv();
      if (cur() == '\0') diag_.error(loc, "unterminated escape");
      s += cur();
      adv();
      continue;
    }
    s += c;
    adv();
  }
  return Token{quote == '"' ? Tok::String : Tok::Char, s, loc, false, false, false, {}};
}

Token Lexer::lexRawString(int prefixLen) {
  SourceLoc loc = here();
  for (int i = 0; i < prefixLen; ++i) adv();
  // At 'R'
  adv(); // R
  if (cur() != '"') diag_.error(loc, "malformed raw string");
  adv(); // "
  std::string delim;
  while (cur() != '(') {
    if (cur() == '\0' || cur() == '\n' || delim.size() > 16)
      diag_.error(loc, "malformed raw string delimiter");
    delim += cur();
    adv();
  }
  adv(); // (
  std::string raw;
  std::string terminator = ")" + delim + "\"";
  while (true) {
    if (cur() == '\0') diag_.error(loc, "unterminated raw string");
    bool match = true;
    for (size_t i = 0; i < terminator.size(); ++i) {
      if (peek(static_cast<int>(i)) != terminator[i]) {
        match = false;
        break;
      }
    }
    if (match) {
      for (size_t i = 0; i < terminator.size(); ++i) adv();
      break;
    }
    raw += cur();
    adv();
  }
  std::string s = "\"";
  for (char c : raw) {
    switch (c) {
      case '\\': s += "\\\\"; break;
      case '"': s += "\\\""; break;
      case '\n': s += "\\n"; break;
      case '\r': break;
      case '\t': s += "\\t"; break;
      default: s += c; break;
    }
  }
  s += "\"";
  return Token{Tok::String, s, loc, false, false, false, {}};
}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> out;

  while (true) {
    if (splice()) continue;
    char c = cur();
    if (c == '\0') break;

    if (c == '\n') {
      out.push_back(Token{Tok::Newline, "\n", here(), true, false, false, {}});
      adv();
      bol_ = true;
      space_ = false;
      continue;
    }

    if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
      space_ = true;
      adv();
      continue;
    }

    if (c == '/' && peek(1) == '/') {
      adv();
      adv();
      while (cur() != '\0' && cur() != '\n') {
        if (splice()) continue;
        adv();
      }
      space_ = true;
      continue;
    }

    if (c == '/' && peek(1) == '*') {
      SourceLoc start = here();
      adv();
      adv();
      bool closed = false;
      while (cur() != '\0') {
        if (splice()) continue;
        if (cur() == '*' && peek(1) == '/') {
          adv();
          adv();
          closed = true;
          break;
        }
        if (cur() == '\n') {
          out.push_back(Token{Tok::Newline, "\n", here(), true, false, false, {}});
          adv();
          bol_ = true;
          continue;
        }
        adv();
      }
      if (!closed) diag_.error(start, "unterminated block comment");
      space_ = true;
      continue;
    }

    SourceLoc loc = here();
    bool wasBol = bol_;
    bool wasSpace = space_;
    bol_ = false;
    space_ = false;

    Token t;

    // String / char literal prefixes.
    int prefixLen = 0;
    bool isRaw = false;
    if (c == 'u' && peek(1) == '8' && (peek(2) == '"' || peek(2) == '\'' || peek(2) == 'R')) {
      prefixLen = 2;
      isRaw = peek(2) == 'R';
    } else if ((c == 'u' || c == 'U' || c == 'L') &&
               (peek(1) == '"' || peek(1) == '\'' || peek(1) == 'R')) {
      prefixLen = 1;
      isRaw = peek(1) == 'R';
    } else if (c == 'R' && peek(1) == '"') {
      isRaw = true;
    }

    if (prefixLen > 0 && isRaw) {
      t = lexRawString(prefixLen);
    } else if (prefixLen > 0 && !isRaw) {
      for (int i = 0; i < prefixLen; ++i) adv();
      t = lexQuoted(cur());
    } else if (c == '"' || c == '\'') {
      t = lexQuoted(c);
    } else if (isDigit(c) || (c == '.' && isDigit(peek(1)))) {
      t = lexNumber();
    } else if (identStart(c)) {
      t = lexIdent();
    } else {
      std::string p;
      for (int i = 0; kPuncts[i]; ++i) {
        const char* cand = kPuncts[i];
        size_t n = std::char_traits<char>::length(cand);
        bool match = true;
        for (size_t j = 0; j < n; ++j) {
          if (peek(static_cast<int>(j)) != cand[j]) {
            match = false;
            break;
          }
        }
        if (match) {
          p = cand;
          break;
        }
      }
      if (p.empty()) {
        // Unknown characters are kept as tokens; this lets #error messages and
        // vendor extensions pass through the preprocessor untouched.
        p = std::string(1, c);
      }
      for (char ch : p) adv();
      t.kind = Tok::Punct;
      t.text = p;
    }

    t.loc = loc;
    t.bol = wasBol;
    t.space = wasSpace;
    out.push_back(t);
  }

  out.push_back(Token{Tok::Eof, "", here(), true, false, false, {}});
  return out;
}

} // namespace tc
