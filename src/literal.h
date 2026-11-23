#pragma once
#include <string>

namespace tc {

struct IntLiteral {
  unsigned long long value = 0;
  bool isUnsigned = false;
  bool isLong = false;
  bool isLongLong = false;
  bool ok = false;
};

IntLiteral parseIntLiteral(const std::string& text);

struct FloatLiteral {
  double value = 0;
  bool isFloat = false;
  bool ok = false;
};

FloatLiteral parseFloatLiteral(const std::string& text);

long long parseCharLiteral(const std::string& text, bool& ok);

} // namespace tc
