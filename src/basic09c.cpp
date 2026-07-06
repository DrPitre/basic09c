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

static cl::opt<bool>
    FormatSource("format",
                 cl::desc("Pretty-print normalized BASIC09 source"),
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
    OptLevel("O", cl::desc("Optimization level for --compile (0, 1, 2, 3, s, z)"),
             cl::init(""), cl::value_desc("level"), cl::Prefix,
             cl::cat(Basic09CCategory));

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

// Traditional BASIC09 keeps whichever procedure was entered/loaded last as
// the "current" one, so a module file containing multiple PROCEDUREs runs
// the last one, not the first.
static const ASTNode *getEntryProcedure(const ASTNode &Root) {
  const ASTNode *Entry = nullptr;
  for (const std::unique_ptr<ASTNode> &Child : Root.Children)
    if (Child->Kind == "Procedure")
      Entry = Child.get();
  return Entry;
}

// The entry procedure's PARAM list becomes the executable's command-line
// arguments. Only scalar types have an obvious textual representation on
// the command line, so arrays and RECORD-typed params are rejected here.
struct EntryParamInfo {
  std::string Name;
  std::string TypeName;
  uint64_t StringLength = 255;
};

static uint64_t constantBoundExtent(const ASTNode &BoundNode) {
  if (BoundNode.Children.empty())
    return 0;
  const ASTNode &Expr = *BoundNode.Children.front();
  uint64_t Value = 0;
  if (Expr.Kind == "Integer")
    StringRef(Expr.Text).getAsInteger(10, Value);
  else if (Expr.Kind == "HexInteger")
    StringRef(Expr.Text).drop_front().getAsInteger(16, Value);
  return Value;
}

static bool isScalarEntryParamType(StringRef TypeName) {
  return TypeName == "BYTE" || TypeName == "INTEGER" ||
         TypeName == "BOOLEAN" || TypeName == "REAL" || TypeName == "STRING";
}

// C type matching the LLVM IR representation of each BASIC09 scalar type
// (BYTE = i8, INTEGER/BOOLEAN = i16, REAL = double), so the argv-parsing
// wrapper's pointers line up byte-for-byte with what the compiled
// procedure expects.
static const char *entryParamCType(const EntryParamInfo &Param) {
  if (Param.TypeName == "BYTE")
    return "unsigned char";
  if (Param.TypeName == "REAL")
    return "double";
  if (Param.TypeName == "STRING")
    return "char";
  return "short";
}

static bool collectEntryParams(const ASTNode &Procedure,
                               std::vector<EntryParamInfo> &Params,
                               std::string &BadParamName) {
  const ASTNode *Body = nullptr;
  for (const std::unique_ptr<ASTNode> &Child : Procedure.Children)
    if (Child->Kind == "Block")
      Body = Child.get();
  if (!Body)
    return true;

  for (const std::unique_ptr<ASTNode> &Stmt : Body->Children) {
    if (Stmt->Kind != "Param")
      continue;
    for (const std::unique_ptr<ASTNode> &Decl : Stmt->Children) {
      if (Decl->Kind != "Decl")
        continue;
      bool HasBound = false;
      const ASTNode *TypeName = nullptr;
      for (const std::unique_ptr<ASTNode> &DeclChild : Decl->Children) {
        if (DeclChild->Kind == "Bound")
          HasBound = true;
        else if (DeclChild->Kind == "TypeName")
          TypeName = DeclChild.get();
      }
      std::string Type = TypeName ? TypeName->Text : "INTEGER";
      if (HasBound || !isScalarEntryParamType(Type)) {
        BadParamName = Decl->Text;
        return false;
      }
      EntryParamInfo Info;
      Info.Name = Decl->Text;
      Info.TypeName = Type;
      if (Type == "STRING" && TypeName)
        for (const std::unique_ptr<ASTNode> &TypeChild : TypeName->Children)
          if (TypeChild->Kind == "StringLength")
            Info.StringLength = constantBoundExtent(*TypeChild);
      Params.push_back(std::move(Info));
    }
  }
  return true;
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

static bool isFormatOperator(StringRef Text) {
  return Text == ":=" || Text == "=" || Text == "<>" || Text == "<" ||
         Text == "<=" || Text == ">" || Text == ">=" || Text == "+" ||
         Text == "-" || Text == "*" || Text == "/" || Text == "^";
}

static bool isWordToken(const Token &Tok) {
  return Tok.Kind == TokenKind::Identifier || Tok.Kind == TokenKind::Integer ||
         Tok.Kind == TokenKind::HexInteger || Tok.Kind == TokenKind::Real ||
         Tok.Kind == TokenKind::String;
}

static bool formatNeedsSpaceBefore(const Token &Tok, const Token *Prev,
                                   const Token *Next) {
  if (!Prev)
    return false;
  StringRef Text = Tok.Text;
  StringRef PrevText = Prev->Text;

  if (Text == "," || Text == ";" || Text == ")" || Text == "]" ||
      Text == ".")
    return false;
  if (Text == ":")
    return false;
  if (Text == "(" || Text == "[")
    return false;
  if (PrevText == "(" || PrevText == "[" || PrevText == "." ||
      PrevText == "#")
    return false;
  if (PrevText == ",")
    return true;
  if (PrevText == ":")
    return true;
  if (isFormatOperator(Text) || isFormatOperator(PrevText))
    return true;
  return isWordToken(*Prev) && isWordToken(Tok);
}

static std::string formatTokenLine(ArrayRef<Token> Line) {
  std::string Out;
  for (size_t I = 0; I < Line.size(); ++I) {
    const Token *Prev = I == 0 ? nullptr : &Line[I - 1];
    const Token *Next = I + 1 < Line.size() ? &Line[I + 1] : nullptr;
    if (formatNeedsSpaceBefore(Line[I], Prev, Next))
      Out.push_back(' ');
    Out += Line[I].Text;
  }
  return Out;
}

static bool isLabelOnlyLine(ArrayRef<Token> Line) {
  return Line.size() == 2 && Line[0].Kind == TokenKind::Identifier &&
         Line[1].Text == ":";
}

static bool isOutdentKeyword(StringRef Keyword) {
  return Keyword == "ELSE" || Keyword == "ENDIF" || Keyword == "ENDWHILE" ||
         Keyword == "ENDLOOP" || Keyword == "NEXT" || Keyword == "UNTIL" ||
         Keyword == "END";
}

static bool isIndentKeyword(StringRef Keyword) {
  return Keyword == "PROCEDURE" || Keyword == "ELSE" || Keyword == "FOR" ||
         Keyword == "WHILE" || Keyword == "REPEAT" || Keyword == "LOOP";
}

static bool lineEndsWithKeyword(ArrayRef<Token> Line, StringRef Keyword) {
  return !Line.empty() && Line.back().Kind == TokenKind::Identifier &&
         Line.back().Text == Keyword;
}

static int formatSource(StringRef Source) {
  std::unique_ptr<ASTNode> Root = parseSource(Source);
  if (!Root)
    return 1;

  std::vector<Token> Tokens;
  if (!normalizeAndLex(Source, Tokens))
    return 1;

  int Indent = 0;
  std::vector<Token> Line;
  auto FlushLine = [&]() {
    if (Line.empty())
      return;

    StringRef First = Line.front().Text;
    bool LabelOnly = isLabelOnlyLine(Line);
    if (!LabelOnly && isOutdentKeyword(First) && Indent > 0)
      --Indent;

    if (LabelOnly) {
      outs() << formatTokenLine(Line) << '\n';
    } else {
      for (int I = 0; I < Indent; ++I)
        outs() << "  ";
      outs() << formatTokenLine(Line) << '\n';
    }

    if (!LabelOnly &&
        (isIndentKeyword(First) || lineEndsWithKeyword(Line, "THEN")))
      ++Indent;
    Line.clear();
  };

  for (const Token &Tok : Tokens) {
    if (Tok.Kind == TokenKind::EndOfFile)
      break;
    if (Tok.Kind == TokenKind::Newline) {
      FlushLine();
      continue;
    }
    Line.push_back(Tok);
  }
  FlushLine();
  return 0;
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
}

void basic09_sdl_delay(int milliseconds) {
  SDL_Delay(milliseconds < 0 ? 0 : (unsigned)milliseconds);
}

void basic09_sdl_pset(int x, int y, int color) {
  if (!basic09_sdl_ready())
    return;
  basic09_sdl_color(color);
  SDL_RenderDrawPoint(basic09_sdl_renderer, x, y);
}

void basic09_sdl_fillbox(int x, int y, int width, int height, int color) {
  if (!basic09_sdl_ready())
    return;
  basic09_sdl_color(color);
  SDL_Rect rect = {x, y, width, height};
  SDL_RenderFillRect(basic09_sdl_renderer, &rect);
}

void basic09_sdl_line(int x1, int y1, int x2, int y2, int color) {
  if (!basic09_sdl_ready())
    return;
  basic09_sdl_color(color);
  SDL_RenderDrawLine(basic09_sdl_renderer, x1, y1, x2, y2);
}
)c";
}

static std::string fileRuntimeSource() {
  return R"c(
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASIC09_MAX_FILES 32

static FILE *basic09_file_table[BASIC09_MAX_FILES];

static int basic09_file_alloc_slot(void) {
  int i;
  for (i = 1; i < BASIC09_MAX_FILES; ++i) {
    if (!basic09_file_table[i])
      return i;
  }
  fputs("basic09 file runtime: too many open files\n", stderr);
  return 0;
}

static FILE *basic09_file_get_handle(int handle) {
  if (handle <= 0 || handle >= BASIC09_MAX_FILES || !basic09_file_table[handle]) {
    fputs("basic09 file runtime: invalid file handle\n", stderr);
    return NULL;
  }
  return basic09_file_table[handle];
}

int basic09_file_open(const char *name, const char *mode) {
  int slot = basic09_file_alloc_slot();
  FILE *fp;
  if (!slot)
    return 0;
  fp = fopen(name, mode);
  if (!fp) {
    fprintf(stderr, "basic09 file runtime: cannot open '%s'\n", name);
    return 0;
  }
  basic09_file_table[slot] = fp;
  return slot;
}

void basic09_file_close(int handle) {
  FILE *fp = basic09_file_get_handle(handle);
  if (!fp)
    return;
  fclose(fp);
  basic09_file_table[handle] = NULL;
}

void basic09_file_delete(const char *name) {
  remove(name);
}

void basic09_file_seek(int handle, long long position) {
  FILE *fp = basic09_file_get_handle(handle);
  if (!fp)
    return;
  fseek(fp, (long)position, SEEK_SET);
}

void basic09_file_get(int handle, void *buffer, long long size) {
  FILE *fp = basic09_file_get_handle(handle);
  if (!fp)
    return;
  fread(buffer, 1, (size_t)size, fp);
}

void basic09_file_put(int handle, const void *buffer, long long size) {
  FILE *fp = basic09_file_get_handle(handle);
  if (!fp)
    return;
  fwrite(buffer, 1, (size_t)size, fp);
}

void basic09_file_write_str(int handle, const char *text) {
  FILE *fp = basic09_file_get_handle(handle);
  if (!fp)
    return;
  fputs(text, fp);
}

void basic09_file_write_real(int handle, double value) {
  FILE *fp = basic09_file_get_handle(handle);
  if (!fp)
    return;
  fprintf(fp, "%g", value);
}

void basic09_file_write_int(int handle, long long value) {
  FILE *fp = basic09_file_get_handle(handle);
  if (!fp)
    return;
  fprintf(fp, "%lld", value);
}

void basic09_file_newline(int handle) {
  FILE *fp = basic09_file_get_handle(handle);
  if (!fp)
    return;
  fputc('\n', fp);
}

void basic09_file_readline(int handle, char *buffer, int capacity) {
  FILE *fp = basic09_file_get_handle(handle);
  size_t len;
  if (!fp) {
    buffer[0] = '\0';
    return;
  }
  if (!fgets(buffer, capacity, fp)) {
    buffer[0] = '\0';
    return;
  }
  len = strlen(buffer);
  while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
    buffer[--len] = '\0';
  }
}

static void basic09_file_skip_eol(FILE *fp) {
  int ch;
  while ((ch = fgetc(fp)) != EOF && ch != '\n') {
  }
}

double basic09_file_read_real(int handle) {
  FILE *fp = basic09_file_get_handle(handle);
  double value = 0.0;
  if (!fp)
    return 0.0;
  if (fscanf(fp, "%lf", &value) != 1) {
    basic09_file_skip_eol(fp);
    return 0.0;
  }
  basic09_file_skip_eol(fp);
  return value;
}

long long basic09_file_read_int(int handle) {
  FILE *fp = basic09_file_get_handle(handle);
  long long value = 0;
  if (!fp)
    return 0;
  if (fscanf(fp, "%lld", &value) != 1) {
    basic09_file_skip_eol(fp);
    return 0;
  }
  basic09_file_skip_eol(fp);
  return value;
}

int basic09_file_eof(int handle) {
  FILE *fp = basic09_file_get_handle(handle);
  int ch;
  if (!fp)
    return 1;
  ch = fgetc(fp);
  if (ch == EOF)
    return 1;
  ungetc(ch, fp);
  return 0;
}

static void basic09_fmt_clamp_width(int *width) {
  if (*width < 1)
    *width = 1;
  if (*width > 255)
    *width = 255;
}

static void basic09_fmt_pad(char *out, int width, int justify,
                            const char *content) {
  int len = (int)strlen(content);
  if (len > width) {
    memcpy(out, content, (size_t)width);
    out[width] = '\0';
    return;
  }
  int total = width - len;
  if (justify == '>') {
    memset(out, ' ', (size_t)total);
    memcpy(out + total, content, (size_t)len);
  } else if (justify == '^') {
    int left = total / 2;
    int right = total - left;
    memset(out, ' ', (size_t)left);
    memcpy(out + left, content, (size_t)len);
    memset(out + left + len, ' ', (size_t)right);
  } else {
    memcpy(out, content, (size_t)len);
    memset(out + len, ' ', (size_t)total);
  }
  out[width] = '\0';
}

void basic09_fmt_str(char *out, int width, int justify, const char *value) {
  basic09_fmt_clamp_width(&width);
  char truncated[256];
  size_t len = strlen(value);
  if (len > (size_t)width)
    len = (size_t)width;
  memcpy(truncated, value, len);
  truncated[len] = '\0';
  basic09_fmt_pad(out, width, justify, truncated);
}

void basic09_fmt_bool(char *out, int width, int justify, long long value) {
  basic09_fmt_str(out, width, justify, value ? "TRUE" : "FALSE");
}

void basic09_fmt_int(char *out, int width, int justify, long long value) {
  basic09_fmt_clamp_width(&width);
  int digits_width = width - 1;
  if (digits_width < 0)
    digits_width = 0;
  char sign = value < 0 ? '-' : ' ';
  unsigned long long mag =
      value < 0 ? (unsigned long long)(-(value)) : (unsigned long long)value;
  char digits[32];
  snprintf(digits, sizeof(digits), "%llu", mag);
  int dlen = (int)strlen(digits);
  if (dlen > digits_width) {
    memset(out, '*', (size_t)width);
    out[width] = '\0';
    return;
  }
  int pad = digits_width - dlen;
  char buf[256];
  int pos = 0;
  if (justify == '<') {
    buf[pos++] = sign;
    memcpy(buf + pos, digits, (size_t)dlen);
    pos += dlen;
    memset(buf + pos, ' ', (size_t)pad);
    pos += pad;
  } else if (justify == '^') {
    buf[pos++] = sign;
    memset(buf + pos, '0', (size_t)pad);
    pos += pad;
    memcpy(buf + pos, digits, (size_t)dlen);
    pos += dlen;
  } else {
    memset(buf + pos, ' ', (size_t)pad);
    pos += pad;
    buf[pos++] = sign;
    memcpy(buf + pos, digits, (size_t)dlen);
    pos += dlen;
  }
  buf[pos] = '\0';
  memcpy(out, buf, (size_t)(pos + 1));
}

void basic09_fmt_real(char *out, int width, int precision, int justify,
                      double value) {
  basic09_fmt_clamp_width(&width);
  if (precision < 0) {
    char digits[320];
    snprintf(digits, sizeof(digits), "%g", value);
    basic09_fmt_pad(out, width, justify, digits);
    return;
  }
  char digits[320];
  snprintf(digits, sizeof(digits), "%.*f", precision, fabs(value));
  int dlen = (int)strlen(digits);
  int digits_width = width - 1;
  if (dlen > digits_width) {
    memset(out, '*', (size_t)width);
    out[width] = '\0';
    return;
  }
  char sign = value < 0 ? '-' : ' ';
  int pad = digits_width - dlen;
  char buf[320];
  int pos = 0;
  if (justify == '>') {
    memset(buf + pos, ' ', (size_t)pad);
    pos += pad;
    buf[pos++] = sign;
    memcpy(buf + pos, digits, (size_t)dlen);
    pos += dlen;
  } else if (justify == '^') {
    memset(buf + pos, ' ', (size_t)pad);
    pos += pad;
    memcpy(buf + pos, digits, (size_t)dlen);
    pos += dlen;
    buf[pos++] = value < 0 ? '-' : ' ';
  } else {
    buf[pos++] = sign;
    memcpy(buf + pos, digits, (size_t)dlen);
    pos += dlen;
    memset(buf + pos, ' ', (size_t)pad);
    pos += pad;
  }
  buf[pos] = '\0';
  memcpy(out, buf, (size_t)(pos + 1));
}

void basic09_fmt_exp(char *out, int width, int precision, int justify,
                     double value) {
  basic09_fmt_clamp_width(&width);
  if (precision < 0)
    precision = 3;
  char digits[320];
  snprintf(digits, sizeof(digits), "%.*E", precision, fabs(value));
  char *epos = strchr(digits, 'E');
  if (epos) {
    int exp = atoi(epos + 1);
    char expbuf[16];
    snprintf(expbuf, sizeof(expbuf), "E%+03d", exp);
    *epos = '\0';
    strncat(digits, expbuf, sizeof(digits) - strlen(digits) - 1);
  }
  int dlen = (int)strlen(digits);
  int digits_width = width - 1;
  if (dlen > digits_width) {
    memset(out, '*', (size_t)width);
    out[width] = '\0';
    return;
  }
  char sign = value < 0 ? '-' : ' ';
  int pad = digits_width - dlen;
  char buf[320];
  int pos = 0;
  if (justify == '>' || justify == '^') {
    memset(buf + pos, ' ', (size_t)pad);
    pos += pad;
    buf[pos++] = sign;
    memcpy(buf + pos, digits, (size_t)dlen);
    pos += dlen;
  } else {
    buf[pos++] = sign;
    memcpy(buf + pos, digits, (size_t)dlen);
    pos += dlen;
    memset(buf + pos, ' ', (size_t)pad);
    pos += pad;
  }
  buf[pos] = '\0';
  memcpy(out, buf, (size_t)(pos + 1));
}

void basic09_fmt_hex_num(char *out, int width, int justify,
                         unsigned long long bits) {
  basic09_fmt_clamp_width(&width);
  if (width < 16)
    bits &= (1ULL << (4 * width)) - 1ULL;
  char digits[24];
  snprintf(digits, sizeof(digits), "%0*llX", width, bits);
  if ((int)strlen(digits) > width)
    memmove(digits, digits + (strlen(digits) - width), (size_t)width + 1);
  basic09_fmt_pad(out, width, justify, digits);
}

void basic09_fmt_hex_str(char *out, int width, int justify,
                         const char *value) {
  basic09_fmt_clamp_width(&width);
  int maxBytes = width / 2;
  char digits[256];
  int pos = 0;
  for (int i = 0; value[i] != '\0' && i < maxBytes; ++i) {
    pos += snprintf(digits + pos, sizeof(digits) - (size_t)pos, "%02X",
                    (unsigned char)value[i]);
  }
  digits[pos] = '\0';
  basic09_fmt_pad(out, width, justify, digits);
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

  const ASTNode *EntryProcedure = getEntryProcedure(*Root);
  if (!EntryProcedure) {
    WithColor::error(errs(), "basic09c")
        << "no PROCEDURE found for executable entry point\n";
    return 1;
  }
  std::string EntryName = getProcedureSymbolName(*EntryProcedure);
  std::vector<EntryParamInfo> EntryParams;
  std::string BadParamName;
  if (!collectEntryParams(*EntryProcedure, EntryParams, BadParamName)) {
    WithColor::error(errs(), "basic09c")
        << "entry procedure '" << EntryName << "' PARAM '" << BadParamName
        << "' is an array or record and cannot be supplied from the "
           "command line; only scalar BYTE/INTEGER/BOOLEAN/REAL/STRING "
           "params are allowed on the entry procedure\n";
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
  SmallString<128> FileRuntimePath;
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
  if (!createTempFile("basic09c-file", "c", FileRuntimePath)) {
    sys::fs::remove(IRPath);
    sys::fs::remove(MainPath);
    if (!SDLRuntimePath.empty())
      sys::fs::remove(SDLRuntimePath);
    return 1;
  }

  auto Cleanup = [&]() {
    sys::fs::remove(IRPath);
    sys::fs::remove(MainPath);
    if (!SDLRuntimePath.empty())
      sys::fs::remove(SDLRuntimePath);
    sys::fs::remove(FileRuntimePath);
  };

  if (!writeFile(IRPath, IR)) {
    Cleanup();
    return 1;
  }

  std::string Wrapper;
  raw_string_ostream WrapperOS(Wrapper);
  if (EntryParams.empty()) {
    WrapperOS << "int " << EntryName << "(void);\n"
              << "int main(void) {\n"
              << "  return " << EntryName << "();\n"
              << "}\n";
  } else {
    WrapperOS << "#include <stdio.h>\n"
              << "#include <stdlib.h>\n"
              << "#include <string.h>\n\n";
    WrapperOS << "int " << EntryName << "(";
    for (size_t I = 0; I < EntryParams.size(); ++I) {
      if (I)
        WrapperOS << ", ";
      WrapperOS << entryParamCType(EntryParams[I]) << " *";
    }
    WrapperOS << ");\n\n";

    WrapperOS << "int main(int argc, char **argv) {\n";
    for (size_t I = 0; I < EntryParams.size(); ++I) {
      const EntryParamInfo &Param = EntryParams[I];
      if (Param.TypeName == "STRING")
        WrapperOS << "  char p" << I << "[" << (Param.StringLength + 1)
                  << "];\n";
      else
        WrapperOS << "  " << entryParamCType(Param) << " p" << I << " = 0;\n";
    }
    WrapperOS << "  if (argc - 1 < " << EntryParams.size() << ") {\n"
              << "    fprintf(stderr, \"usage: %s";
    for (const EntryParamInfo &Param : EntryParams)
      WrapperOS << " <" << Param.Name << ">";
    WrapperOS << "\\n\", argv[0]);\n"
              << "    return 1;\n"
              << "  }\n";
    for (size_t I = 0; I < EntryParams.size(); ++I) {
      const EntryParamInfo &Param = EntryParams[I];
      if (Param.TypeName == "STRING") {
        WrapperOS << "  strncpy(p" << I << ", argv[" << (I + 1)
                  << "], sizeof(p" << I << ") - 1);\n"
                  << "  p" << I << "[sizeof(p" << I << ") - 1] = 0;\n";
      } else if (Param.TypeName == "REAL") {
        WrapperOS << "  p" << I << " = strtod(argv[" << (I + 1)
                  << "], NULL);\n";
      } else {
        WrapperOS << "  p" << I << " = (" << entryParamCType(Param)
                  << ")strtol(argv[" << (I + 1) << "], NULL, 10);\n";
      }
    }
    WrapperOS << "  return " << EntryName << "(";
    for (size_t I = 0; I < EntryParams.size(); ++I) {
      if (I)
        WrapperOS << ", ";
      WrapperOS << (EntryParams[I].TypeName == "STRING" ? "" : "&") << "p"
                << I;
    }
    WrapperOS << ");\n}\n";
  }
  WrapperOS.flush();

  if (!writeFile(MainPath, Wrapper)) {
    Cleanup();
    return 1;
  }
  if (EnableSDL && !writeFile(SDLRuntimePath, sdlRuntimeSource())) {
    Cleanup();
    return 1;
  }
  if (!writeFile(FileRuntimePath, fileRuntimeSource())) {
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

  std::string OptLevelArg;
  std::vector<std::string> ArgStorage;
  std::vector<StringRef> Args = {*Compiler, IRPath, MainPath, FileRuntimePath};
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
  if (!OptLevel.empty()) {
    OptLevelArg = "-O" + std::string(OptLevel);
    Args.push_back(OptLevelArg);
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
  if (FormatSource)
    return formatSource((*InputOrErr)->getBuffer());
  if (EmitLLVM)
    return emitParsedLLVM((*InputOrErr)->getBuffer(), InputFilename);
  if (Compile)
    return compileToExecutable((*InputOrErr)->getBuffer(), InputFilename);

  WithColor::error(errs(), "basic09c")
      << "no action specified; use --dump-tokens, --dump-ast, "
         "--dump-symbols, --analyze-only, --syntax-only, --format, "
         "--emit-llvm, or --compile\n";
  return 1;
}
