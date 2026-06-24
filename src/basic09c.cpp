//===- basic09c.cpp - BASIC09 frontend prototype -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is an intentionally small BASIC09 frontend scaffold.  The first
// supported operation is a token dump that makes the source normalization
// boundary explicit before parser, semantic, and LLVM IR lowering work is
// added.
//
//===----------------------------------------------------------------------===//

#include "Basic09AST.h"
#include "Basic09IR.h"
#include "Basic09Lexer.h"
#include "Basic09Parser.h"
#include "Basic09Semantic.h"
#include "Basic09Symbols.h"
#include "Basic09Token.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace llvm;
using namespace llvm::basic09;

namespace {

static cl::OptionCategory Basic09CCategory("basic09c options");

static cl::opt<bool> DumpTokens("dump-tokens",
                                cl::desc("Dump normalized BASIC09 tokens"),
                                cl::cat(Basic09CCategory));

static cl::opt<bool> DumpAST("dump-ast",
                             cl::desc("Parse BASIC09 source and dump the AST"),
                             cl::cat(Basic09CCategory));

static cl::opt<bool> SyntaxOnly("syntax-only",
                                cl::desc("Parse BASIC09 source and exit"),
                                cl::cat(Basic09CCategory));

static cl::opt<bool>
    AnalyzeOnly("analyze-only",
                cl::desc("Parse and semantically analyze BASIC09 source"),
                cl::cat(Basic09CCategory));

static cl::opt<bool>
    DumpSymbols("dump-symbols",
                cl::desc("Parse BASIC09 source and dump symbols"),
                cl::cat(Basic09CCategory));

static cl::opt<bool> EmitLLVM("emit-llvm",
                              cl::desc("Parse BASIC09 source and emit LLVM IR"),
                              cl::cat(Basic09CCategory));

static cl::opt<bool>
    Compile("compile",
            cl::desc("Compile BASIC09 source to a native executable"),
            cl::cat(Basic09CCategory));

static cl::opt<bool>
    EnableSDL("sdl",
              cl::desc("Link the optional SDL2 graphics runtime for --compile"),
              cl::cat(Basic09CCategory));

static cl::opt<std::string>
    OutputFilename("o", cl::desc("Output filename for --compile"),
                   cl::value_desc("path"), cl::cat(Basic09CCategory));

static cl::opt<std::string>
    CCompiler("cc", cl::desc("C compiler used by --compile"),
              cl::init(std::getenv("CC") ? std::getenv("CC") : "cc"),
              cl::value_desc("path"), cl::cat(Basic09CCategory));

static cl::opt<std::string>
    TargetTriple("target-triple", cl::desc("Target triple for emitted LLVM IR"),
                 cl::init(sys::getDefaultTargetTriple()),
                 cl::cat(Basic09CCategory));

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input BASIC09 source>"),
                                          cl::Required,
                                          cl::cat(Basic09CCategory));

static bool normalizeAndLex(StringRef Source, std::vector<Token> &Tokens) {
  std::string Normalized;
  std::string Error;
  if (!normalizeSource(Source, Normalized, Error)) {
    WithColor::error(errs(), "basic09c") << Error << '\n';
    return false;
  }

  Tokens = lex(Normalized);
  return true;
}

static std::unique_ptr<ASTNode> parseSource(StringRef Source) {
  std::vector<Token> Tokens;
  if (!normalizeAndLex(Source, Tokens))
    return nullptr;

  Parser P(Tokens);
  std::unique_ptr<ASTNode> Root = P.parseProgram();
  if (!P.getError().empty()) {
    WithColor::error(errs(), "basic09c") << P.getError() << '\n';
    return nullptr;
  }
  return Root;
}

static std::string getProcedureSymbolName(const ASTNode &Procedure) {
  StringRef Text = Procedure.Text;
  if (!Text.consume_front("PROCEDURE"))
    return "";
  Text = Text.trim();
  size_t End = Text.find_first_of(" (");
  return Text.substr(0, End).lower();
}

static std::string getEntryProcedureName(const ASTNode &Root) {
  for (const std::unique_ptr<ASTNode> &Child : Root.Children)
    if (Child->Kind == "Procedure")
      return getProcedureSymbolName(*Child);
  return "";
}

static bool writeFile(StringRef Path, StringRef Contents) {
  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::OF_Text);
  if (EC) {
    WithColor::error(errs(), "basic09c")
        << "cannot write '" << Path << "': " << EC.message() << '\n';
    return false;
  }
  OS << Contents;
  return true;
}

static bool createTempFile(StringRef Prefix, StringRef Suffix,
                           SmallVectorImpl<char> &Path) {
  int FD = -1;
  std::error_code EC = sys::fs::createTemporaryFile(Prefix, Suffix, FD, Path);
  if (EC) {
    WithColor::error(errs(), "basic09c")
        << "cannot create temporary file: " << EC.message() << '\n';
    return false;
  }
  sys::Process::SafelyCloseFileDescriptor(FD);
  return true;
}

static void normalizeIRForHostCompiler(std::string &IR) {
  size_t Pos = 0;
  while ((Pos = IR.find("f0x", Pos)) != std::string::npos) {
    char Prev = Pos == 0 ? '\0' : IR[Pos - 1];
    if (!std::isalnum(static_cast<unsigned char>(Prev)) && Prev != '_' &&
        Prev != '.') {
      IR.erase(Pos, 1);
      Pos += 2;
      continue;
    }
    Pos += 3;
  }
}

static int dumpTokens(StringRef Source) {
  std::vector<Token> Tokens;
  if (!normalizeAndLex(Source, Tokens))
    return 1;

  for (const Token &Tok : Tokens) {
    outs() << Tok.Line << ':' << Tok.Column << ": "
           << getTokenKindName(Tok.Kind);
    if (Tok.Kind != TokenKind::EndOfFile)
      outs() << " \"" << Tok.Text << '"';
    outs() << '\n';
  }
  return 0;
}

static int dumpParsedAST(StringRef Source) {
  std::unique_ptr<ASTNode> Root = parseSource(Source);
  if (!Root)
    return 1;

  dumpAST(*Root, outs());
  return 0;
}

static int parseSyntaxOnly(StringRef Source) {
  return parseSource(Source) ? 0 : 1;
}

static int analyzeOnly(StringRef Source) {
  std::unique_ptr<ASTNode> Root = parseSource(Source);
  if (!Root)
    return 1;

  return analyzeSemantics(*Root, errs()) ? 0 : 1;
}

static int dumpParsedSymbols(StringRef Source) {
  std::unique_ptr<ASTNode> Root = parseSource(Source);
  if (!Root)
    return 1;

  return dumpSymbols(*Root, outs(), errs()) ? 0 : 1;
}

static int emitParsedLLVM(StringRef Source, StringRef ModuleName) {
  std::unique_ptr<ASTNode> Root = parseSource(Source);
  if (!Root)
    return 1;

  if (!analyzeSemantics(*Root, errs()))
    return 1;
  return emitLLVMIR(*Root, ModuleName, TargetTriple, outs(), errs()) ? 0 : 1;
}

static std::string sdlRuntimeSource() {
  return R"c(
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

static SDL_Window *basic09_sdl_window;
static SDL_Renderer *basic09_sdl_renderer;
static int basic09_sdl_quit_requested;
static int basic09_sdl_last_key;

void basic09_sdl_close(void);

static void basic09_sdl_color(int color) {
  static const unsigned char palette[16][3] = {
      {0, 0, 0},       {0, 0, 170},     {0, 170, 0},     {0, 170, 170},
      {170, 0, 0},     {170, 0, 170},   {170, 85, 0},    {170, 170, 170},
      {85, 85, 85},    {85, 85, 255},   {85, 255, 85},   {85, 255, 255},
      {255, 85, 85},   {255, 85, 255},  {255, 255, 85},  {255, 255, 255},
  };
  color &= 15;
  SDL_SetRenderDrawColor(basic09_sdl_renderer, palette[color][0],
                         palette[color][1], palette[color][2], 255);
}

static int basic09_sdl_ready(void) {
  if (basic09_sdl_renderer)
    return 1;
  fputs("basic09 SDL runtime is not open\n", stderr);
  return 0;
}

void basic09_sdl_poll(void) {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT) {
      basic09_sdl_quit_requested = 1;
    } else if (event.type == SDL_KEYDOWN) {
      basic09_sdl_last_key = (int)event.key.keysym.sym;
      if (event.key.keysym.sym == SDLK_ESCAPE)
        basic09_sdl_quit_requested = 1;
    }
  }
}

int basic09_sdl_quit(void) {
  return basic09_sdl_quit_requested;
}

int basic09_sdl_key(void) {
  int key = basic09_sdl_last_key;
  basic09_sdl_last_key = 0;
  return key;
}

void basic09_sdl_close(void) {
  if (basic09_sdl_renderer) {
    SDL_DestroyRenderer(basic09_sdl_renderer);
    basic09_sdl_renderer = NULL;
  }
  if (basic09_sdl_window) {
    SDL_DestroyWindow(basic09_sdl_window);
    basic09_sdl_window = NULL;
  }
  SDL_Quit();
}

void basic09_sdl_open(int width, int height) {
  if (basic09_sdl_renderer)
    return;
  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    exit(1);
  }
  basic09_sdl_window =
      SDL_CreateWindow("basic09c SDL", SDL_WINDOWPOS_CENTERED,
                       SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_SHOWN);
  if (!basic09_sdl_window) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    exit(1);
  }
  basic09_sdl_renderer =
      SDL_CreateRenderer(basic09_sdl_window, -1, SDL_RENDERER_ACCELERATED);
  if (!basic09_sdl_renderer) {
    basic09_sdl_renderer =
        SDL_CreateRenderer(basic09_sdl_window, -1, SDL_RENDERER_SOFTWARE);
  }
  if (!basic09_sdl_renderer) {
    fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    exit(1);
  }
  basic09_sdl_quit_requested = 0;
  basic09_sdl_last_key = 0;
  atexit(basic09_sdl_close);
  SDL_ShowWindow(basic09_sdl_window);
  basic09_sdl_poll();
}

void basic09_sdl_clear(int color) {
  if (!basic09_sdl_ready())
    return;
  basic09_sdl_color(color);
  SDL_RenderClear(basic09_sdl_renderer);
}

void basic09_sdl_present(void) {
  if (!basic09_sdl_ready())
    return;
  SDL_RenderPresent(basic09_sdl_renderer);
  basic09_sdl_poll();
}

void basic09_sdl_delay(int milliseconds) {
  int remaining = milliseconds < 0 ? 0 : milliseconds;
  while (remaining > 0) {
    int step = remaining < 16 ? remaining : 16;
    SDL_Delay((unsigned)step);
    basic09_sdl_poll();
    remaining -= step;
  }
}

void basic09_sdl_pset(int x, int y, int color) {
  if (!basic09_sdl_ready())
    return;
  basic09_sdl_color(color);
  SDL_RenderDrawPoint(basic09_sdl_renderer, x, y);
}

void basic09_sdl_line(int x1, int y1, int x2, int y2, int color) {
  if (!basic09_sdl_ready())
    return;
  basic09_sdl_color(color);
  SDL_RenderDrawLine(basic09_sdl_renderer, x1, y1, x2, y2);
}
)c";
}

static std::vector<std::string> splitCommandLineWords(StringRef Text) {
  std::vector<std::string> Words;
  std::istringstream IS(Text.str());
  std::string Word;
  while (IS >> Word)
    Words.push_back(Word);
  return Words;
}

static std::string runCommandCapture(StringRef Command) {
  std::string Output;
  FILE *Pipe = popen(Command.str().c_str(), "r");
  if (!Pipe)
    return Output;
  char Buffer[256];
  while (fgets(Buffer, sizeof(Buffer), Pipe))
    Output += Buffer;
  pclose(Pipe);
  return Output;
}

static std::vector<std::string> getSDLCompilerFlags() {
  std::string Flags = runCommandCapture("sdl2-config --cflags --libs");
  if (Flags.empty())
    Flags = runCommandCapture("pkg-config --cflags --libs sdl2");
  return splitCommandLineWords(Flags);
}

static int compileToExecutable(StringRef Source, StringRef ModuleName) {
  if (OutputFilename.empty()) {
    WithColor::error(errs(), "basic09c")
        << "--compile requires -o <output>\n";
    return 1;
  }

  std::unique_ptr<ASTNode> Root = parseSource(Source);
  if (!Root)
    return 1;
  if (!analyzeSemantics(*Root, errs()))
    return 1;

  std::string EntryName = getEntryProcedureName(*Root);
  if (EntryName.empty()) {
    WithColor::error(errs(), "basic09c")
        << "no PROCEDURE found for executable entry point\n";
    return 1;
  }

  std::string IR;
  raw_string_ostream IROS(IR);
  if (!emitLLVMIR(*Root, ModuleName, TargetTriple, IROS, errs()))
    return 1;
  IROS.flush();
  normalizeIRForHostCompiler(IR);
  if (!EnableSDL && StringRef(IR).contains("@basic09_sdl_")) {
    WithColor::error(errs(), "basic09c")
        << "source uses SDL runtime calls; pass --sdl with --compile\n";
    return 1;
  }

  SmallString<128> IRPath;
  SmallString<128> MainPath;
  SmallString<128> SDLRuntimePath;
  if (!createTempFile("basic09c", "ll", IRPath))
    return 1;
  if (!createTempFile("basic09c-main", "c", MainPath)) {
    sys::fs::remove(IRPath);
    return 1;
  }
  if (EnableSDL && !createTempFile("basic09c-sdl", "c", SDLRuntimePath)) {
    sys::fs::remove(IRPath);
    sys::fs::remove(MainPath);
    return 1;
  }

  auto Cleanup = [&]() {
    sys::fs::remove(IRPath);
    sys::fs::remove(MainPath);
    if (!SDLRuntimePath.empty())
      sys::fs::remove(SDLRuntimePath);
  };

  if (!writeFile(IRPath, IR)) {
    Cleanup();
    return 1;
  }

  std::string Wrapper;
  raw_string_ostream WrapperOS(Wrapper);
  WrapperOS << "int " << EntryName << "(void);\n"
            << "int main(void) {\n"
            << "  return " << EntryName << "();\n"
            << "}\n";
  WrapperOS.flush();

  if (!writeFile(MainPath, Wrapper)) {
    Cleanup();
    return 1;
  }
  if (EnableSDL && !writeFile(SDLRuntimePath, sdlRuntimeSource())) {
    Cleanup();
    return 1;
  }

  ErrorOr<std::string> Compiler = sys::findProgramByName(CCompiler);
  if (!Compiler) {
    WithColor::error(errs(), "basic09c")
        << "cannot find compiler '" << CCompiler
        << "': " << Compiler.getError().message() << '\n';
    Cleanup();
    return 1;
  }

  std::vector<std::string> ArgStorage;
  std::vector<StringRef> Args = {*Compiler, IRPath, MainPath};
  if (EnableSDL) {
    Args.push_back(SDLRuntimePath);
    ArgStorage = getSDLCompilerFlags();
    if (ArgStorage.empty()) {
      WithColor::error(errs(), "basic09c")
          << "--sdl requires SDL2 development flags from sdl2-config or "
             "pkg-config\n";
      Cleanup();
      return 1;
    }
  }
  Args.push_back("-o");
  Args.push_back(OutputFilename);
  for (const std::string &Arg : ArgStorage)
    Args.push_back(Arg);
  int Result = sys::ExecuteAndWait(*Compiler, Args);
  Cleanup();

  if (Result != 0) {
    WithColor::error(errs(), "basic09c")
        << "compiler command failed with exit code " << Result << '\n';
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  cl::HideUnrelatedOptions(Basic09CCategory);
  cl::ParseCommandLineOptions(argc, argv, "BASIC09 frontend prototype\n");

  ErrorOr<std::unique_ptr<MemoryBuffer>> InputOrErr =
      MemoryBuffer::getFileOrSTDIN(InputFilename);
  if (!InputOrErr) {
    WithColor::error(errs(), "basic09c")
        << "cannot read '" << InputFilename
        << "': " << InputOrErr.getError().message() << '\n';
    return 1;
  }

  if (DumpTokens)
    return dumpTokens((*InputOrErr)->getBuffer());
  if (DumpAST)
    return dumpParsedAST((*InputOrErr)->getBuffer());
  if (SyntaxOnly)
    return parseSyntaxOnly((*InputOrErr)->getBuffer());
  if (AnalyzeOnly)
    return analyzeOnly((*InputOrErr)->getBuffer());
  if (DumpSymbols)
    return dumpParsedSymbols((*InputOrErr)->getBuffer());
  if (EmitLLVM)
    return emitParsedLLVM((*InputOrErr)->getBuffer(), InputFilename);
  if (Compile)
    return compileToExecutable((*InputOrErr)->getBuffer(), InputFilename);

  WithColor::error(errs(), "basic09c")
      << "no action specified; use --dump-tokens, --dump-ast, "
         "--dump-symbols, --analyze-only, --syntax-only, --emit-llvm, "
         "or --compile\n";
  return 1;
}
