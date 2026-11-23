#include "literal.h"
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace tc {
namespace {

int digitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int hexValue(const std::string& s, size_t& i, int maxDigits) {
  int v = 0;
  int n = 0;
  while (i < s.size() && n < maxDigits) {
    int d = digitValue(s[i]);
    if (d < 0) break;
    v = v * 16 + d;
    ++i;
    ++n;
  }
  return v;
}

} // namespace

IntLiteral parseIntLiteral(const std::string& raw) {
  IntLiteral r;
  std::string s;
  for (char c : raw)
    if (c != '\'') s += c;

  while (!s.empty()) {
    char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s.back())));
    if (c == 'u') {
      r.isUnsigned = true;
      s.pop_back();
    } else if (c == 'l') {
      if (!s.empty() && std::tolower(static_cast<unsigned char>(s.back())) == 'l')
        r.isLongLong = true;
      else
        r.isLong = true;
      s.pop_back();
    } else {
      break;
    }
  }
  if (s.empty()) return r;

  int base = 10;
  size_t i = 0;
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    i = 2;
  } else if (s.size() >= 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
    base = 2;
    i = 2;
  } else if (s.size() > 1 && s[0] == '0') {
    base = 8;
    i = 1;
  }

  unsigned long long v = 0;
  bool any = false;
  for (; i < s.size(); ++i) {
    int d = digitValue(s[i]);
    if (d < 0 || d >= base) return r;
    v = v * static_cast<unsigned long long>(base) + static_cast<unsigned long long>(d);
    any = true;
  }
  if (!any) return r;
  r.value = v;
  r.ok = true;
  return r;
}

FloatLiteral parseFloatLiteral(const std::string& raw) {
  FloatLiteral r;
  std::string s;
  for (char c : raw)
    if (c != '\'') s += c;
  if (!s.empty() && (s.back() == 'f' || s.back() == 'F')) {
    r.isFloat = true;
    s.pop_back();
  }
  if (s.empty()) return r;
  char* end = nullptr;
  r.value = std::strtod(s.c_str(), &end);
  if (end && *end == '\0') r.ok = true;
  return r;
}

long long parseCharLiteral(const std::string& raw, bool& ok) {
  ok = false;
  std::string s = raw;
  if (s.size() < 2 || s.front() != '\'') return 0;
  // Strip prefix (u8, u, U, L).
  size_t start = 1;
  while (start < s.size() && s[start] != '\'') ++start;
  if (start >= s.size()) return 0;
  size_t i = start + 1;
  if (i >= s.size() || s.back() != '\'') return 0;
  size_t end = s.size() - 1;

  unsigned long long value = 0;
  int count = 0;
  while (i < end) {
    unsigned int c = static_cast<unsigned char>(s[i]);
    if (c == '\\') {
      ++i;
      if (i >= end) return 0;
      char e = s[i++];
      switch (e) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case 'a': c = '\a'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'v': c = '\v'; break;
        case '\\': c = '\\'; break;
        case '\'': c = '\''; break;
        case '"': c = '"'; break;
        case '?': c = '?'; break;
        case 'x': c = static_cast<unsigned int>(hexValue(s, i, 2)); break;
        case 'u': c = static_cast<unsigned int>(hexValue(s, i, 4)); break;
        case 'U': c = static_cast<unsigned int>(hexValue(s, i, 8)); break;
        default:
          if (e >= '0' && e <= '7') {
            c = static_cast<unsigned int>(e - '0');
            for (int k = 0; k < 2 && i < end && s[i] >= '0' && s[i] <= '7'; ++k)
              c = c * 8 + static_cast<unsigned int>(s[i++] - '0');
          } else {
            c = static_cast<unsigned int>(static_cast<unsigned char>(e));
          }
          break;
      }
    } else {
      ++i;
    }
    value = (value << 8) | (c & 0xff);
    ++count;
  }
  if (count == 0) return 0;
  ok = true;
  return static_cast<long long>(value);
}

} // namespace tc
