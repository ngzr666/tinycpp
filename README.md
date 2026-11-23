# tcpp

A small, hand-built C++ compiler written in C++17.  
It aims for a **tcc-like** workflow: fast, simple, portable, and with a **strong preprocessor**.

## What it is

`tcpp` compiles a C++ subset to portable C, then invokes the host C compiler (`$CC` or `cc`) to produce an executable/object. This gives broad OS/architecture portability anywhere a C compiler exists.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Binary: `./build/tcpp`

## Usage

```sh
./build/tcpp tests/hello.cpp -o hello
./build/tcpp --run tests/hello.cpp
./build/tcpp -E file.cpp              # preprocess only
./build/tcpp --lex file.cpp            # dump tokens
./build/tcpp --emit-c file.cpp         # emit generated C
./build/tcpp -c file.cpp -o file.o     # compile only
./build/tcpp -Iinclude -DFOO=1 -Ubar file.cpp
```

Options:

- `-E` preprocess only
- `--lex` dump preprocessed tokens
- `--emit-c` emit generated C
- `-c` compile to object
- `--run` compile and run
- `--keep-c` keep generated C temp dir
- `-o FILE` output file
- `-I DIR`, `-DNAME[=VAL]`, `-U NAME`
- `-v` verbose

## Preprocessor

The preprocessor is token-based and supports:

- `#include`, `#include_next`, include search paths, include cycle/depth guards
- `#define` object/function/variadic macros, `#undef`
- macro expansion with hide sets (no recursive re-expansion)
- `#` stringize, `##` paste, `__VA_ARGS__`, `__VA_OPT__`
- `#if/#ifdef/#ifndef/#elif/#else/#endif`, `defined`, integer constant expressions
- `#pragma once`, `#error`, `#warning`, `#line` (accepted/handled where applicable)
- builtins: `__FILE__`, `__LINE__`, `__COUNTER__`, `__DATE__`, `__TIME__`
- `__has_include`, `__has_include_next`
- line splicing, comments, string/char/raw string literals in lexer
- multi-line macro invocations

It can preprocess real libc++ headers in this environment (tested with `<cstddef>` and `<cstdio>` using Termux include paths and `_LIBCPP_HARDENING_MODE_DEFAULT`).

## Supported C++ subset

- functions, globals, locals, parameters, default args
- `if/while/do/for/switch/case/default/break/continue/return`
- arithmetic, logical, bitwise, assignment, ternary, comma, casts
- pointers, arrays, references (including reference params/locals)
- `struct`/`class`, fields, methods, constructors/destructors
- constructor initializer lists, default field initializers
- `new`/`delete` (object form), simple `new[]`/`delete[]` allocation
- namespaces (basic), `using namespace`, `typedef`, `using` alias, enums
- `extern "C"`
- function calls including variadic C calls

## Not yet supported

- templates, STL containers, exceptions, RTTI/virtual dispatch
- full overload resolution, operator overloading
- inheritance layout/virtual bases
- non-trivial copy/move semantics generation
- function pointers in full generality
- separate compilation/link model beyond host C compiler invocation
- full C++ standard library compatibility

## Design notes

- Frontend: lexer -> preprocessor -> parser -> typed AST.
- Backend: emits readable C99-ish code with tcpp runtime helpers (`tc_alloc`, `tc_free`).
- Name mangling keeps C++ symbols separate; `main` and `extern "C"` symbols keep C names.
- Class locals with destructors get scope-exit cleanup, including normal `return`/`break`/`continue` paths.

## Roadmap

- Better diagnostics and source locations through all passes
- More expression/type coverage and overload resolution
- Copy/move constructors and assignment
- Separate `-c`/link pipeline and multiple translation units
- Direct native backends (optional), keeping C backend as portable fallback
