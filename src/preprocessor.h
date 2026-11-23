#pragma once
#include "common.h"
#include "token.h"
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace tc {

struct Macro {
  std::string name;
  bool isFunction = false;
  bool isVariadic = false;
  std::string variadicName;
  std::vector<std::string> params;
  std::vector<Token> body;
  SourceLoc loc;
};

class Preprocessor {
public:
  Preprocessor(SourceManager& sm, Diag& diag);

  void addIncludeDir(std::string dir, bool system);
  void define(const std::string& name, const std::string& value);
  void undef(const std::string& name);
  bool isDefined(const std::string& name) const;
  void setVerbose(bool v) { verbose_ = v; }

  std::vector<Token> preprocessFile(const std::string& path);
  std::vector<Token> preprocessText(const std::string& text, const std::string& name);

  static std::string printTokens(const std::vector<Token>& toks);

private:
  struct Cond {
    bool parentActive = true;
    bool active = true;
    bool taken = false;
    bool elseSeen = false;
  };

  std::vector<Token> run(int fileId, const std::string& canonical);
  void handleDirective(const std::vector<Token>& line, int fileId,
                       std::vector<Cond>& conds, bool& skipping,
                       std::vector<Token>& out);
  void handleDefine(const std::vector<Token>& line);
  void handleInclude(const std::vector<Token>& line, SourceLoc loc,
                     bool next, std::vector<Token>& out);
  bool evalCondition(std::vector<Token> line, SourceLoc loc);

  std::vector<Token> expand(std::vector<Token> in, int depth = 0);
  std::vector<Token> substitute(const Macro& m,
                                const std::vector<std::vector<Token>>& raw,
                                const std::vector<std::vector<Token>>& expanded);
  std::vector<std::vector<Token>> parseMacroArgs(const Macro& m,
                                                 std::deque<Token>& work,
                                                 SourceLoc loc);
  int paramIndex(const Macro& m, const std::string& name) const;

  std::string findInclude(const std::string& name, bool quoted, SourceLoc loc,
                          bool next = false);
  bool tryFindInclude(const std::string& name, bool quoted, std::string& out,
                      bool next = false) const;
  std::string canonicalPath(const std::string& path) const;
  std::string readFile(const std::string& path);
  std::vector<Token> lexSnippet(const std::string& text, SourceLoc loc);

  SourceManager& sm_;
  Diag& diag_;
  std::map<std::string, Macro> macros_;
  std::vector<std::pair<std::string, bool>> includeDirs_;
  std::set<std::string> onceFiles_;
  std::vector<std::string> includeStack_;
  std::string currentPath_;
  int counter_ = 0;
  int includeDepth_ = 0;
  bool verbose_ = false;
  std::string date_;
  std::string time_;
};

} // namespace tc
