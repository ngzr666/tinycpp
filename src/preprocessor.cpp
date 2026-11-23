#include "preprocessor.h"
#include "lexer.h"
#include "literal.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace tc {
namespace fs = std::filesystem;

namespace {

bool hidden(const Token& t, const std::string& name) {
  return std::find(t.hide.begin(), t.hide.end(), name) != t.hide.end();
}

void addHide(Token& t, const std::string& name) {
  if (!hidden(t, name)) t.hide.push_back(name);
}

void mergeHide(Token& t, const std::vector<std::string>& hs) {
  for (const auto& h : hs) addHide(t, h);
}

Token numToken(const std::string& s, SourceLoc loc) {
  Token t;
  t.kind = Tok::Number;
  t.text = s;
  t.loc = loc;
  return t;
}

Token strToken(const std::string& s, SourceLoc loc) {
  Token t;
  t.kind = Tok::String;
  t.text = s;
  t.loc = loc;
  return t;
}

Token punctToken(const std::string& s, SourceLoc loc) {
  Token t;
  t.kind = Tok::Punct;
  t.text = s;
  t.loc = loc;
  return t;
}

std::string escapeForString(const std::string& s) {
  std::string r;
  for (char c : s) {
    switch (c) {
      case '\\': r += "\\\\"; break;
      case '"': r += "\\\""; break;
      case '\n': r += "\\n"; break;
      case '\r': r += "\\r"; break;
      case '\t': r += "\\t"; break;
      default: r += c; break;
    }
  }
  return r;
}

std::string unquote(const std::string& s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
    return s.substr(1, s.size() - 2);
  return s;
}

bool headerFromTokens(const std::vector<Token>& toks, std::string& name, bool& quoted) {
  if (toks.empty()) return false;
  if (toks[0].kind == Tok::String) {
    name = unquote(toks[0].text);
    quoted = true;
    return true;
  }
  if (isPunct(toks[0], "<")) {
    std::string s;
    for (size_t i = 1; i < toks.size(); ++i) {
      if (isPunct(toks[i], ">")) {
        name = s;
        quoted = false;
        return true;
      }
      if (i > 1 && toks[i].space) s += ' ';
      s += toks[i].text;
    }
  }
  return false;
}

Token stringize(const std::vector<Token>& arg, SourceLoc loc) {
  std::string s = "\"";
  for (size_t i = 0; i < arg.size(); ++i) {
    if (i > 0 && arg[i].space) s += ' ';
    for (char c : arg[i].text) {
      if (c == '\\' || c == '"') s += '\\';
      s += c;
    }
  }
  s += '"';
  return strToken(s, loc);
}

class PPExpr {
public:
  PPExpr(const std::vector<Token>& toks, Diag& diag) : toks_(toks), diag_(diag) {}

  long long parse() { return conditional(); }

private:
  const std::vector<Token>& toks_;
  Diag& diag_;
  size_t pos_ = 0;

  bool eof() const { return pos_ >= toks_.size(); }
  const Token& cur() const {
    static Token eofTok;
    return eof() ? eofTok : toks_[pos_];
  }
  bool at(const char* s) const { return !eof() && isPunct(toks_[pos_], s); }
  void expect(const char* s) {
    if (!at(s)) diag_.error(cur().loc, std::string("expected '") + s + "' in #if");
    ++pos_;
  }

  static int precedence(const std::string& op) {
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

  long long conditional() {
    long long c = binary(1);
    if (at("?")) {
      ++pos_;
      long long a = conditional();
      expect(":");
      long long b = conditional();
      return c ? a : b;
    }
    return c;
  }

  long long binary(int minPrec) {
    long long lhs = unary();
    while (!eof()) {
      const std::string& op = cur().text;
      int prec = precedence(op);
      if (prec < minPrec) break;
      ++pos_;
      long long rhs = binary(prec + 1);
      lhs = apply(op, lhs, rhs);
    }
    return lhs;
  }

  long long unary() {
    if (eof()) {
      diag_.error(SourceLoc{}, "unexpected end of #if expression");
    }
    if (at("+")) { ++pos_; return unary(); }
    if (at("-")) { ++pos_; return -unary(); }
    if (at("!")) { ++pos_; return unary() == 0 ? 1 : 0; }
    if (at("~")) { ++pos_; return ~unary(); }
    return primary();
  }

  long long primary() {
    if (eof()) diag_.error(SourceLoc{}, "unexpected end of #if expression");
    Token t = toks_[pos_];
    if (isPunct(t, "(")) {
      ++pos_;
      long long v = conditional();
      expect(")");
      return v;
    }
    if (t.kind == Tok::Number) {
      ++pos_;
      IntLiteral lit = parseIntLiteral(t.text);
      if (!lit.ok) diag_.error(t.loc, "invalid integer in #if: " + t.text);
      return static_cast<long long>(lit.value);
    }
    if (t.kind == Tok::Char) {
      ++pos_;
      bool ok = false;
      long long v = parseCharLiteral(t.text, ok);
      if (!ok) diag_.error(t.loc, "invalid character in #if");
      return v;
    }
    if (t.kind == Tok::Ident) {
      ++pos_;
      if (t.text == "true") return 1;
      return 0;
    }
    diag_.error(t.loc, "invalid token in #if expression: " + t.text);
  }

  long long apply(const std::string& op, long long a, long long b) {
    if (op == "||") return (a || b) ? 1 : 0;
    if (op == "&&") return (a && b) ? 1 : 0;
    if (op == "|") return a | b;
    if (op == "^") return a ^ b;
    if (op == "&") return a & b;
    if (op == "==") return a == b;
    if (op == "!=") return a != b;
    if (op == "<") return a < b;
    if (op == "<=") return a <= b;
    if (op == ">") return a > b;
    if (op == ">=") return a >= b;
    if (op == "<<") return a << (b & 63);
    if (op == ">>") return a >> (b & 63);
    if (op == "+") return a + b;
    if (op == "-") return a - b;
    if (op == "*") return a * b;
    if (op == "/") {
      if (b == 0) diag_.error(cur().loc, "division by zero in #if");
      return a / b;
    }
    if (op == "%") {
      if (b == 0) diag_.error(cur().loc, "division by zero in #if");
      return a % b;
    }
    diag_.error(cur().loc, "unknown operator in #if: " + op);
  }
};

} // namespace

Preprocessor::Preprocessor(SourceManager& sm, Diag& diag) : sm_(sm), diag_(diag) {
  std::time_t now = std::time(nullptr);
  std::tm* tm = std::localtime(&now);
  char dbuf[64] = {};
  char tbuf[64] = {};
  if (tm) {
    std::strftime(dbuf, sizeof(dbuf), "%b %e %Y", tm);
    std::strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm);
  }
  date_ = dbuf;
  time_ = tbuf;

  define("__TINYCPP__", "1");
  define("__cplusplus", "201703L");
  define("__STDC__", "1");
  define("__STDC_VERSION__", "201112L");
  define("__STDC_HOSTED__", "1");

  // Target model of the host C compiler (tinycpp emits C, so the host is the
  // target). These are the probes real-world headers rely on.
  define("__CHAR_BIT__", "8");
  define("__SIZEOF_SHORT__", std::to_string(sizeof(short)));
  define("__SIZEOF_INT__", std::to_string(sizeof(int)));
  define("__SIZEOF_LONG__", std::to_string(sizeof(long)));
  define("__SIZEOF_LONG_LONG__", std::to_string(sizeof(long long)));
  define("__SIZEOF_POINTER__", std::to_string(sizeof(void*)));
  define("__SIZEOF_SIZE_T__", std::to_string(sizeof(std::size_t)));
  define("__SIZEOF_FLOAT__", std::to_string(sizeof(float)));
  define("__SIZEOF_DOUBLE__", std::to_string(sizeof(double)));
  define("__SIZEOF_LONG_DOUBLE__", std::to_string(sizeof(long double)));
  define("__SIZEOF_WCHAR_T__", std::to_string(sizeof(wchar_t)));
  define("__ORDER_LITTLE_ENDIAN__", "1234");
  define("__ORDER_BIG_ENDIAN__", "4321");
  define("__ORDER_PDP_ENDIAN__", "3412");
  {
    unsigned int probe = 1;
    bool little = *reinterpret_cast<unsigned char*>(&probe) == 1;
    define("__BYTE_ORDER__", little ? "1234" : "4321");
    define("__FLOAT_WORD_ORDER__", little ? "1234" : "4321");
    define("__LITTLE_ENDIAN__", little ? "1" : "0");
  }
  if (sizeof(void*) == 8 && sizeof(long) == 8) define("__LP64__", "1");
#if defined(__SIZEOF_INT128__)
  define("__SIZEOF_INT128__", "16");
#endif

#if defined(__aarch64__)
  define("__aarch64__", "1");
#elif defined(__x86_64__)
  define("__x86_64__", "1");
#elif defined(__i386__)
  define("__i386__", "1");
#elif defined(__arm__)
  define("__arm__", "1");
#elif defined(__riscv)
  define("__riscv", "1");
#endif

#if defined(__linux__)
  define("__linux__", "1");
  define("__unix__", "1");
#endif
#if defined(__ANDROID__)
  define("__ANDROID__", "1");
#endif
#if defined(__APPLE__)
  define("__APPLE__", "1");
#endif
#if defined(_WIN32)
  define("_WIN32", "1");
#endif

  // Compiler feature probes used by libc++ and other real-world headers.
  auto featureValue = [&](const char* name, const char* value) {
    Macro m;
    m.name = name;
    m.isFunction = true;
    m.params = {"x"};
    m.body = {numToken(value, SourceLoc{})};
    macros_[name] = std::move(m);
  };
  featureValue("__has_feature", "0");
  featureValue("__has_extension", "0");
  featureValue("__has_builtin", "1");
  featureValue("__has_attribute", "0");
  featureValue("__has_cpp_attribute", "0");
  featureValue("__has_c_attribute", "0");
  featureValue("__has_declspec_attribute", "0");
  featureValue("__has_warning", "0");
  featureValue("__has_constexpr_builtin", "1");
  featureValue("__has_unique_object_representations", "1");

  {
    Macro m;
    m.name = "__is_identifier";
    m.isFunction = true;
    m.params = {"x"};
    m.body = {numToken("1", SourceLoc{})};
    macros_[m.name] = std::move(m);
  }
}

void Preprocessor::addIncludeDir(std::string dir, bool system) {
  includeDirs_.emplace_back(std::move(dir), system);
}

void Preprocessor::define(const std::string& name, const std::string& value) {
  Macro m;
  m.name = name;
  if (!value.empty()) m.body = lexSnippet(value, SourceLoc{});
  macros_[name] = std::move(m);
}

void Preprocessor::undef(const std::string& name) {
  macros_.erase(name);
}

bool Preprocessor::isDefined(const std::string& name) const {
  return macros_.count(name) != 0;
}

std::string Preprocessor::readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) diag_.error(SourceLoc{}, "cannot open '" + path + "'");
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string Preprocessor::canonicalPath(const std::string& path) const {
  std::error_code ec;
  fs::path p(path);
  fs::path c = fs::weakly_canonical(p, ec);
  if (!ec) return c.string();
  fs::path a = fs::absolute(p, ec);
  if (!ec) return a.lexically_normal().string();
  return path;
}

bool Preprocessor::tryFindInclude(const std::string& name, bool quoted,
                                  std::string& out, bool next) const {
  fs::path p(name);
  std::vector<fs::path> candidates;
  if (p.is_absolute()) {
    candidates.push_back(p);
  } else {
    if (quoted && !currentPath_.empty()) {
      fs::path base(currentPath_);
      candidates.push_back(base.parent_path() / p);
    }
    for (const auto& d : includeDirs_)
      candidates.push_back(fs::path(d.first) / p);
  }

  fs::path currentDir;
  if (!currentPath_.empty()) {
    std::error_code ec;
    currentDir = fs::weakly_canonical(fs::path(currentPath_).parent_path(), ec);
  }

  for (const auto& c : candidates) {
    std::error_code ec;
    if (!fs::is_regular_file(c, ec)) continue;
    std::error_code ec2;
    fs::path cc = fs::weakly_canonical(c, ec2);
    std::string canon = ec2 ? c.string() : cc.string();
    if (next) {
      std::error_code ec3;
      fs::path cdir = fs::weakly_canonical(c.parent_path(), ec3);
      if (!ec3 && !currentDir.empty() && cdir == currentDir) continue;
      if (!currentPath_.empty() && canon == currentPath_) continue;
    }
    out = canon;
    return true;
  }
  return false;
}

std::string Preprocessor::findInclude(const std::string& name, bool quoted,
                                      SourceLoc loc, bool next) {
  std::string out;
  if (tryFindInclude(name, quoted, out, next)) return out;
  std::string msg = "cannot find include file '" + name + "'";
  if (!includeDirs_.empty()) {
    msg += " (search path:";
    for (const auto& d : includeDirs_) msg += " " + d.first;
    msg += ")";
  }
  diag_.error(loc, msg);
}

std::vector<Token> Preprocessor::lexSnippet(const std::string& text, SourceLoc loc) {
  int id = sm_.addFile("<snippet>", text);
  Lexer lex(sm_, diag_, id);
  auto toks = lex.tokenize();
  std::vector<Token> out;
  for (auto& t : toks) {
    if (t.kind == Tok::Newline || t.kind == Tok::Eof) continue;
    if (loc.file >= 0) t.loc = loc;
    out.push_back(std::move(t));
  }
  return out;
}

std::vector<Token> Preprocessor::preprocessFile(const std::string& path) {
  std::string text = readFile(path);
  std::string canon = canonicalPath(path);
  if (onceFiles_.count(canon)) return {};
  if (includeDepth_ > 200) diag_.error(SourceLoc{}, "include nesting too deep");
  int id = sm_.addFile(canon, text);
  std::string save = currentPath_;
  currentPath_ = canon;
  includeStack_.push_back(canon);
  ++includeDepth_;
  std::vector<Token> toks = run(id, canon);
  --includeDepth_;
  includeStack_.pop_back();
  currentPath_ = save;
  return toks;
}

std::vector<Token> Preprocessor::preprocessText(const std::string& text,
                                                const std::string& name) {
  int id = sm_.addFile(name, text);
  std::string save = currentPath_;
  currentPath_ = name;
  std::vector<Token> toks = run(id, name);
  currentPath_ = save;
  return toks;
}

std::vector<Token> Preprocessor::run(int fileId, const std::string& canonical) {
  Lexer lex(sm_, diag_, fileId);
  std::vector<Token> toks = lex.tokenize();
  std::vector<Token> out;
  std::vector<Cond> conds;
  bool skipping = false;
  size_t i = 0;

  while (i < toks.size()) {
    const Token& t = toks[i];
    if (t.kind == Tok::Eof) break;
    if (t.kind == Tok::Newline) {
      ++i;
      continue;
    }

    if (t.bol && isPunct(t, "#")) {
      ++i;
      std::vector<Token> line;
      while (i < toks.size() && toks[i].kind != Tok::Newline && toks[i].kind != Tok::Eof) {
        line.push_back(toks[i]);
        ++i;
      }
      handleDirective(line, fileId, conds, skipping, out);
      continue;
    }

    if (skipping) {
      while (i < toks.size() && toks[i].kind != Tok::Newline && toks[i].kind != Tok::Eof)
        ++i;
      continue;
    }

    std::vector<Token> line;
    int parenDepth = 0;
    while (i < toks.size()) {
      while (i < toks.size() && toks[i].kind != Tok::Newline && toks[i].kind != Tok::Eof) {
        if (isPunct(toks[i], "(")) ++parenDepth;
        else if (isPunct(toks[i], ")")) --parenDepth;
        line.push_back(toks[i]);
        ++i;
      }
      if (parenDepth > 0 && i < toks.size() && toks[i].kind == Tok::Newline) {
        // A macro invocation (or parenthesized expression) continues on the
        // next physical line. Newlines are whitespace to the macro expander.
        ++i;
        if (i < toks.size() && toks[i].kind != Tok::Newline && toks[i].kind != Tok::Eof)
          toks[i].space = true;
        continue;
      }
      break;
    }
    std::vector<Token> ex = expand(std::move(line));
    out.insert(out.end(), ex.begin(), ex.end());
  }

  if (!conds.empty())
    diag_.error(SourceLoc{fileId, 1, 1}, "unterminated conditional directive");
  (void)canonical;
  return out;
}

void Preprocessor::handleDirective(const std::vector<Token>& line, int fileId,
                                   std::vector<Cond>& conds, bool& skipping,
                                   std::vector<Token>& out) {
  (void)fileId;
  if (line.empty()) return;
  if (line[0].kind != Tok::Ident) {
    diag_.error(line[0].loc, "expected directive name after '#'");
  }
  const std::string& name = line[0].text;
  SourceLoc loc = line[0].loc;

  auto eval = [&](size_t from) {
    std::vector<Token> rest(line.begin() + static_cast<long>(from), line.end());
    return evalCondition(std::move(rest), loc);
  };

  if (name == "if") {
    bool parent = !skipping;
    bool cond = parent ? eval(1) : false;
    Cond c;
    c.parentActive = parent;
    c.active = parent && cond;
    c.taken = c.active;
    conds.push_back(c);
    skipping = !c.active;
    return;
  }
  if (name == "ifdef" || name == "ifndef") {
    bool parent = !skipping;
    bool cond = false;
    if (line.size() < 2 || line[1].kind != Tok::Ident)
      diag_.error(loc, "expected identifier after #" + name);
    if (parent) {
      cond = macros_.count(line[1].text) != 0;
      if (name == "ifndef") cond = !cond;
    }
    Cond c;
    c.parentActive = parent;
    c.active = parent && cond;
    c.taken = c.active;
    conds.push_back(c);
    skipping = !c.active;
    return;
  }
  if (name == "elif") {
    if (conds.empty()) diag_.error(loc, "#elif without #if");
    Cond& c = conds.back();
    if (c.elseSeen) diag_.error(loc, "#elif after #else");
    if (!c.parentActive || c.taken) {
      c.active = false;
    } else {
      bool cond = eval(1);
      c.active = cond;
      c.taken = c.taken || cond;
    }
    skipping = !c.active;
    return;
  }
  if (name == "else") {
    if (conds.empty()) diag_.error(loc, "#else without #if");
    Cond& c = conds.back();
    if (c.elseSeen) diag_.error(loc, "#else after #else");
    c.elseSeen = true;
    c.active = c.parentActive && !c.taken;
    c.taken = true;
    skipping = !c.active;
    return;
  }
  if (name == "endif") {
    if (conds.empty()) diag_.error(loc, "#endif without #if");
    conds.pop_back();
    skipping = conds.empty() ? false : !conds.back().active;
    return;
  }

  if (skipping) return;

  if (name == "define") {
    handleDefine(line);
    return;
  }
  if (name == "undef") {
    if (line.size() < 2 || line[1].kind != Tok::Ident)
      diag_.error(loc, "expected identifier after #undef");
    macros_.erase(line[1].text);
    return;
  }
  if (name == "include" || name == "include_next") {
    handleInclude(line, loc, name == "include_next", out);
    return;
  }
  if (name == "pragma") {
    if (line.size() >= 2 && isIdent(line[1], "once")) {
      if (!currentPath_.empty()) onceFiles_.insert(currentPath_);
    }
    return;
  }
  if (name == "error") {
    std::string msg;
    for (size_t i = 1; i < line.size(); ++i) {
      if (i > 1 && line[i].space) msg += ' ';
      msg += line[i].text;
    }
    bool sys = loc.file >= 0 && (sm_.file(loc.file).path.rfind("/usr/include",0)==0 ||
               sm_.file(loc.file).path.rfind("/data/data/com.termux/files/usr/include",0)==0 ||
               sm_.file(loc.file).path.find("/c++/v1/")!=std::string::npos);
    if (sys) diag_.warning(loc, std::string("system header #error ignored: ") + (msg.empty() ? "#error" : msg));
    else diag_.error(loc, msg.empty() ? "#error" : msg);
    return;
  }
  if (name == "warning") {
    std::string msg;
    for (size_t i = 1; i < line.size(); ++i) {
      if (i > 1 && line[i].space) msg += ' ';
      msg += line[i].text;
    }
    diag_.warning(loc, msg.empty() ? "#warning" : msg);
    return;
  }
  if (name == "line") {
    return;
  }
  if (name.empty()) return;

  if (verbose_)
    diag_.warning(loc, "ignoring unknown directive #" + name);
}

void Preprocessor::handleDefine(const std::vector<Token>& line) {
  if (line.size() < 2 || line[1].kind != Tok::Ident)
    diag_.error(line[0].loc, "expected macro name after #define");
  Macro m;
  m.name = line[1].text;
  m.loc = line[0].loc;
  if (m.name == "defined")
    diag_.error(line[1].loc, "'defined' cannot be used as a macro name");

  size_t i = 2;
  if (i < line.size() && isPunct(line[i], "(") && !line[i].space) {
    m.isFunction = true;
    ++i;
    if (i < line.size() && isPunct(line[i], ")")) {
      ++i;
    } else {
      while (i < line.size()) {
        if (isPunct(line[i], "...")) {
          m.isVariadic = true;
          ++i;
          break;
        }
        if (line[i].kind != Tok::Ident)
          diag_.error(line[i].loc, "expected parameter name in macro definition");
        std::string pname = line[i].text;
        ++i;
        if (i < line.size() && isPunct(line[i], "...")) {
          m.isVariadic = true;
          m.variadicName = pname;
          ++i;
          break;
        }
        if (std::find(m.params.begin(), m.params.end(), pname) != m.params.end())
          diag_.error(line[i - 1].loc, "duplicate macro parameter '" + pname + "'");
        m.params.push_back(pname);
        if (i < line.size() && isPunct(line[i], ",")) {
          ++i;
          continue;
        }
        break;
      }
      if (i >= line.size() || !isPunct(line[i], ")"))
        diag_.error(line[0].loc, "expected ')' in macro parameter list");
      ++i;
    }
  }

  m.body.assign(line.begin() + static_cast<long>(i), line.end());
  for (const auto& t : m.body) {
    if (isIdent(t, "__VA_ARGS__") && !m.isVariadic)
      diag_.error(t.loc, "__VA_ARGS__ used outside a variadic macro");
    if (isIdent(t, "__VA_OPT__") && !m.isVariadic)
      diag_.error(t.loc, "__VA_OPT__ used outside a variadic macro");
  }
  macros_[m.name] = std::move(m);
}

void Preprocessor::handleInclude(const std::vector<Token>& line, SourceLoc loc,
                                 bool next, std::vector<Token>& out) {
  std::vector<Token> args(line.begin() + 1, line.end());
  std::string name;
  bool quoted = false;
  std::vector<Token> ex;
  bool directForm = !args.empty() && (args[0].kind == Tok::String || isPunct(args[0], "<"));
  if (directForm) {
    if (!headerFromTokens(args, name, quoted))
      diag_.error(loc, "expected \"file\" or <file> after #include");
  } else {
    ex = expand(std::move(args));
    if (!headerFromTokens(ex, name, quoted))
      diag_.error(loc, "expected \"file\" or <file> after #include");
  }

  (void)next;
  std::string path = findInclude(name, quoted, loc, next);
  std::string canon = canonicalPath(path);
  if (onceFiles_.count(canon)) return;
  if (includeDepth_ > 200)
    diag_.error(loc, "include nesting too deep (possible cycle)");

  std::string text = readFile(canon);
  int id = sm_.addFile(canon, text);
  std::string save = currentPath_;
  currentPath_ = canon;
  includeStack_.push_back(canon);
  ++includeDepth_;
  std::vector<Token> toks = run(id, canon);
  --includeDepth_;
  includeStack_.pop_back();
  currentPath_ = save;
  out.insert(out.end(), toks.begin(), toks.end());
}

bool Preprocessor::evalCondition(std::vector<Token> line, SourceLoc loc) {
  std::vector<Token> pre;
  for (size_t i = 0; i < line.size(); ++i) {
    if (isIdent(line[i], "defined")) {
      size_t j = i + 1;
      bool paren = false;
      if (j < line.size() && isPunct(line[j], "(")) {
        paren = true;
        ++j;
      }
      if (j >= line.size() || line[j].kind != Tok::Ident)
        diag_.error(line[i].loc, "expected identifier after 'defined'");
      bool def = macros_.count(line[j].text) != 0;
      ++j;
      if (paren) {
        if (j >= line.size() || !isPunct(line[j], ")"))
          diag_.error(line[i].loc, "expected ')' after defined(");
        ++j;
      }
      pre.push_back(numToken(def ? "1" : "0", line[i].loc));
      i = j - 1;
      continue;
    }
    if ((isIdent(line[i], "__has_include") || isIdent(line[i], "__has_include_next")) &&
        i + 1 < line.size() && isPunct(line[i + 1], "(")) {
      size_t j = i + 2;
      int depth = 0;
      std::vector<Token> inner;
      bool closed = false;
      while (j < line.size()) {
        if (isPunct(line[j], "(")) {
          ++depth;
          inner.push_back(line[j]);
        } else if (isPunct(line[j], ")")) {
          if (depth == 0) {
            closed = true;
            break;
          }
          --depth;
          inner.push_back(line[j]);
        } else {
          inner.push_back(line[j]);
        }
        ++j;
      }
      if (!closed) diag_.error(line[i].loc, "unterminated __has_include");
      std::string name;
      bool quoted = false;
      std::string found;
      bool next = isIdent(line[i], "__has_include_next");
      bool ok = headerFromTokens(inner, name, quoted) &&
                tryFindInclude(name, quoted, found, next);
      pre.push_back(numToken(ok ? "1" : "0", line[i].loc));
      i = j;
      continue;
    }
    pre.push_back(line[i]);
  }

  std::vector<Token> ex = expand(std::move(pre));
  for (auto& t : ex) {
    if (t.kind == Tok::Ident) {
      t.kind = Tok::Number;
      t.text = (t.text == "true") ? "1" : "0";
    }
  }
  if (ex.empty()) diag_.error(loc, "empty #if expression");
  PPExpr parser(ex, diag_);
  return parser.parse() != 0;
}

int Preprocessor::paramIndex(const Macro& m, const std::string& name) const {
  for (size_t i = 0; i < m.params.size(); ++i)
    if (m.params[i] == name) return static_cast<int>(i);
  if (m.isVariadic) {
    if (m.variadicName.empty()) {
      if (name == "__VA_ARGS__") return static_cast<int>(m.params.size());
    } else if (name == m.variadicName) {
      return static_cast<int>(m.params.size());
    }
  }
  return -1;
}

std::vector<std::vector<Token>> Preprocessor::parseMacroArgs(const Macro& m,
                                                             std::deque<Token>& work,
                                                             SourceLoc loc) {
  if (work.empty() || !isPunct(work.front(), "("))
    diag_.error(loc, "internal: expected '(' for macro arguments");
  work.pop_front();
  std::vector<std::vector<Token>> args;
  std::vector<Token> cur;
  int paren = 0;
  bool closed = false;
  while (!work.empty()) {
    Token t = work.front();
    work.pop_front();
    if (isPunct(t, "(")) {
      ++paren;
      cur.push_back(t);
      continue;
    }
    if (isPunct(t, ")")) {
      if (paren == 0) {
        closed = true;
        break;
      }
      --paren;
      cur.push_back(t);
      continue;
    }
    if (isPunct(t, ",") && paren == 0) {
      args.push_back(cur);
      cur.clear();
      continue;
    }
    cur.push_back(t);
  }
  if (!closed) diag_.error(loc, "unterminated macro invocation");
  if (!cur.empty() || !args.empty()) args.push_back(cur);
  (void)m;
  return args;
}

std::vector<Token> Preprocessor::substitute(
    const Macro& m, const std::vector<std::vector<Token>>& raw,
    const std::vector<std::vector<Token>>& expanded) {
  std::vector<Token> tmp;

  for (size_t i = 0; i < m.body.size(); ++i) {
    Token b = m.body[i];
    if (isPunct(b, "#") && i + 1 < m.body.size()) {
      int idx = paramIndex(m, m.body[i + 1].text);
      if (idx >= 0) {
        tmp.push_back(stringize(raw[static_cast<size_t>(idx)], b.loc));
        ++i;
        continue;
      }
    }
    if (isIdent(b, "__VA_OPT__") && i + 1 < m.body.size() && isPunct(m.body[i + 1], "(")) {
      size_t j = i + 2;
      int depth = 1;
      std::vector<Token> content;
      while (j < m.body.size() && depth > 0) {
        if (isPunct(m.body[j], "(")) {
          ++depth;
          content.push_back(m.body[j]);
        } else if (isPunct(m.body[j], ")")) {
          --depth;
          if (depth == 0) break;
          content.push_back(m.body[j]);
        } else {
          content.push_back(m.body[j]);
        }
        ++j;
      }
      if (j >= m.body.size()) diag_.error(b.loc, "unterminated __VA_OPT__");
      if (!raw.empty() && !raw.back().empty()) {
        Macro sub = m;
        sub.body = content;
        auto part = substitute(sub, raw, expanded);
        tmp.insert(tmp.end(), part.begin(), part.end());
      }
      i = j;
      continue;
    }

    int idx = paramIndex(m, b.text);
    if (idx >= 0) {
      bool paste = (i > 0 && isPunct(m.body[i - 1], "##")) ||
                   (i + 1 < m.body.size() && isPunct(m.body[i + 1], "##"));
      const std::vector<Token>& arg = paste ? raw[static_cast<size_t>(idx)]
                                            : expanded[static_cast<size_t>(idx)];
      if (arg.empty()) {
        Token pm;
        pm.kind = Tok::Placemarker;
        pm.loc = b.loc;
        tmp.push_back(pm);
      } else {
        tmp.insert(tmp.end(), arg.begin(), arg.end());
      }
      continue;
    }
    tmp.push_back(b);
  }

  std::vector<Token> out;
  for (size_t i = 0; i < tmp.size(); ++i) {
    if (isPunct(tmp[i], "##")) {
      if (out.empty()) continue;
      Token left = out.back();
      out.pop_back();
      if (i + 1 >= tmp.size()) {
        out.push_back(left);
        break;
      }
      Token right = tmp[++i];
      if (left.kind == Tok::Placemarker && right.kind == Tok::Placemarker) {
        Token pm;
        pm.kind = Tok::Placemarker;
        pm.loc = left.loc;
        out.push_back(pm);
      } else if (left.kind == Tok::Placemarker) {
        out.push_back(right);
      } else if (right.kind == Tok::Placemarker) {
        if (!isPunct(left, ",")) out.push_back(left);
      } else {
        std::vector<Token> pasted = lexSnippet(left.text + right.text, left.loc);
        if (pasted.size() != 1)
          diag_.error(left.loc, "token pasting does not form a single token");
        Token nt = pasted[0];
        nt.space = left.space;
        nt.bol = left.bol;
        for (const auto& h : left.hide) addHide(nt, h);
        out.push_back(nt);
      }
    } else if (tmp[i].kind == Tok::Placemarker) {
      // drop
    } else {
      out.push_back(tmp[i]);
    }
  }
  return out;
}

std::vector<Token> Preprocessor::expand(std::vector<Token> in, int depth) {
  if (depth > 400)
    diag_.error(in.empty() ? SourceLoc{} : in[0].loc, "macro expansion too deep");

  std::deque<Token> work(in.begin(), in.end());
  std::vector<Token> out;

  while (!work.empty()) {
    Token t = work.front();
    work.pop_front();

    if (t.kind != Tok::Ident) {
      out.push_back(t);
      continue;
    }

    if (t.text == "__LINE__") {
      out.push_back(numToken(std::to_string(t.loc.line), t.loc));
      continue;
    }
    if (t.text == "__FILE__") {
      out.push_back(strToken("\"" + escapeForString(currentPath_) + "\"", t.loc));
      continue;
    }
    if (t.text == "__COUNTER__") {
      out.push_back(numToken(std::to_string(counter_++), t.loc));
      continue;
    }
    if (t.text == "__DATE__") {
      out.push_back(strToken("\"" + escapeForString(date_) + "\"", t.loc));
      continue;
    }
    if (t.text == "__TIME__") {
      out.push_back(strToken("\"" + escapeForString(time_) + "\"", t.loc));
      continue;
    }

    if ((t.text == "__has_include" || t.text == "__has_include_next") &&
        !work.empty() && isPunct(work.front(), "(")) {
      work.pop_front();
      std::vector<Token> inner;
      int paren = 0;
      bool closed = false;
      while (!work.empty()) {
        Token a = work.front();
        work.pop_front();
        if (isPunct(a, "(")) {
          ++paren;
          inner.push_back(a);
        } else if (isPunct(a, ")")) {
          if (paren == 0) {
            closed = true;
            break;
          }
          --paren;
          inner.push_back(a);
        } else {
          inner.push_back(a);
        }
      }
      if (!closed) diag_.error(t.loc, "unterminated __has_include");
      std::string name;
      bool quoted = false;
      std::string found;
      bool next = t.text == "__has_include_next";
      bool ok = headerFromTokens(inner, name, quoted) &&
                tryFindInclude(name, quoted, found, next);
      out.push_back(numToken(ok ? "1" : "0", t.loc));
      continue;
    }

    auto it = macros_.find(t.text);
    if (it == macros_.end() || hidden(t, t.text)) {
      out.push_back(t);
      continue;
    }
    const Macro& m = it->second;

    if (!m.isFunction) {
      std::vector<Token> repl = m.body;
      for (auto& r : repl) {
        r.loc = t.loc;
        r.bol = false;
        r.fromMacro = true;
        addHide(r, m.name);
        mergeHide(r, t.hide);
      }
      if (!repl.empty()) repl.front().space = t.space;
      for (auto ri = repl.rbegin(); ri != repl.rend(); ++ri)
        work.push_front(*ri);
      continue;
    }

    if (work.empty() || !isPunct(work.front(), "(")) {
      out.push_back(t);
      continue;
    }

    std::vector<std::vector<Token>> raw = parseMacroArgs(m, work, t.loc);
    size_t fixed = m.params.size();
    while (raw.size() < fixed) raw.push_back({});
    if (!m.isVariadic && raw.size() > fixed)
      diag_.error(t.loc, "too many arguments for macro '" + m.name + "'");
    if (m.isVariadic && raw.size() < fixed)
      diag_.error(t.loc, "too few arguments for macro '" + m.name + "'");

    std::vector<std::vector<Token>> args;
    for (size_t i = 0; i < fixed; ++i) args.push_back(raw[i]);
    if (m.isVariadic) {
      std::vector<Token> va;
      for (size_t i = fixed; i < raw.size(); ++i) {
        if (!va.empty()) va.push_back(punctToken(",", t.loc));
        va.insert(va.end(), raw[i].begin(), raw[i].end());
      }
      args.push_back(va);
    }

    std::vector<std::vector<Token>> expArgs;
    expArgs.reserve(args.size());
    for (const auto& a : args) expArgs.push_back(expand(a, depth + 1));

    std::vector<Token> repl = substitute(m, args, expArgs);
    for (auto& r : repl) {
      r.loc = t.loc;
      r.bol = false;
      r.fromMacro = true;
      addHide(r, m.name);
      mergeHide(r, t.hide);
    }
    if (!repl.empty()) repl.front().space = t.space;
    for (auto ri = repl.rbegin(); ri != repl.rend(); ++ri)
      work.push_front(*ri);
  }

  return out;
}

std::string Preprocessor::printTokens(const std::vector<Token>& toks) {
  std::string out;
  int lastLine = -1;
  bool first = true;
  for (const auto& t : toks) {
    if (t.kind == Tok::Eof || t.kind == Tok::Newline) continue;
    if (t.loc.line != lastLine) {
      if (!first) out += '\n';
      lastLine = t.loc.line;
      first = false;
    } else if (t.space) {
      out += ' ';
    }
    out += t.text;
  }
  if (!out.empty()) out += '\n';
  return out;
}

} // namespace tc
