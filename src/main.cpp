#include "common.h"
#include "lexer.h"
#include "preprocessor.h"
#include "parser.h"
#include "codegen.h"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string shellQuote(const std::string& s) {
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') r += "'\\''";
    else r += c;
  }
  r += "'";
  return r;
}

static void usage() {
  std::cout <<
    "tcpp - a tiny C++ compiler\n"
    "\n"
    "Usage: tcpp [options] file...\n"
    "\n"
    "  -E              preprocess only, write to stdout\n"
    "  --lex           dump preprocessed tokens (debug)\n"
    "  --emit-c        emit generated C instead of compiling\n"
    "  -c              compile to an object file\n"
    "  -shared         produce a shared library\n"
    "  -fPIC           generate position-independent code\n"
    "  -run, --run     compile and run the program\n"
    "  --keep-c        keep the generated C file\n"
    "  -o FILE         output file (default: a.out)\n"
    "  -I DIR          add include search directory\n"
    "  -D NAME[=VAL]   define a macro\n"
    "  -U NAME         undefine a macro\n"
    "  -O<level>       pass optimization level to host C compiler\n"
    "  -L DIR          pass library path to host C compiler\n"
    "  -l NAME         pass library to host C compiler\n"
    "  -Wl,...         pass linker flags to host C compiler\n"
    "  --cc CMD        host C compiler (default: $CC or cc)\n"
    "  --from-libc++   prefer system libc++ headers over tinycpp stl stubs\n"
    "  --abort-assert  abort on failed assertions (default: warn and continue)\n"
    "  --version       show version\n"
    "  -v              verbose diagnostics\n"
    "  -h, --help      show this help\n";
}

static std::string dirName(const std::string& path) {
  fs::path p(path);
  return p.parent_path().string();
}

static void splitDefine(const std::string& s, std::string& name, std::string& value) {
  size_t eq = s.find('=');
  if (eq == std::string::npos) {
    name = s;
    value = "1";
  } else {
    name = s.substr(0, eq);
    value = s.substr(eq + 1);
  }
}

int main(int argc, char** argv) {
  tc::SourceManager sm;
  tc::Diag diag;
  diag.setSourceManager(&sm);
  tc::Preprocessor pp(sm, diag);

  std::vector<std::string> inputs;
  std::vector<std::string> hostArgs;
  std::string output;
  std::string ccCmd;
  bool preprocessOnly = false;
  bool lexOnly = false;
  bool emitC = false;
  bool compileOnly = false;
  bool run = false;
  bool keepC = false;
  bool verbose = false;
  bool abortAssert = false;
  bool fromLibcxx = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-E") {
      preprocessOnly = true;
    } else if (a == "--lex") {
      lexOnly = true;
    } else if (a == "--emit-c") {
      emitC = true;
    } else if (a == "-c") {
      compileOnly = true;
    } else if (a == "--run" || a == "-run") {
      run = true;
    } else if (a == "--keep-c") {
      keepC = true;
    } else if (a == "--abort-assert") {
      abortAssert = true;
    } else if (a == "--from-libc++") {
      fromLibcxx = true;
    } else if (a == "-v") {
      verbose = true;
    } else if (a == "--version") {
      std::cout << "tcpp 0.2.0\n";
      return 0;
    } else if (a == "--cc") {
      if (++i >= argc) { std::cerr << "tcpp: missing argument to --cc\n"; return 1; }
      ccCmd = argv[i];
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else if (a == "-o") {
      if (++i >= argc) {
        std::cerr << "tcpp: missing argument to -o\n";
        return 1;
      }
      output = argv[i];
    } else if (a == "-I") {
      if (++i >= argc) {
        std::cerr << "tcpp: missing argument to -I\n";
        return 1;
      }
      pp.addIncludeDir(argv[i], false);
    } else if (a.rfind("-I", 0) == 0 && a.size() > 2) {
      pp.addIncludeDir(a.substr(2), false);
    } else if (a == "-D") {
      if (++i >= argc) {
        std::cerr << "tcpp: missing argument to -D\n";
        return 1;
      }
      std::string name, value;
      splitDefine(argv[i], name, value);
      pp.define(name, value);
    } else if (a.rfind("-D", 0) == 0 && a.size() > 2) {
      std::string name, value;
      splitDefine(a.substr(2), name, value);
      pp.define(name, value);
    } else if (a == "-U") {
      if (++i >= argc) {
        std::cerr << "tcpp: missing argument to -U\n";
        return 1;
      }
      pp.undef(argv[i]);
    } else if (a.rfind("-U", 0) == 0 && a.size() > 2) {
      pp.undef(a.substr(2));
    } else if (a == "-L" || a == "-l") {
      if (++i >= argc) { std::cerr << "tcpp: missing argument to " << a << "\n"; return 1; }
      hostArgs.push_back(a + std::string(argv[i]));
    } else if (a.rfind("-L", 0) == 0 || a.rfind("-l", 0) == 0 || a.rfind("-Wl,", 0) == 0) {
      hostArgs.push_back(a);
    } else if (a.rfind("-O", 0) == 0) {
      hostArgs.push_back(a);
    } else if (a == "-shared" || a == "-fPIC" || a == "-fpic" || a == "-rdynamic" || a == "-pthread") {
      hostArgs.push_back(a);
    } else if (a.rfind("-std=", 0) == 0) {
      // accepted for compatibility
    } else if (!a.empty() && a[0] == '-') {
      std::cerr << "tcpp: unknown option '" << a << "'\n";
      return 1;
    } else {
      inputs.push_back(a);
    }
  }

  pp.setVerbose(verbose);
  pp.define("__tcpp__", "1");
  pp.define("__TCPP__", "1");
  pp.define("__tinycpp__", "1");
  pp.define("__TINYCPP__", "1");
  if (!pp.isDefined("_LIBCPP_HARDENING_MODE_DEFAULT") &&
      fs::exists("/data/data/com.termux/files/usr/include/c++/v1/__config")) {
    pp.define("_LIBCPP_HARDENING_MODE_DEFAULT", "_LIBCPP_HARDENING_MODE_NONE");
  }

  auto addIfDir = [&](const std::string& d, bool after = false) {
    std::error_code ec;
    if (fs::is_directory(d, ec)) pp.addIncludeDir(d, after);
  };

  // Default include paths: ./stl, ./include and <exe>/../include, plus TINYCPP_INCLUDE.
  if (!fromLibcxx) {
    pp.addIncludeDir("stl", false);
    pp.addIncludeDir("include", false);
  }
  {
    std::string exeDir = argc > 0 ? dirName(argv[0]) : std::string();
    if (exeDir.empty()) {
      std::error_code ec;
      auto self = fs::read_symlink("/proc/self/exe", ec);
      if (!ec) exeDir = dirName(self.string());
    }
    if (!exeDir.empty()) {
      if (!fromLibcxx) {
        pp.addIncludeDir(exeDir + "/../stl", false);
        pp.addIncludeDir(exeDir + "/../share/tcpp/stl", false);
      }
      pp.addIncludeDir(exeDir + "/../include", false);
    }
  }
  if (const char* env = std::getenv("TINYCPP_INCLUDE")) {
    std::string senv = env;
    size_t start = 0;
    while (start <= senv.size()) {
      size_t colon = senv.find(':', start);
      std::string part = senv.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
      if (!part.empty()) pp.addIncludeDir(part, false);
      if (colon == std::string::npos) break;
      start = colon + 1;
    }
  }

  // Common system paths (best-effort, portability-first).
  addIfDir("/data/data/com.termux/files/usr/include/c++/v1");
  addIfDir("/data/data/com.termux/files/usr/lib/clang/21/include");
  addIfDir("/data/data/com.termux/files/usr/include/aarch64-linux-android");
  addIfDir("/data/data/com.termux/files/usr/include/arm-linux-androideabi");
  addIfDir("/data/data/com.termux/files/usr/include/x86_64-linux-android");
  addIfDir("/data/data/com.termux/files/usr/include/i686-linux-android");
  addIfDir("/data/data/com.termux/files/usr/include");
  addIfDir("/usr/include/c++/v1");
  addIfDir("/usr/include");

  if (inputs.empty()) {
    usage();
    return 1;
  }

  bool textMode = preprocessOnly || lexOnly || emitC;

  try {
    std::string all;
    std::vector<fs::path> cFiles;

    fs::path tmpDir;
    if (!textMode) {
      tmpDir = fs::temp_directory_path() /
               ("tcpp-" + std::to_string(static_cast<long long>(std::time(nullptr))) +
                "-" + std::to_string(inputs.size()));
      std::error_code ec;
      fs::create_directories(tmpDir, ec);
      if (ec) {
        std::cerr << "tcpp: cannot create temp directory: " << ec.message() << "\n";
        return 1;
      }
    }

    size_t fileIndex = 0;
    for (const auto& input : inputs) {
      std::vector<tc::Token> toks = pp.preprocessFile(input);
      if (preprocessOnly) {
        all += tc::Preprocessor::printTokens(toks);
        continue;
      }
      if (lexOnly) {
        for (const auto& t : toks) {
          if (t.kind == tc::Tok::Eof || t.kind == tc::Tok::Newline) continue;
          std::ostringstream line;
          line << sm.locString(t.loc) << "  " << static_cast<int>(t.kind)
               << "  '" << t.text << "'\n";
          all += line.str();
        }
        continue;
      }

      // Filter out tokens from system headers - we provide builtins and
      // include standard headers in generated C. This keeps the tiny parser
      // from choking on full C++ standard library headers.
      std::vector<tc::Token> filtered;
      filtered.reserve(toks.size());
      for (auto& t : toks) {
        if (t.kind == tc::Tok::Eof) { filtered.push_back(t); continue; }
        bool system = false;
        if (t.loc.file >= 0) {
          const std::string& p = sm.file(t.loc.file).path;
          if (p.rfind("/usr/include", 0) == 0 ||
              p.rfind("/data/data/com.termux/files/usr/include", 0) == 0 ||
              p.rfind("/data/data/com.termux/files/usr/lib/clang", 0) == 0 ||
              p.find("/c++/v1/") != std::string::npos) {
            system = true;
          }
        }
        if (!system) filtered.push_back(std::move(t));
      }
      tc::Parser parser(sm, diag, std::move(filtered));
      tc::Program prog = parser.parse();
      tc::CodeGen cg(prog, diag);
      cg.setAbortAssert(abortAssert);
      std::string c = cg.generate();

      if (emitC) {
        if (inputs.size() > 1)
          all += "/* " + input + " */\n";
        all += c;
        continue;
      }

      fs::path cPath = tmpDir / ("tu" + std::to_string(fileIndex++) + ".c");
      std::ofstream f(cPath, std::ios::binary);
      if (!f) {
        std::cerr << "tcpp: cannot write temp file '" << cPath.string() << "'\n";
        return 1;
      }
      f << c;
      f.close();
      cFiles.push_back(cPath);
    }

    if (textMode) {
      if (!output.empty()) {
        std::ofstream f(output, std::ios::binary);
        if (!f) {
          std::cerr << "tcpp: cannot write '" << output << "'\n";
          return 1;
        }
        f << all;
      } else {
        std::cout << all;
      }
      return 0;
    }

    // Invoke the host C compiler on the generated translation units.
    std::string outFile = output.empty() ? "a.out" : output;
    const char* ccEnv = std::getenv("CC");
    std::string cc = !ccCmd.empty() ? ccCmd : ((ccEnv && *ccEnv) ? ccEnv : "cc");

    std::string cmd = cc;
    cmd += compileOnly ? " -c" : " -O2";
    cmd += " -w";
    for (const auto& p : cFiles) {
      cmd += " ";
      cmd += shellQuote(p.string());
    }
    cmd += " -o ";
    cmd += shellQuote(outFile);
    if (!compileOnly) cmd += " -lm";
    for (const auto& a : hostArgs) { cmd += " "; cmd += shellQuote(a); }

    if (verbose) std::cerr << "tcpp: " << cmd << "\n";

    int rc = std::system(cmd.c_str());
    if (rc != 0) {
      std::cerr << "tcpp: host compiler failed (exit " << rc << ")\n";
      return 1;
    }

    if (run && !compileOnly) {
      std::string runCmd = shellQuote(outFile);
      if (outFile.find('/') == std::string::npos && outFile.find('\\') == std::string::npos)
        runCmd = "./" + runCmd;
      int rrc = std::system(runCmd.c_str());
      std::error_code ec;
      if (!keepC) fs::remove_all(tmpDir, ec);
      return rrc == 0 ? 0 : 1;
    }

    if (!keepC) {
      std::error_code ec;
      fs::remove_all(tmpDir, ec);
    } else if (verbose) {
      std::cerr << "tcpp: generated C kept in " << tmpDir.string() << "\n";
    }
  } catch (const tc::CompileError&) {
    return 1;
  }

  return 0;
}
