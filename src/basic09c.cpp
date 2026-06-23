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
#include <cstdlib>
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

  SmallString<128> IRPath;
  SmallString<128> MainPath;
  if (!createTempFile("basic09c", "ll", IRPath))
    return 1;
  if (!createTempFile("basic09c-main", "c", MainPath)) {
    sys::fs::remove(IRPath);
    return 1;
  }

  auto Cleanup = [&]() {
    sys::fs::remove(IRPath);
    sys::fs::remove(MainPath);
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

  ErrorOr<std::string> Compiler = sys::findProgramByName(CCompiler);
  if (!Compiler) {
    WithColor::error(errs(), "basic09c")
        << "cannot find compiler '" << CCompiler
        << "': " << Compiler.getError().message() << '\n';
    Cleanup();
    return 1;
  }

  std::vector<StringRef> Args = {*Compiler, IRPath, MainPath, "-o",
                                 OutputFilename};
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
