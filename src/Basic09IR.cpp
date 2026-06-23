//===- Basic09IR.cpp - BASIC09 LLVM IR lowering --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Basic09IR.h"
#include "Basic09AST.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/WithColor.h"
#include "llvm/TargetParser/Triple.h"
#include <cstdint>
#include <vector>

using namespace llvm;
using namespace llvm::basic09;

namespace {

enum class BasicKind { Byte, Integer, Real, String, Record };

struct LocalInfo {
  Value *Slot = nullptr;
  Type *StorageTy = nullptr;
  Type *ElementTy = nullptr;
  BasicKind Kind = BasicKind::Integer;
  bool IsArray = false;
  uint64_t StringLength = 0;
};

struct ParamInfo {
  std::string Name;
  Type *StorageTy = nullptr;
  Type *ElementTy = nullptr;
  BasicKind Kind = BasicKind::Integer;
  uint64_t StringLength = 0;
};

struct ProcedureInfo {
  Function *Fn = nullptr;
  std::vector<ParamInfo> Params;
};

class IREmitter {
public:
  IREmitter(StringRef ModuleName, StringRef Triple, raw_ostream &Errs)
      : Errs(Errs), Builder(Context),
        M(std::make_unique<Module>(ModuleName, Context)) {
    M->setTargetTriple(llvm::Triple(Triple));
  }

  bool emit(const ASTNode &Root, raw_ostream &OS) {
    if (Root.Kind != "Program")
      return error(Root, "expected Program root");

    std::vector<const ASTNode *> ProceduresToEmit;
    std::vector<const ASTNode *> TopLevelStatements;
    for (const std::unique_ptr<ASTNode> &Child : Root.Children) {
      if (Child->Kind == "Procedure") {
        if (!declareProcedure(*Child))
          return false;
        ProceduresToEmit.push_back(Child.get());
        continue;
      }
      TopLevelStatements.push_back(Child.get());
    }
    for (const ASTNode *Procedure : ProceduresToEmit)
      if (!emitProcedure(*Procedure))
        return false;
    if (!TopLevelStatements.empty() && !emitMain(TopLevelStatements))
      return false;

    if (verifyModule(*M, &Errs))
      return false;

    M->print(OS, nullptr);
    return true;
  }

private:
  raw_ostream &Errs;
  LLVMContext Context;
  IRBuilder<> Builder;
  std::unique_ptr<Module> M;
  Function *CurrentFunction = nullptr;
  StringMap<ProcedureInfo> Procedures;
  StringMap<LocalInfo> Locals;
  StringMap<BasicBlock *> LabelBlocks;
  std::vector<BasicBlock *> LoopExits;
  std::vector<double> DataValues;
  std::vector<BasicBlock *> GosubReturnBlocks;
  std::vector<SwitchInst *> ReturnSwitches;
  AllocaInst *GosubStack = nullptr;
  AllocaInst *GosubSP = nullptr;
  size_t DataCursor = 0;

  Type *i16Ty() { return Type::getInt16Ty(Context); }
  Type *i32Ty() { return Type::getInt32Ty(Context); }
  Type *i64Ty() { return Type::getInt64Ty(Context); }
  Type *doubleTy() { return Type::getDoubleTy(Context); }

  bool error(const ASTNode &Node, StringRef Message) {
    WithColor::error(Errs, "basic09c")
        << "line " << Node.Line << ": " << Message << '\n';
    return false;
  }

  static const ASTNode *firstChildKind(const ASTNode &Node, StringRef Kind) {
    for (const std::unique_ptr<ASTNode> &Child : Node.Children)
      if (Child->Kind == Kind)
        return Child.get();
    return nullptr;
  }

  static std::string procedureName(const ASTNode &Procedure) {
    StringRef Text = Procedure.Text;
    if (!Text.consume_front("PROCEDURE"))
      return "main";
    Text = Text.trim();
    size_t End = Text.find_first_of(" (");
    return Text.substr(0, End).lower();
  }

  bool declareProcedure(const ASTNode &Procedure) {
    std::string Name = procedureName(Procedure);
    if (Name.empty())
      return error(Procedure, "procedure has no name");
    ProcedureInfo Info;
    if (!collectProcedureParams(Procedure, Info.Params))
      return false;

    std::vector<Type *> ArgTys(Info.Params.size(),
                               PointerType::getUnqual(Context));
    FunctionType *FT = FunctionType::get(i32Ty(), ArgTys, false);
    Info.Fn = Function::Create(FT, Function::ExternalLinkage, Name, *M);
    Procedures[Name] = std::move(Info);
    return true;
  }

  bool emitProcedure(const ASTNode &Procedure) {
    std::string Name = procedureName(Procedure);
    if (Name.empty())
      return error(Procedure, "procedure has no name");

    auto ProcIt = Procedures.find(Name);
    if (ProcIt == Procedures.end())
      return error(Procedure, "procedure was not declared: " + Name);
    ProcedureInfo &Proc = ProcIt->second;
    CurrentFunction = Proc.Fn;
    BasicBlock *Entry = BasicBlock::Create(Context, "entry", CurrentFunction);
    Builder.SetInsertPoint(Entry);
    Locals.clear();
    DataValues.clear();
    GosubReturnBlocks.clear();
    ReturnSwitches.clear();
    DataCursor = 0;
    initializeGosubStack();

    unsigned Index = 0;
    for (Argument &Arg : CurrentFunction->args()) {
      const ParamInfo &Param = Proc.Params[Index++];
      Arg.setName(Param.Name);
      Locals[Param.Name] = {&Arg, Param.StorageTy, Param.ElementTy, Param.Kind,
                            false, Param.StringLength};
    }

    const ASTNode *Body = firstChildKind(Procedure, "Block");
    if (Body)
      collectLabels(*Body);
    if (Body && !emitBlock(*Body))
      return false;

    if (!Builder.GetInsertBlock()->hasTerminator())
      Builder.CreateRet(ConstantInt::get(i32Ty(), 0));

    CurrentFunction = nullptr;
    Locals.clear();
    LabelBlocks.clear();
    DataValues.clear();
    GosubReturnBlocks.clear();
    ReturnSwitches.clear();
    GosubStack = nullptr;
    GosubSP = nullptr;
    DataCursor = 0;
    return true;
  }

  bool collectProcedureParams(const ASTNode &Procedure,
                              std::vector<ParamInfo> &Params) {
    const ASTNode *Body = firstChildKind(Procedure, "Block");
    if (!Body)
      return true;
    for (const std::unique_ptr<ASTNode> &Stmt : Body->Children) {
      if (Stmt->Kind != "Param")
        continue;
      for (const std::unique_ptr<ASTNode> &Decl : Stmt->Children) {
        if (Decl->Kind != "Decl")
          continue;
        const ASTNode *TypeName = firstChildKind(*Decl, "TypeName");
        StringRef BasicType = TypeName ? StringRef(TypeName->Text) : "INTEGER";
        ParamInfo Param;
        Param.Name = Decl->Text;
        if (!getBasicType(*Decl, BasicType, Param.Kind, Param.ElementTy,
                          Param.StringLength))
          return false;
        Param.StorageTy = Param.ElementTy;
        Params.push_back(Param);
      }
    }
    return true;
  }

  bool emitMain(ArrayRef<const ASTNode *> Statements) {
    FunctionType *FT = FunctionType::get(i32Ty(), false);
    CurrentFunction = Function::Create(FT, Function::ExternalLinkage, "main", *M);
    BasicBlock *Entry = BasicBlock::Create(Context, "entry", CurrentFunction);
    Builder.SetInsertPoint(Entry);
    Locals.clear();
    LabelBlocks.clear();
    DataValues.clear();
    GosubReturnBlocks.clear();
    ReturnSwitches.clear();
    DataCursor = 0;
    initializeGosubStack();

    for (const ASTNode *Stmt : Statements)
      if (!emitStatement(*Stmt))
        return false;

    if (!Builder.GetInsertBlock()->hasTerminator())
      Builder.CreateRet(ConstantInt::get(i32Ty(), 0));

    CurrentFunction = nullptr;
    Locals.clear();
    LabelBlocks.clear();
    DataValues.clear();
    GosubReturnBlocks.clear();
    ReturnSwitches.clear();
    GosubStack = nullptr;
    GosubSP = nullptr;
    DataCursor = 0;
    return true;
  }

  bool emitBlock(const ASTNode &Block) {
    for (const std::unique_ptr<ASTNode> &Stmt : Block.Children)
      if (!emitStatement(*Stmt))
        return false;
    return true;
  }

  bool emitStatement(const ASTNode &Stmt) {
    if (Stmt.Kind == "Dim")
      return emitDim(Stmt);
    if (Stmt.Kind == "Assign" || Stmt.Kind == "Let")
      return emitAssign(Stmt);
    if (Stmt.Kind == "Print")
      return emitPrint(Stmt);
    if (Stmt.Kind == "PrintUsing")
      return emitPrintUsing(Stmt);
    if (Stmt.Kind == "If")
      return emitIf(Stmt);
    if (Stmt.Kind == "For")
      return emitFor(Stmt);
    if (Stmt.Kind == "While")
      return emitWhile(Stmt);
    if (Stmt.Kind == "Repeat")
      return emitRepeat(Stmt);
    if (Stmt.Kind == "Loop")
      return emitLoop(Stmt);
    if (Stmt.Kind == "Exit")
      return emitExit(Stmt);
    if (Stmt.Kind == "ExitIf")
      return emitExitIf(Stmt);
    if (Stmt.Kind == "Run")
      return emitRun(Stmt);
    if (Stmt.Kind == "Data")
      return emitData(Stmt);
    if (Stmt.Kind == "Restore")
      return emitRestore(Stmt);
    if (Stmt.Kind == "Read")
      return emitRead(Stmt);
    if (Stmt.Kind == "Input")
      return emitInput(Stmt);
    if (Stmt.Kind == "Label")
      return emitLabel(Stmt);
    if (Stmt.Kind == "BranchTarget")
      return emitBranchTarget(Stmt);
    if (Stmt.Kind == "Goto")
      return emitBranchStatement(Stmt);
    if (Stmt.Kind == "Gosub")
      return emitGosub(Stmt);
    if (Stmt.Kind == "OnGoto" || Stmt.Kind == "OnGosub")
      return emitComputedBranch(Stmt);
    if (Stmt.Kind == "Type" || Stmt.Kind == "Param")
      return true;
    if (Stmt.Kind == "EndIf")
      return true;
    if (Stmt.Kind == "Return")
      return emitReturn(Stmt);
    if (Stmt.Kind == "End" || Stmt.Kind == "Stop") {
      if (!Builder.GetInsertBlock()->hasTerminator())
        Builder.CreateRet(ConstantInt::get(i32Ty(), 0));
      return true;
    }
    return error(Stmt, "statement is not supported by LLVM IR lowering yet: " +
                           Stmt.Kind);
  }

  bool emitDim(const ASTNode &Stmt) {
    for (const std::unique_ptr<ASTNode> &Decl : Stmt.Children) {
      if (Decl->Kind != "Decl")
        continue;
      const ASTNode *TypeName = firstChildKind(*Decl, "TypeName");
      StringRef BasicType = TypeName ? StringRef(TypeName->Text) : "INTEGER";
      BasicKind Kind;
      Type *ElementTy = nullptr;
      uint64_t StringLength = 0;
      if (!getBasicType(*Decl, BasicType, Kind, ElementTy, StringLength))
        return false;

      Type *StorageTy = ElementTy;
      std::vector<uint64_t> Bounds;
      for (const std::unique_ptr<ASTNode> &Child : Decl->Children)
        if (Child->Kind == "Bound")
          Bounds.push_back(constantExtent(*Child));
      for (uint64_t Bound : llvm::reverse(Bounds))
        StorageTy = ArrayType::get(StorageTy, Bound + 1);

      AllocaInst *Slot = Builder.CreateAlloca(StorageTy, nullptr, Decl->Text);
      Builder.CreateStore(Constant::getNullValue(StorageTy), Slot);
      Locals[Decl->Text] = {Slot, StorageTy, ElementTy, Kind, !Bounds.empty(),
                            StringLength};
    }
    return true;
  }

  void collectLabels(const ASTNode &Node) {
    if (Node.Kind == "Label") {
      std::string Name = normalizedLabel(Node.Text);
      if (!LabelBlocks.count(Name))
        LabelBlocks[Name] = BasicBlock::Create(Context, Name, CurrentFunction);
    }
    for (const std::unique_ptr<ASTNode> &Child : Node.Children)
      collectLabels(*Child);
  }

  bool emitLabel(const ASTNode &Stmt) {
    std::string Name = normalizedLabel(Stmt.Text);
    auto It = LabelBlocks.find(Name);
    if (It == LabelBlocks.end())
      return error(Stmt, "unknown label: " + Name);
    if (!Builder.GetInsertBlock()->hasTerminator())
      Builder.CreateBr(It->second);
    Builder.SetInsertPoint(It->second);
    return true;
  }

  bool emitBranchStatement(const ASTNode &Stmt) {
    const ASTNode *Target = firstChildKind(Stmt, "BranchTarget");
    if (!Target)
      return error(Stmt, "branch statement is missing a target");
    return emitBranchTarget(*Target);
  }

  bool emitGosub(const ASTNode &Stmt) {
    const ASTNode *Target = firstChildKind(Stmt, "BranchTarget");
    if (!Target)
      return error(Stmt, "GOSUB is missing a target");
    BasicBlock *TargetBB = lookupLabel(*Target);
    if (!TargetBB)
      return false;
    BasicBlock *ReturnBB = createGosubReturnBlock();
    emitPushGosubReturn(ReturnBB);
    Builder.CreateBr(TargetBB);
    Builder.SetInsertPoint(ReturnBB);
    return true;
  }

  bool emitComputedBranch(const ASTNode &Stmt) {
    const ASTNode *Selector = firstChildKind(Stmt, "Selector");
    if (!Selector)
      return error(Stmt, Stmt.Kind + " is missing a selector");
    Value *Selected = emitExprChild(*Selector);
    if (!Selected)
      return false;
    Selected = coerceScalar(Selected, i32Ty());

    BasicBlock *Fallthrough =
        BasicBlock::Create(Context, "computed.next", CurrentFunction);
    if (Stmt.Kind == "OnGosub")
      emitPushGosubReturn(createGosubReturnBlock(Fallthrough));

    SwitchInst *Switch = Builder.CreateSwitch(Selected, Fallthrough);
    int64_t CaseValue = 1;
    for (const std::unique_ptr<ASTNode> &Child : Stmt.Children) {
      if (Child->Kind != "BranchTarget")
        continue;
      BasicBlock *TargetBB = lookupLabel(*Child);
      if (!TargetBB)
        return false;
      Switch->addCase(ConstantInt::get(cast<IntegerType>(i32Ty()), CaseValue++),
                      TargetBB);
    }
    Builder.SetInsertPoint(Fallthrough);
    return true;
  }

  bool emitBranchTarget(const ASTNode &Stmt) {
    std::string Name = normalizedLabel(Stmt.Text);
    auto It = LabelBlocks.find(Name);
    if (It == LabelBlocks.end())
      return error(Stmt, "unknown branch target: " + Name);
    if (!Builder.GetInsertBlock()->hasTerminator())
      Builder.CreateBr(It->second);
    BasicBlock *AfterBranch =
        BasicBlock::Create(Context, "after.branch", CurrentFunction);
    Builder.SetInsertPoint(AfterBranch);
    return true;
  }

  bool emitReturn(const ASTNode &) {
    Value *SP = Builder.CreateLoad(i32Ty(), GosubSP, "gosub.sp");
    Value *HasReturn =
        Builder.CreateICmpSGE(SP, ConstantInt::get(i32Ty(), 0));
    BasicBlock *DispatchBB =
        BasicBlock::Create(Context, "return.dispatch", CurrentFunction);
    BasicBlock *FunctionReturnBB =
        BasicBlock::Create(Context, "return.function", CurrentFunction);
    Builder.CreateCondBr(HasReturn, DispatchBB, FunctionReturnBB);

    Builder.SetInsertPoint(FunctionReturnBB);
    Builder.CreateRet(ConstantInt::get(i32Ty(), 0));

    Builder.SetInsertPoint(DispatchBB);
    Value *Slot = Builder.CreateInBoundsGEP(
        GosubStack->getAllocatedType(), GosubStack,
        {ConstantInt::get(i32Ty(), 0), SP});
    Value *ReturnID = Builder.CreateLoad(i32Ty(), Slot, "gosub.return");
    Builder.CreateStore(Builder.CreateSub(SP, ConstantInt::get(i32Ty(), 1)),
                        GosubSP);
    SwitchInst *Switch = Builder.CreateSwitch(ReturnID, FunctionReturnBB);
    for (unsigned I = 0, E = GosubReturnBlocks.size(); I != E; ++I)
      Switch->addCase(ConstantInt::get(cast<IntegerType>(i32Ty()), I),
                      GosubReturnBlocks[I]);
    ReturnSwitches.push_back(Switch);

    BasicBlock *AfterReturn =
        BasicBlock::Create(Context, "after.return", CurrentFunction);
    Builder.SetInsertPoint(AfterReturn);
    return true;
  }

  void initializeGosubStack() {
    GosubStack =
        Builder.CreateAlloca(ArrayType::get(i32Ty(), 128), nullptr,
                             "gosub.stack");
    GosubSP = Builder.CreateAlloca(i32Ty(), nullptr, "gosub.sp");
    Builder.CreateStore(ConstantInt::getSigned(i32Ty(), -1), GosubSP);
  }

  BasicBlock *lookupLabel(const ASTNode &Target) {
    std::string Name = normalizedLabel(Target.Text);
    auto It = LabelBlocks.find(Name);
    if (It == LabelBlocks.end()) {
      error(Target, "unknown branch target: " + Name);
      return nullptr;
    }
    return It->second;
  }

  BasicBlock *createGosubReturnBlock(BasicBlock *ReturnBB = nullptr) {
    if (!ReturnBB)
      ReturnBB = BasicBlock::Create(Context, "gosub.return", CurrentFunction);
    unsigned ReturnID = GosubReturnBlocks.size();
    GosubReturnBlocks.push_back(ReturnBB);
    for (SwitchInst *Switch : ReturnSwitches)
      Switch->addCase(ConstantInt::get(cast<IntegerType>(i32Ty()), ReturnID),
                      ReturnBB);
    return ReturnBB;
  }

  void emitPushGosubReturn(BasicBlock *ReturnBB) {
    unsigned ReturnID = 0;
    for (unsigned I = 0, E = GosubReturnBlocks.size(); I != E; ++I)
      if (GosubReturnBlocks[I] == ReturnBB) {
        ReturnID = I;
        break;
      }

    Value *SP = Builder.CreateLoad(i32Ty(), GosubSP, "gosub.sp");
    Value *NextSP = Builder.CreateAdd(SP, ConstantInt::get(i32Ty(), 1));
    Builder.CreateStore(NextSP, GosubSP);
    Value *Slot = Builder.CreateInBoundsGEP(
        GosubStack->getAllocatedType(), GosubStack,
        {ConstantInt::get(i32Ty(), 0), NextSP});
    Builder.CreateStore(ConstantInt::get(i32Ty(), ReturnID), Slot);
  }

  bool emitData(const ASTNode &Stmt) {
    for (const std::unique_ptr<ASTNode> &Value : Stmt.Children) {
      double Number = 0.0;
      if (!constantDataNumber(*Value, Number))
        return error(*Value, "only numeric DATA values are supported");
      DataValues.push_back(Number);
    }
    return true;
  }

  bool emitRestore(const ASTNode &) {
    DataCursor = 0;
    return true;
  }

  bool emitRead(const ASTNode &Stmt) {
    for (const std::unique_ptr<ASTNode> &Target : Stmt.Children) {
      if (Target->Kind != "Target")
        continue;
      if (DataCursor >= DataValues.size())
        return error(*Target, "READ ran past available DATA values");
      if (!emitStoreDesignator(Target->Text,
                               ConstantFP::get(doubleTy(),
                                               DataValues[DataCursor++]),
                               *Target))
        return false;
    }
    return true;
  }

  bool emitInput(const ASTNode &Stmt) {
    if (const ASTNode *Prompt = firstChildKind(Stmt, "Prompt")) {
      if (const ASTNode *String = firstChildKind(*Prompt, "String"))
        Builder.CreateCall(getPrintf(),
                           {Builder.CreateGlobalString(unquote(String->Text))});
    }
    Builder.CreateCall(getPrintf(), {Builder.CreateGlobalString("? ")});
    Builder.CreateCall(getFflush(),
                       {ConstantPointerNull::get(PointerType::getUnqual(
                           Context))});

    for (const std::unique_ptr<ASTNode> &Target : Stmt.Children) {
      if (Target->Kind != "Target")
        continue;
      if (!emitScanDesignator(Target->Text, *Target))
        return false;
    }
    return true;
  }

  bool emitAssign(const ASTNode &Stmt) {
    const ASTNode *LHS = firstChildKind(Stmt, "LHS");
    const ASTNode *RHS = firstChildKind(Stmt, "RHS");
    if (!LHS || !RHS)
      return error(Stmt, "assignment is missing an operand");
    if (StringRef(LHS->Text).contains("("))
      return emitArrayAssign(*LHS, *RHS);
    if (StringRef(LHS->Text).contains("."))
      return emitRecordAssign(*LHS, *RHS);
    LocalInfo *Local = getOrCreateScalarLocal(LHS->Text);
    if (!Local)
      return false;
    if (Local->IsArray)
      return error(*LHS,
                   "array assignment is not supported by LLVM IR lowering yet");
    if (Local->Kind == BasicKind::String) {
      const ASTNode *String = firstChildKind(*RHS, "String");
      if (String) {
        emitStringStore(Local->ElementTy, Local->Slot, unquote(String->Text));
        return true;
      }
      Value *Text = emitExprChild(*RHS);
      if (!Text)
        return false;
      if (!Text->getType()->isPointerTy())
        return error(*RHS, "STRING assignment requires a string expression");
      Builder.CreateCall(getStrcpy(),
                         {stringDataPtr(Local->ElementTy, Local->Slot), Text});
      return true;
    }
    Value *V = emitExprChild(*RHS);
    if (!V)
      return false;
    Type *DestTy = Local->ElementTy;
    V = coerceScalar(V, DestTy);
    Builder.CreateStore(V, Local->Slot);
    return true;
  }

  bool emitPrint(const ASTNode &Stmt) {
    FunctionCallee Printf = getPrintf();

    for (const std::unique_ptr<ASTNode> &Child : Stmt.Children) {
      if (Child->Kind == "Separator")
        continue;
      if (Child->Kind != "Item")
        continue;
      const ASTNode *Expr = firstChildKind(*Child, "String");
      if (Expr) {
        Builder.CreateCall(Printf,
                           {Builder.CreateGlobalString(unquote(Expr->Text))});
        continue;
      }
      const ASTNode *Call = firstChildKind(*Child, "Call");
      if (Call && Call->Text == "TAB") {
        if (!emitPrintTab(*Call))
          return false;
        continue;
      }
      Value *V = emitExprChild(*Child);
      if (!V)
        return false;
      if (V->getType()->isPointerTy()) {
        Builder.CreateCall(Printf, {Builder.CreateGlobalString("%s"), V});
      } else if (V->getType()->isDoubleTy()) {
        Builder.CreateCall(Printf, {Builder.CreateGlobalString("%g"), V});
      } else if (V->getType()->isIntegerTy()) {
        V = Builder.CreateSExtOrTrunc(V, i32Ty());
        Builder.CreateCall(Printf, {Builder.CreateGlobalString("%d"), V});
      } else {
        return error(*Child, "PRINT expression has unsupported IR type");
      }
    }

    if (shouldPrintNewline(Stmt))
      Builder.CreateCall(Printf, {Builder.CreateGlobalString("\n")});
    return true;
  }

  bool shouldPrintNewline(const ASTNode &Stmt) const {
    if (Stmt.Children.empty())
      return true;
    const ASTNode &Last = *Stmt.Children.back();
    return Last.Kind != "Separator";
  }

  bool emitPrintUsing(const ASTNode &Stmt) {
    if (const ASTNode *Format = firstChildKind(Stmt, "Format"))
      if (const ASTNode *String = firstChildKind(*Format, "String")) {
        std::string Text = printUsingLabel(unquote(String->Text));
        if (!Text.empty())
          Builder.CreateCall(getPrintf(), {Builder.CreateGlobalString(Text)});
      }

    for (const std::unique_ptr<ASTNode> &Child : Stmt.Children) {
      if (Child->Kind != "Item")
        continue;
      Value *V = emitExprChild(*Child);
      if (!V)
        return false;
      if (V->getType()->isPointerTy())
        Builder.CreateCall(getPrintf(), {Builder.CreateGlobalString("%s"), V});
      else if (V->getType()->isDoubleTy())
        Builder.CreateCall(getPrintf(), {Builder.CreateGlobalString("%g"), V});
      else if (V->getType()->isIntegerTy())
        Builder.CreateCall(
            getPrintf(),
            {Builder.CreateGlobalString("%d"),
             Builder.CreateSExtOrTrunc(V, i32Ty())});
    }
    Builder.CreateCall(getPrintf(), {Builder.CreateGlobalString("\n")});
    return true;
  }

  bool emitPrintTab(const ASTNode &Call) {
    if (Call.Children.size() != 1)
      return error(Call, "TAB expects one argument");
    Value *Width = emitExpr(*Call.Children.front());
    if (!Width)
      return false;
    Width = coerceScalar(Width, i32Ty());
    Builder.CreateCall(getPrintf(),
                       {Builder.CreateGlobalString("%*s"), Width,
                        Builder.CreateGlobalString("")});
    return true;
  }

  bool emitArrayAssign(const ASTNode &LHS, const ASTNode &RHS) {
    StringRef Name = arrayDesignatorName(LHS.Text);
    auto It = Locals.find(Name);
    if (It == Locals.end() || !It->second.IsArray)
      return error(LHS, "assignment target is not a local array: " + LHS.Text);
    Value *Ptr = emitArrayElementPtr(LHS.Text, LHS);
    if (!Ptr)
      return false;
    if (It->second.Kind == BasicKind::String) {
      const ASTNode *String = firstChildKind(RHS, "String");
      if (String) {
        emitStringStore(It->second.ElementTy, Ptr, unquote(String->Text));
        return true;
      }
      Value *Text = emitExprChild(RHS);
      if (!Text)
        return false;
      if (!Text->getType()->isPointerTy())
        return error(RHS, "STRING array assignment requires a string expression");
      Builder.CreateCall(getStrcpy(), {stringDataPtr(It->second.ElementTy, Ptr),
                                       Text});
      return true;
    }
    Value *V = emitExprChild(RHS);
    if (!V)
      return false;
    Type *ElemTy = recordFieldName(LHS.Text).empty() ? It->second.ElementTy
                                                     : doubleTy();
    V = coerceScalar(V, ElemTy);
    Builder.CreateStore(V, Ptr);
    return true;
  }

  bool emitRecordAssign(const ASTNode &LHS, const ASTNode &RHS) {
    StringRef Name;
    StringRef Field;
    std::tie(Name, Field) = StringRef(LHS.Text).split('.');
    auto It = Locals.find(Name.trim());
    if (It == Locals.end() || It->second.Kind != BasicKind::Record)
      return error(LHS, "assignment target is not a record: " + LHS.Text);
    Value *Ptr = Builder.CreateInBoundsGEP(
        It->second.StorageTy, It->second.Slot,
        {ConstantInt::get(i32Ty(), 0),
         ConstantInt::get(i32Ty(), recordFieldIndex(Field.trim()))});
    Value *V = emitExprChild(RHS);
    if (!V)
      return false;
    Builder.CreateStore(coerceScalar(V, doubleTy()), Ptr);
    return true;
  }

  bool emitStoreDesignator(StringRef Designator, Value *V, const ASTNode &At) {
    if (Designator.contains("(")) {
      StringRef Name = arrayDesignatorName(Designator);
      auto It = Locals.find(Name);
      if (It == Locals.end() || !It->second.IsArray)
        return error(At, ("unknown array: " + Name).str());
      Value *Ptr = emitArrayElementPtr(Designator, At);
      if (!Ptr)
        return false;
      Type *ElemTy = recordFieldName(Designator).empty() ? It->second.ElementTy
                                                         : doubleTy();
      Builder.CreateStore(coerceScalar(V, ElemTy), Ptr);
      return true;
    }

    LocalInfo *Local = getOrCreateScalarLocal(Designator);
    if (!Local)
      return false;
    Builder.CreateStore(coerceScalar(V, Local->ElementTy), Local->Slot);
    return true;
  }

  bool emitScanDesignator(StringRef Designator, const ASTNode &At) {
    LocalInfo *Local = nullptr;
    Value *Ptr = nullptr;
    Type *ElemTy = nullptr;
    BasicKind Kind = BasicKind::Integer;

    if (Designator.contains("(")) {
      StringRef Name = arrayDesignatorName(Designator);
      auto It = Locals.find(Name);
      if (It == Locals.end() || !It->second.IsArray)
        return error(At, ("unknown array: " + Name).str());
      Local = &It->second;
      Ptr = emitArrayElementPtr(Designator, At);
      ElemTy = recordFieldName(Designator).empty() ? Local->ElementTy
                                                   : doubleTy();
      Kind = Local->Kind;
    } else {
      Local = getOrCreateScalarLocal(Designator);
      if (!Local)
        return false;
      Ptr = Local->Slot;
      ElemTy = Local->ElementTy;
      Kind = Local->Kind;
    }

    if (!Ptr)
      return false;
    if (Kind == BasicKind::String) {
      Builder.CreateCall(getScanf(),
                         {Builder.CreateGlobalString("%255s"),
                          stringDataPtr(ElemTy, Ptr)});
      return true;
    }
    if (ElemTy->isDoubleTy()) {
      Builder.CreateCall(getScanf(), {Builder.CreateGlobalString("%lf"), Ptr});
      return true;
    }
    if (ElemTy->isIntegerTy(8)) {
      Builder.CreateCall(getScanf(), {Builder.CreateGlobalString("%hhd"), Ptr});
      return true;
    }
    Builder.CreateCall(getScanf(), {Builder.CreateGlobalString("%hd"), Ptr});
    return true;
  }

  bool emitIf(const ASTNode &Stmt) {
    const ASTNode *Condition = firstChildKind(Stmt, "Condition");
    const ASTNode *ThenBlock = firstChildKind(Stmt, "Block");
    const ASTNode *Else = firstChildKind(Stmt, "Else");
    const ASTNode *ElseBlock = Else ? firstChildKind(*Else, "Block") : nullptr;
    if (!Condition || !ThenBlock)
      return error(Stmt, "IF is missing a condition or THEN block");

    Value *Cond = emitExprChild(*Condition);
    if (!Cond)
      return false;
    Cond = toBool(Cond);

    BasicBlock *ThenBB =
        BasicBlock::Create(Context, "if.then", CurrentFunction);
    BasicBlock *ElseBB = ElseBlock
                             ? BasicBlock::Create(Context, "if.else",
                                                  CurrentFunction)
                             : nullptr;
    BasicBlock *MergeBB =
        BasicBlock::Create(Context, "if.end", CurrentFunction);

    Builder.CreateCondBr(Cond, ThenBB, ElseBB ? ElseBB : MergeBB);

    Builder.SetInsertPoint(ThenBB);
    if (!emitBlock(*ThenBlock))
      return false;
    if (!Builder.GetInsertBlock()->hasTerminator())
      Builder.CreateBr(MergeBB);

    if (ElseBB) {
      Builder.SetInsertPoint(ElseBB);
      if (!emitBlock(*ElseBlock))
        return false;
      if (!Builder.GetInsertBlock()->hasTerminator())
        Builder.CreateBr(MergeBB);
    }

    Builder.SetInsertPoint(MergeBB);
    return true;
  }

  bool emitFor(const ASTNode &Stmt) {
    const ASTNode *Variable = firstChildKind(Stmt, "Variable");
    const ASTNode *Start = firstChildKind(Stmt, "Start");
    const ASTNode *End = firstChildKind(Stmt, "End");
    const ASTNode *Step = firstChildKind(Stmt, "Step");
    const ASTNode *Body = firstChildKind(Stmt, "Block");
    const ASTNode *NextVariable = firstChildKind(Stmt, "NextVariable");
    if (!Variable || !Start || !End || !Body)
      return error(Stmt, "FOR is missing a variable, range, or body");
    if (NextVariable && NextVariable->Text != Variable->Text)
      return error(*NextVariable, "NEXT variable does not match FOR variable");

    LocalInfo *Local = getOrCreateScalarLocal(Variable->Text);
    if (!Local)
      return false;
    if (Local->IsArray)
      return error(*Variable, "FOR variable cannot be an array: " +
                                  Variable->Text);

    Value *Slot = Local->Slot;
    Type *VarTy = Local->ElementTy;
    Value *StartValue = emitExprChild(*Start);
    if (!StartValue)
      return false;
    Builder.CreateStore(coerceScalar(StartValue, VarTy), Slot);

    Value *EndValue = emitExprChild(*End);
    if (!EndValue)
      return false;
    EndValue = coerceScalar(EndValue, VarTy);
    AllocaInst *EndSlot = createEntryAlloca(VarTy, Variable->Text + ".end");
    Builder.CreateStore(EndValue, EndSlot);

    Value *StepValue = Step ? emitExprChild(*Step) : numericConstant(VarTy, 1);
    if (!StepValue)
      return false;
    StepValue = coerceScalar(StepValue, VarTy);
    AllocaInst *StepSlot = createEntryAlloca(VarTy, Variable->Text + ".step");
    Builder.CreateStore(StepValue, StepSlot);

    BasicBlock *CondBB =
        BasicBlock::Create(Context, "for.cond", CurrentFunction);
    BasicBlock *BodyBB =
        BasicBlock::Create(Context, "for.body", CurrentFunction);
    BasicBlock *IncBB = BasicBlock::Create(Context, "for.inc", CurrentFunction);
    BasicBlock *EndBB = BasicBlock::Create(Context, "for.end", CurrentFunction);

    Builder.CreateBr(CondBB);

    Builder.SetInsertPoint(CondBB);
    Value *Current = Builder.CreateLoad(VarTy, Slot, Variable->Text);
    EndValue = Builder.CreateLoad(VarTy, EndSlot, Variable->Text + ".end");
    StepValue = Builder.CreateLoad(VarTy, StepSlot, Variable->Text + ".step");
    Value *StepIsNegative =
        VarTy->isDoubleTy()
            ? Builder.CreateFCmpOLT(StepValue, numericConstant(VarTy, 0))
            : Builder.CreateICmpSLT(StepValue, numericConstant(VarTy, 0));
    Value *ForwardCond = VarTy->isDoubleTy()
                             ? Builder.CreateFCmpOLE(Current, EndValue)
                             : Builder.CreateICmpSLE(Current, EndValue);
    Value *ReverseCond = VarTy->isDoubleTy()
                             ? Builder.CreateFCmpOGE(Current, EndValue)
                             : Builder.CreateICmpSGE(Current, EndValue);
    Builder.CreateCondBr(Builder.CreateSelect(StepIsNegative, ReverseCond,
                                              ForwardCond),
                         BodyBB, EndBB);

    Builder.SetInsertPoint(BodyBB);
    LoopExits.push_back(EndBB);
    if (!emitBlock(*Body))
      return false;
    LoopExits.pop_back();
    if (!Builder.GetInsertBlock()->hasTerminator())
      Builder.CreateBr(IncBB);

    Builder.SetInsertPoint(IncBB);
    Value *IncValue = Builder.CreateLoad(VarTy, Slot, Variable->Text);
    StepValue = Builder.CreateLoad(VarTy, StepSlot, Variable->Text + ".step");
    IncValue = VarTy->isDoubleTy() ? Builder.CreateFAdd(IncValue, StepValue)
                                   : Builder.CreateAdd(IncValue, StepValue);
    Builder.CreateStore(IncValue, Slot);
    Builder.CreateBr(CondBB);

    Builder.SetInsertPoint(EndBB);
    return true;
  }

  bool emitWhile(const ASTNode &Stmt) {
    const ASTNode *Condition = firstChildKind(Stmt, "Condition");
    const ASTNode *Body = firstChildKind(Stmt, "Block");
    if (!Condition || !Body)
      return error(Stmt, "WHILE is missing a condition or body");

    BasicBlock *CondBB =
        BasicBlock::Create(Context, "while.cond", CurrentFunction);
    BasicBlock *BodyBB =
        BasicBlock::Create(Context, "while.body", CurrentFunction);
    BasicBlock *EndBB =
        BasicBlock::Create(Context, "while.end", CurrentFunction);

    Builder.CreateBr(CondBB);

    Builder.SetInsertPoint(CondBB);
    Value *Cond = emitExprChild(*Condition);
    if (!Cond)
      return false;
    Builder.CreateCondBr(toBool(Cond), BodyBB, EndBB);

    Builder.SetInsertPoint(BodyBB);
    LoopExits.push_back(EndBB);
    if (!emitBlock(*Body))
      return false;
    LoopExits.pop_back();
    if (!Builder.GetInsertBlock()->hasTerminator())
      Builder.CreateBr(CondBB);

    Builder.SetInsertPoint(EndBB);
    return true;
  }

  bool emitRepeat(const ASTNode &Stmt) {
    const ASTNode *Body = firstChildKind(Stmt, "Block");
    const ASTNode *Condition = firstChildKind(Stmt, "Condition");
    if (!Body || !Condition)
      return error(Stmt, "REPEAT is missing a body or UNTIL condition");

    BasicBlock *BodyBB =
        BasicBlock::Create(Context, "repeat.body", CurrentFunction);
    BasicBlock *CondBB =
        BasicBlock::Create(Context, "repeat.cond", CurrentFunction);
    BasicBlock *EndBB =
        BasicBlock::Create(Context, "repeat.end", CurrentFunction);

    Builder.CreateBr(BodyBB);

    Builder.SetInsertPoint(BodyBB);
    LoopExits.push_back(EndBB);
    if (!emitBlock(*Body))
      return false;
    LoopExits.pop_back();
    if (!Builder.GetInsertBlock()->hasTerminator())
      Builder.CreateBr(CondBB);

    Builder.SetInsertPoint(CondBB);
    Value *Cond = emitExprChild(*Condition);
    if (!Cond)
      return false;
    Builder.CreateCondBr(toBool(Cond), EndBB, BodyBB);

    Builder.SetInsertPoint(EndBB);
    return true;
  }

  bool emitLoop(const ASTNode &Stmt) {
    const ASTNode *Body = firstChildKind(Stmt, "Block");
    if (!Body)
      return error(Stmt, "LOOP is missing a body");

    BasicBlock *BodyBB =
        BasicBlock::Create(Context, "loop.body", CurrentFunction);
    BasicBlock *EndBB =
        BasicBlock::Create(Context, "loop.end", CurrentFunction);

    Builder.CreateBr(BodyBB);

    Builder.SetInsertPoint(BodyBB);
    LoopExits.push_back(EndBB);
    if (!emitBlock(*Body))
      return false;
    LoopExits.pop_back();
    if (!Builder.GetInsertBlock()->hasTerminator())
      Builder.CreateBr(BodyBB);

    Builder.SetInsertPoint(EndBB);
    return true;
  }

  bool emitExit(const ASTNode &Stmt) {
    if (LoopExits.empty())
      return error(Stmt, "EXIT is not inside a loop");
    Builder.CreateBr(LoopExits.back());
    BasicBlock *AfterExit =
        BasicBlock::Create(Context, "after.exit", CurrentFunction);
    Builder.SetInsertPoint(AfterExit);
    return true;
  }

  bool emitExitIf(const ASTNode &Stmt) {
    const ASTNode *Condition = firstChildKind(Stmt, "Condition");
    const ASTNode *Body = firstChildKind(Stmt, "Block");
    if (!Condition)
      return error(Stmt, "EXITIF is missing a condition");
    if (LoopExits.empty())
      return error(Stmt, "EXITIF is not inside a loop");

    Value *Cond = emitExprChild(*Condition);
    if (!Cond)
      return false;
    Cond = toBool(Cond);

    BasicBlock *ThenBB =
        BasicBlock::Create(Context, "exitif.then", CurrentFunction);
    BasicBlock *ContBB =
        BasicBlock::Create(Context, "exitif.end", CurrentFunction);
    BasicBlock *ExitBB = LoopExits.back();

    Builder.CreateCondBr(Cond, ThenBB, ContBB);

    Builder.SetInsertPoint(ThenBB);
    if (Body && !emitBlock(*Body))
      return false;
    if (!Builder.GetInsertBlock()->hasTerminator())
      Builder.CreateBr(ExitBB);

    Builder.SetInsertPoint(ContBB);
    return true;
  }

  bool emitRun(const ASTNode &Stmt) {
    const ASTNode *CallWrapper = firstChildKind(Stmt, "Call");
    if (!CallWrapper)
      return true;
    const ASTNode *Call = CallWrapper;
    if (!CallWrapper->Children.empty() && CallWrapper->Children.front()->Kind == "Call")
      Call = CallWrapper->Children.front().get();

    if (Call->Text == "RANDOMIZE")
      return true;

    auto ProcIt = Procedures.find(StringRef(Call->Text).lower());
    if (ProcIt != Procedures.end())
      return emitProcedureCall(*Call, ProcIt->second);
    if (Call->Text == "RAND8I") {
      if (Call->Children.size() != 1 || Call->Children.front()->Kind != "Var")
        return error(*Call, "RAND8I expects one variable argument");
      LocalInfo *Local = getOrCreateScalarLocal(Call->Children.front()->Text);
      if (!Local)
        return false;
      Value *R = Builder.CreateCall(getZeroDoubleFn("drand48"));
      R = Builder.CreateFMul(R, ConstantFP::get(doubleTy(), 8.0));
      R = Builder.CreateFAdd(R, ConstantFP::get(doubleTy(), 1.0));
      R = Builder.CreateCall(getUnaryDoubleFn("floor"), {R});
      Builder.CreateStore(coerceScalar(R, Local->ElementTy), Local->Slot);
      return true;
    }

    return true;
  }

  bool emitProcedureCall(const ASTNode &Call, const ProcedureInfo &Proc) {
    ArrayRef<std::unique_ptr<ASTNode>> Children = Call.Children;
    bool BareNoArgCall = Proc.Params.empty() && Children.size() == 1 &&
                         Children.front()->Kind == "Var" &&
                         Children.front()->Text == Call.Text;
    if (BareNoArgCall)
      Children = {};

    if (Children.size() != Proc.Params.size())
      return error(Call, "procedure argument count does not match: " +
                             Call.Text);

    std::vector<Value *> Args;
    for (auto Indexed : llvm::enumerate(Children)) {
      Value *Arg =
          emitArgumentAddress(*Indexed.value(), Proc.Params[Indexed.index()]);
      if (!Arg)
        return false;
      Args.push_back(Arg);
    }
    Builder.CreateCall(Proc.Fn, Args);
    return true;
  }

  Value *emitArgumentAddress(const ASTNode &Arg, const ParamInfo &Param) {
    if (Param.Kind == BasicKind::String)
      return emitStringArgumentAddress(Arg, Param);

    if (Arg.Kind == "Var") {
      if (Arg.Text == "TRUE" || Arg.Text == "FALSE" || Arg.Text == "PI") {
        Value *Value = emitExpr(Arg);
        if (!Value)
          return nullptr;
        AllocaInst *Temp = createEntryAlloca(Param.StorageTy, "arg.const.tmp");
        Builder.CreateStore(coerceScalar(Value, Param.ElementTy), Temp);
        return Temp;
      }
      LocalInfo *Local = getOrCreateScalarLocal(Arg.Text);
      if (!Local)
        return nullptr;
      if (Local->Kind != Param.Kind || Local->ElementTy != Param.ElementTy) {
        Value *Value = Builder.CreateLoad(Local->ElementTy, Local->Slot,
                                          Arg.Text + ".arg");
        AllocaInst *Temp = createEntryAlloca(Param.StorageTy, "arg.coerce.tmp");
        Builder.CreateStore(coerceScalar(Value, Param.ElementTy), Temp);
        return Temp;
      }
      return Local->Slot;
    }
    if (Arg.Kind == "Call") {
      auto It = Locals.find(Arg.Text);
      if (It != Locals.end() && It->second.IsArray)
        return emitArrayElementPtr(Arg);
    }

    Value *Value = emitExpr(Arg);
    if (!Value)
      return nullptr;
    AllocaInst *Temp = createEntryAlloca(Param.StorageTy, "arg.tmp");
    Builder.CreateStore(coerceScalar(Value, Param.ElementTy), Temp);
    return Temp;
  }

  Value *emitStringArgumentAddress(const ASTNode &Arg, const ParamInfo &Param) {
    if (Arg.Kind == "Var") {
      LocalInfo *Local = getOrCreateScalarLocal(Arg.Text);
      if (!Local)
        return nullptr;
      if (Local->Kind == BasicKind::String)
        return Local->Slot;
    }
    if (Arg.Kind == "Call") {
      auto It = Locals.find(Arg.Text);
      if (It != Locals.end() && It->second.IsArray &&
          It->second.Kind == BasicKind::String)
        return emitArrayElementPtr(Arg);
    }

    Value *Text = emitExpr(Arg);
    if (!Text)
      return nullptr;
    AllocaInst *Temp = createEntryAlloca(Param.StorageTy, "arg.str.tmp");
    Builder.CreateCall(getStrcpy(), {stringDataPtr(Param.StorageTy, Temp), Text});
    return Temp;
  }

  Value *emitExprChild(const ASTNode &Wrapper) {
    if (Wrapper.Children.empty()) {
      error(Wrapper, "expression is missing");
      return nullptr;
    }
    return emitExpr(*Wrapper.Children.front());
  }

  Value *emitExpr(const ASTNode &Expr) {
    if (Expr.Kind == "Integer")
      return ConstantInt::get(i16Ty(), parseInteger(Expr.Text, 10), true);
    if (Expr.Kind == "HexInteger")
      return ConstantInt::get(
          i16Ty(), parseInteger(StringRef(Expr.Text).drop_front(), 16), true);
    if (Expr.Kind == "Real") {
      double Value = 0.0;
      StringRef(Expr.Text).getAsDouble(Value);
      return ConstantFP::get(doubleTy(), Value);
    }
    if (Expr.Kind == "String")
      return Builder.CreateGlobalString(unquote(Expr.Text));
    if (Expr.Kind == "Var") {
      if (Expr.Text == "TRUE")
        return ConstantInt::get(i16Ty(), 1);
      if (Expr.Text == "FALSE")
        return ConstantInt::get(i16Ty(), 0);
      if (Expr.Text == "PI")
        return ConstantFP::get(doubleTy(), 3.14159265358979323846);
      LocalInfo *Local = getOrCreateScalarLocal(Expr.Text);
      if (!Local)
        return nullptr;
      if (Local->IsArray) {
        error(Expr, "array value requires an index: " + Expr.Text);
        return nullptr;
      }
      if (Local->Kind == BasicKind::Record) {
        const ASTNode *Field = firstChildKind(Expr, "Field");
        if (!Field) {
          error(Expr, "record value requires a field: " + Expr.Text);
          return nullptr;
        }
        Value *Ptr = Builder.CreateInBoundsGEP(
            Local->StorageTy, Local->Slot,
            {ConstantInt::get(i32Ty(), 0),
             ConstantInt::get(i32Ty(), recordFieldIndex(Field->Text))});
        return Builder.CreateLoad(doubleTy(), Ptr, Expr.Text);
      }
      if (Local->Kind == BasicKind::String)
        return stringDataPtr(Local->ElementTy, Local->Slot);
      return Builder.CreateLoad(Local->ElementTy, Local->Slot, Expr.Text);
    }
    if (Expr.Kind == "Unary")
      return emitUnary(Expr);
    if (Expr.Kind == "Binary")
      return emitBinary(Expr);
    if (Expr.Kind == "Call")
      return emitCall(Expr);

    error(Expr,
          "expression is not supported by LLVM IR lowering yet: " + Expr.Kind);
    return nullptr;
  }

  Value *emitUnary(const ASTNode &Expr) {
    if (Expr.Children.empty()) {
      error(Expr, "unary expression is missing an operand");
      return nullptr;
    }
    Value *V = emitExpr(*Expr.Children.front());
    if (!V)
      return nullptr;
    if (Expr.Text == "+")
      return V;
    if (Expr.Text == "-") {
      if (V->getType()->isDoubleTy())
        return Builder.CreateFNeg(V);
      return Builder.CreateNeg(V);
    }
    if (Expr.Text == "NOT")
      return Builder.CreateZExt(Builder.CreateNot(toBool(V)), i16Ty());
    error(Expr, "unsupported unary operator: " + Expr.Text);
    return nullptr;
  }

  Value *emitBinary(const ASTNode &Expr) {
    if (Expr.Children.size() != 2) {
      error(Expr, "binary expression is missing an operand");
      return nullptr;
    }
    Value *LHS = emitExpr(*Expr.Children[0]);
    Value *RHS = emitExpr(*Expr.Children[1]);
    if (!LHS || !RHS)
      return nullptr;
    if (Expr.Text == "AND" || Expr.Text == "OR" || Expr.Text == "XOR") {
      Value *L = toBool(LHS);
      Value *R = toBool(RHS);
      Value *B = nullptr;
      if (Expr.Text == "AND")
        B = Builder.CreateAnd(L, R);
      else if (Expr.Text == "OR")
        B = Builder.CreateOr(L, R);
      else
        B = Builder.CreateXor(L, R);
      return Builder.CreateZExt(B, i16Ty());
    }
    if (LHS->getType()->isPointerTy() && RHS->getType()->isPointerTy()) {
      if (Expr.Text == "+")
        return emitStringConcat(LHS, RHS);
      if (Expr.Text == "=" || Expr.Text == "<>" || Expr.Text == "<" ||
          Expr.Text == "<=" || Expr.Text == ">" || Expr.Text == ">=") {
        Value *Cmp = Builder.CreateCall(getStrcmp(), {LHS, RHS});
        Value *Zero = ConstantInt::get(i32Ty(), 0);
        Value *IsEqual = nullptr;
        if (Expr.Text == "=")
          IsEqual = Builder.CreateICmpEQ(Cmp, Zero);
        else if (Expr.Text == "<>")
          IsEqual = Builder.CreateICmpNE(Cmp, Zero);
        else if (Expr.Text == "<")
          IsEqual = Builder.CreateICmpSLT(Cmp, Zero);
        else if (Expr.Text == "<=")
          IsEqual = Builder.CreateICmpSLE(Cmp, Zero);
        else if (Expr.Text == ">")
          IsEqual = Builder.CreateICmpSGT(Cmp, Zero);
        else
          IsEqual = Builder.CreateICmpSGE(Cmp, Zero);
        return Builder.CreateZExt(IsEqual, i16Ty());
      }
    }
    Type *OpTy = commonNumericType(LHS, RHS);
    LHS = coerceScalar(LHS, OpTy);
    RHS = coerceScalar(RHS, OpTy);

    if (Expr.Text == "+")
      return OpTy->isDoubleTy() ? Builder.CreateFAdd(LHS, RHS)
                                : Builder.CreateAdd(LHS, RHS);
    if (Expr.Text == "-")
      return OpTy->isDoubleTy() ? Builder.CreateFSub(LHS, RHS)
                                : Builder.CreateSub(LHS, RHS);
    if (Expr.Text == "*")
      return OpTy->isDoubleTy() ? Builder.CreateFMul(LHS, RHS)
                                : Builder.CreateMul(LHS, RHS);
    if (Expr.Text == "/")
      return OpTy->isDoubleTy() ? Builder.CreateFDiv(LHS, RHS)
                                : Builder.CreateSDiv(LHS, RHS);
    if (Expr.Text == "^")
      return emitPow(LHS, RHS);
    if (Expr.Text == "=")
      return compare(OpTy->isDoubleTy() ? CmpInst::FCMP_OEQ
                                        : CmpInst::ICMP_EQ,
                     LHS, RHS);
    if (Expr.Text == "<>")
      return compare(OpTy->isDoubleTy() ? CmpInst::FCMP_ONE
                                        : CmpInst::ICMP_NE,
                     LHS, RHS);
    if (Expr.Text == "<")
      return compare(OpTy->isDoubleTy() ? CmpInst::FCMP_OLT
                                        : CmpInst::ICMP_SLT,
                     LHS, RHS);
    if (Expr.Text == "<=")
      return compare(OpTy->isDoubleTy() ? CmpInst::FCMP_OLE
                                        : CmpInst::ICMP_SLE,
                     LHS, RHS);
    if (Expr.Text == ">")
      return compare(OpTy->isDoubleTy() ? CmpInst::FCMP_OGT
                                        : CmpInst::ICMP_SGT,
                     LHS, RHS);
    if (Expr.Text == ">=")
      return compare(OpTy->isDoubleTy() ? CmpInst::FCMP_OGE
                                        : CmpInst::ICMP_SGE,
                     LHS, RHS);
    error(Expr, "unsupported binary operator: " + Expr.Text);
    return nullptr;
  }

  Value *emitCall(const ASTNode &Expr) {
    auto Local = Locals.find(Expr.Text);
    if (Local != Locals.end() && Local->second.IsArray) {
      Value *Ptr = emitArrayElementPtr(Expr);
      if (!Ptr)
        return nullptr;
      if (Local->second.Kind == BasicKind::String)
        return stringDataPtr(Local->second.ElementTy, Ptr);
      if (Local->second.Kind == BasicKind::Record)
        return Builder.CreateLoad(doubleTy(), Ptr, Expr.Text);
      return Builder.CreateLoad(Local->second.ElementTy, Ptr, Expr.Text);
    }

    if (Expr.Text == "RND") {
      Value *R = Builder.CreateCall(getZeroDoubleFn("drand48"));
      if (Expr.Children.empty())
        return R;
      if (Expr.Children.size() != 1) {
        error(Expr, "RND expects zero or one argument");
        return nullptr;
      }
      Value *Limit = emitExpr(*Expr.Children.front());
      if (!Limit)
        return nullptr;
      return Builder.CreateFMul(R, coerceScalar(Limit, doubleTy()));
    }

    if (Expr.Text == "LEFT$")
      return emitLeft(Expr);
    if (Expr.Text == "RIGHT$")
      return emitRight(Expr);
    if (Expr.Text == "MID$")
      return emitMid(Expr);
    if (Expr.Text == "SUBSTR")
      return emitSubstr(Expr);
    if (Expr.Text == "LEN")
      return emitLen(Expr);
    if (Expr.Text == "STR$")
      return emitStr(Expr);
    if (Expr.Text == "CHR$")
      return emitChr(Expr);
    if (Expr.Text == "VAL")
      return emitVal(Expr);
    if (Expr.Text == "ASC")
      return emitAsc(Expr);
    if (Expr.Text == "MOD")
      return emitMod(Expr);
    if (Expr.Text == "LAND" || Expr.Text == "LOR" || Expr.Text == "LXOR")
      return emitBitwiseCall(Expr);

    if (Expr.Children.size() != 1) {
      error(Expr, "builtin call expects one argument: " + Expr.Text);
      return nullptr;
    }
    Value *Arg = emitExpr(*Expr.Children.front());
    if (!Arg)
      return nullptr;
    Arg = coerceScalar(Arg, doubleTy());

    if (Expr.Text == "INT")
      return Builder.CreateCall(getUnaryDoubleFn("floor"), {Arg});
    if (Expr.Text == "COS")
      return Builder.CreateCall(getUnaryDoubleFn("cos"), {Arg});
    if (Expr.Text == "SIN")
      return Builder.CreateCall(getUnaryDoubleFn("sin"), {Arg});
    if (Expr.Text == "ATN")
      return Builder.CreateCall(getUnaryDoubleFn("atan"), {Arg});
    if (Expr.Text == "ACS")
      return Builder.CreateCall(getUnaryDoubleFn("acos"), {Arg});
    if (Expr.Text == "ASN")
      return Builder.CreateCall(getUnaryDoubleFn("asin"), {Arg});
    if (Expr.Text == "SQR")
      return Builder.CreateCall(getUnaryDoubleFn("sqrt"), {Arg});
    if (Expr.Text == "FIX")
      return Builder.CreateFPToSI(Arg, i16Ty());
    if (Expr.Text == "ABS")
      return Builder.CreateCall(getUnaryDoubleFn("fabs"), {Arg});
    if (Expr.Text == "SQ")
      return Builder.CreateFMul(Arg, Arg);
    if (Expr.Text == "SGN")
      return emitSgn(Arg);
    if (Expr.Text == "EXP")
      return Builder.CreateCall(getUnaryDoubleFn("exp"), {Arg});
    if (Expr.Text == "LOG")
      return Builder.CreateCall(getUnaryDoubleFn("log"), {Arg});
    if (Expr.Text == "FLOAT")
      return Arg;

    error(Expr, "call is not supported by LLVM IR lowering yet: " + Expr.Text);
    return nullptr;
  }

  Value *emitLeft(const ASTNode &Expr) {
    if (Expr.Children.size() != 2) {
      error(Expr, "LEFT$ expects two arguments");
      return nullptr;
    }
    Value *Source = emitExpr(*Expr.Children[0]);
    Value *Count = emitExpr(*Expr.Children[1]);
    if (!Source || !Count)
      return nullptr;
    return emitStringSlice(Source, ConstantInt::get(i32Ty(), 0), Count);
  }

  Value *emitRight(const ASTNode &Expr) {
    if (Expr.Children.size() != 2) {
      error(Expr, "RIGHT$ expects two arguments");
      return nullptr;
    }
    Value *Source = emitExpr(*Expr.Children[0]);
    Value *Count = emitExpr(*Expr.Children[1]);
    if (!Source || !Count)
      return nullptr;
    Count = coerceScalar(Count, i32Ty());
    Value *Length = Builder.CreateTrunc(Builder.CreateCall(getStrlen(), {Source}),
                                        i32Ty());
    Value *Start = Builder.CreateSub(Length, Count);
    Start = Builder.CreateSelect(Builder.CreateICmpSLT(Start,
                                                       ConstantInt::get(i32Ty(), 0)),
                                 ConstantInt::get(i32Ty(), 0), Start);
    return emitStringSlice(Source, Start, Count);
  }

  Value *emitMid(const ASTNode &Expr) {
    if (Expr.Children.size() != 2 && Expr.Children.size() != 3) {
      error(Expr, "MID$ expects two or three arguments");
      return nullptr;
    }
    Value *Source = emitExpr(*Expr.Children[0]);
    Value *Start = emitExpr(*Expr.Children[1]);
    Value *Count = Expr.Children.size() == 3 ? emitExpr(*Expr.Children[2])
                                             : ConstantInt::get(i32Ty(), 255);
    if (!Source || !Start || !Count)
      return nullptr;
    Start = Builder.CreateSub(coerceScalar(Start, i32Ty()),
                              ConstantInt::get(i32Ty(), 1));
    Start = Builder.CreateSelect(Builder.CreateICmpSLT(Start,
                                                       ConstantInt::get(i32Ty(), 0)),
                                 ConstantInt::get(i32Ty(), 0), Start);
    return emitStringSlice(Source, Start, Count);
  }

  Value *emitSubstr(const ASTNode &Expr) {
    if (Expr.Children.size() != 2) {
      error(Expr, "SUBSTR expects two arguments");
      return nullptr;
    }
    Value *Needle = emitExpr(*Expr.Children[0]);
    Value *Haystack = emitExpr(*Expr.Children[1]);
    if (!Needle || !Haystack)
      return nullptr;
    Value *Found = Builder.CreateCall(getStrstr(), {Haystack, Needle});
    Value *FoundInt = Builder.CreatePtrToInt(Found, i64Ty());
    Value *HaystackInt = Builder.CreatePtrToInt(Haystack, i64Ty());
    Value *Position = Builder.CreateAdd(Builder.CreateSub(FoundInt, HaystackInt),
                                        ConstantInt::get(i64Ty(), 1));
    Value *IsNull = Builder.CreateICmpEQ(
        Found, ConstantPointerNull::get(PointerType::getUnqual(Context)));
    return Builder.CreateTrunc(
        Builder.CreateSelect(IsNull, ConstantInt::get(i64Ty(), 0), Position),
        i16Ty());
  }

  Value *emitLen(const ASTNode &Expr) {
    if (Expr.Children.size() != 1) {
      error(Expr, "LEN expects one argument");
      return nullptr;
    }
    Value *Source = emitExpr(*Expr.Children.front());
    if (!Source)
      return nullptr;
    return Builder.CreateTrunc(Builder.CreateCall(getStrlen(), {Source}),
                               i16Ty());
  }

  Value *emitStr(const ASTNode &Expr) {
    if (Expr.Children.size() != 1) {
      error(Expr, "STR$ expects one argument");
      return nullptr;
    }
    Value *Number = emitExpr(*Expr.Children.front());
    if (!Number)
      return nullptr;
    Number = coerceScalar(Number, doubleTy());
    Value *Dest = createTempString();
    Builder.CreateCall(getSnprintf(),
                       {Dest, ConstantInt::get(i64Ty(), 256),
                        Builder.CreateGlobalString("%g"), Number});
    return Dest;
  }

  Value *emitVal(const ASTNode &Expr) {
    if (Expr.Children.size() != 1) {
      error(Expr, "VAL expects one argument");
      return nullptr;
    }
    Value *Source = emitExpr(*Expr.Children.front());
    if (!Source)
      return nullptr;
    return Builder.CreateCall(getStrtod(),
                              {Source, ConstantPointerNull::get(
                                           PointerType::getUnqual(Context))});
  }

  Value *emitAsc(const ASTNode &Expr) {
    if (Expr.Children.size() != 1) {
      error(Expr, "ASC expects one argument");
      return nullptr;
    }
    Value *Source = emitExpr(*Expr.Children.front());
    if (!Source)
      return nullptr;
    Value *Ch = Builder.CreateLoad(Type::getInt8Ty(Context), Source);
    return Builder.CreateZExt(Ch, i16Ty());
  }

  Value *emitChr(const ASTNode &Expr) {
    if (Expr.Children.size() != 1) {
      error(Expr, "CHR$ expects one argument");
      return nullptr;
    }
    Value *Code = emitExpr(*Expr.Children.front());
    if (!Code)
      return nullptr;
    Code = coerceScalar(Code, i32Ty());
    Value *Dest = createTempString();
    Builder.CreateStore(Builder.CreateTrunc(Code, Type::getInt8Ty(Context)),
                        Dest);
    Builder.CreateStore(
        ConstantInt::get(Type::getInt8Ty(Context), 0),
        Builder.CreateInBoundsGEP(Type::getInt8Ty(Context), Dest,
                                  ConstantInt::get(i32Ty(), 1)));
    return Dest;
  }

  Value *emitMod(const ASTNode &Expr) {
    if (Expr.Children.size() != 2) {
      error(Expr, "MOD expects two arguments");
      return nullptr;
    }
    Value *LHS = emitExpr(*Expr.Children[0]);
    Value *RHS = emitExpr(*Expr.Children[1]);
    if (!LHS || !RHS)
      return nullptr;
    LHS = coerceScalar(LHS, i16Ty());
    RHS = coerceScalar(RHS, i16Ty());
    return Builder.CreateSRem(LHS, RHS);
  }

  Value *emitBitwiseCall(const ASTNode &Expr) {
    if (Expr.Children.size() != 2) {
      error(Expr, Expr.Text + " expects two arguments");
      return nullptr;
    }
    Value *LHS = emitExpr(*Expr.Children[0]);
    Value *RHS = emitExpr(*Expr.Children[1]);
    if (!LHS || !RHS)
      return nullptr;
    LHS = coerceScalar(LHS, i16Ty());
    RHS = coerceScalar(RHS, i16Ty());
    if (Expr.Text == "LAND")
      return Builder.CreateAnd(LHS, RHS);
    if (Expr.Text == "LOR")
      return Builder.CreateOr(LHS, RHS);
    return Builder.CreateXor(LHS, RHS);
  }

  Value *emitSgn(Value *Arg) {
    Value *Zero = ConstantFP::get(doubleTy(), 0.0);
    Value *Positive = Builder.CreateFCmpOGT(Arg, Zero);
    Value *Negative = Builder.CreateFCmpOLT(Arg, Zero);
    return Builder.CreateSelect(
        Positive, ConstantInt::get(i16Ty(), 1),
        Builder.CreateSelect(Negative, ConstantInt::getSigned(i16Ty(), -1),
                             ConstantInt::get(i16Ty(), 0)));
  }

  Value *emitArrayElementPtr(const ASTNode &Call) {
    auto It = Locals.find(Call.Text);
    if (It == Locals.end() || !It->second.IsArray) {
      error(Call, "unknown array: " + Call.Text);
      return nullptr;
    }
    std::vector<Value *> Indices;
    Indices.push_back(ConstantInt::get(i32Ty(), 0));
    for (const std::unique_ptr<ASTNode> &IndexExpr : Call.Children) {
      if (IndexExpr->Kind == "Field")
        continue;
      Value *Index = emitExpr(*IndexExpr);
      if (!Index)
        return nullptr;
      Indices.push_back(coerceScalar(Index, i32Ty()));
    }
    if (It->second.Kind == BasicKind::Record) {
      const ASTNode *Field = firstChildKind(Call, "Field");
      if (!Field) {
        error(Call, "record array reference requires a field: " + Call.Text);
        return nullptr;
      }
      Indices.push_back(ConstantInt::get(i32Ty(), recordFieldIndex(Field->Text)));
    }
    return Builder.CreateInBoundsGEP(It->second.StorageTy, It->second.Slot,
                                     Indices, Call.Text);
  }

  Value *emitArrayElementPtr(StringRef Designator, const ASTNode &At) {
    StringRef Name = arrayDesignatorName(Designator);
    auto It = Locals.find(Name);
    if (It == Locals.end() || !It->second.IsArray) {
      error(At, ("unknown array: " + Name).str());
      return nullptr;
    }
    std::vector<Value *> Indices;
    Indices.push_back(ConstantInt::get(i32Ty(), 0));
    for (std::string IndexText : arrayDesignatorIndices(Designator)) {
      Value *Index = emitSimpleIndex(IndexText, At);
      if (!Index)
        return nullptr;
      Indices.push_back(Index);
    }
    if (It->second.Kind == BasicKind::Record)
      Indices.push_back(
          ConstantInt::get(i32Ty(), recordFieldIndex(recordFieldName(Designator))));
    return Builder.CreateInBoundsGEP(It->second.StorageTy, It->second.Slot,
                                     Indices, Name);
  }

  Value *stringDataPtr(Type *StringTy, Value *Ptr) {
    return Builder.CreateInBoundsGEP(
        StringTy, Ptr,
        {ConstantInt::get(i32Ty(), 0), ConstantInt::get(i32Ty(), 0)});
  }

  Value *createTempString() {
    AllocaInst *Temp =
        Builder.CreateAlloca(ArrayType::get(Type::getInt8Ty(Context), 256));
    return stringDataPtr(Temp->getAllocatedType(), Temp);
  }

  Value *emitStringSlice(Value *Source, Value *Start, Value *Count) {
    Value *Dest = createTempString();
    Start = coerceScalar(Start, i32Ty());
    Count = coerceScalar(Count, i32Ty());
    Value *SourceStart =
        Builder.CreateInBoundsGEP(Type::getInt8Ty(Context), Source, Start);
    Value *Count64 = Builder.CreateSExtOrTrunc(Count, i64Ty());
    Builder.CreateCall(getStrncpy(), {Dest, SourceStart, Count64});
    Builder.CreateStore(ConstantInt::get(Type::getInt8Ty(Context), 0),
                        Builder.CreateInBoundsGEP(Type::getInt8Ty(Context), Dest,
                                                  Count));
    return Dest;
  }

  Value *emitStringConcat(Value *LHS, Value *RHS) {
    Value *Dest = createTempString();
    Builder.CreateCall(getStrcpy(), {Dest, LHS});
    Builder.CreateCall(getStrcat(), {Dest, RHS});
    return Dest;
  }

  void emitStringStore(Type *StringTy, Value *Dest, StringRef Text) {
    Builder.CreateCall(getStrcpy(),
                       {stringDataPtr(StringTy, Dest),
                        Builder.CreateGlobalString(Text)});
  }

  Value *emitSimpleIndex(StringRef Text, const ASTNode &At) {
    Text = Text.trim();
    if (Text.empty()) {
      error(At, "array index is empty");
      return nullptr;
    }
    uint64_t IntValue = 0;
    if (!Text.getAsInteger(10, IntValue))
      return ConstantInt::get(i32Ty(), IntValue);
    if (Text.consume_front("$") && !Text.getAsInteger(16, IntValue))
      return ConstantInt::get(i32Ty(), IntValue);
    LocalInfo *Local = getOrCreateScalarLocal(Text);
    if (!Local)
      return nullptr;
    if (Local->IsArray) {
      error(At, ("array index cannot be an array: " + Text).str());
      return nullptr;
    }
    return coerceScalar(Builder.CreateLoad(Local->ElementTy, Local->Slot, Text),
                        i32Ty());
  }

  static StringRef arrayDesignatorName(StringRef Text) {
    return Text.take_until([](char C) { return C == '('; }).trim();
  }

  static std::vector<std::string> arrayDesignatorIndices(StringRef Text) {
    std::vector<std::string> Result;
    size_t Open = Text.find('(');
    size_t Close = Text.rfind(')');
    if (Open == StringRef::npos || Close == StringRef::npos || Close <= Open)
      return Result;
    StringRef Body = Text.slice(Open + 1, Close);
    while (!Body.empty()) {
      StringRef Part;
      std::tie(Part, Body) = Body.split(',');
      Result.push_back(Part.trim().str());
    }
    return Result;
  }

  static StringRef recordFieldName(StringRef Text) {
    size_t Dot = Text.rfind('.');
    if (Dot == StringRef::npos)
      return StringRef();
    return Text.drop_front(Dot + 1).trim();
  }

  static unsigned recordFieldIndex(StringRef Field) {
    return StringSwitch<unsigned>(Field.upper())
        .Case("SECTX", 0)
        .Case("SECTY", 1)
        .Case("ENERGY", 2)
        .Case("LEFT", 0)
        .Case("RIGHT", 1)
        .Default(0);
  }

  Value *emitPow(Value *LHS, Value *RHS) {
    LHS = coerceScalar(LHS, doubleTy());
    RHS = coerceScalar(RHS, doubleTy());
    return Builder.CreateCall(getBinaryDoubleFn("pow"), {LHS, RHS});
  }

  Value *compare(CmpInst::Predicate Pred, Value *LHS, Value *RHS) {
    Value *Cmp = CmpInst::isFPPredicate(Pred)
                     ? Builder.CreateFCmp(Pred, LHS, RHS)
                     : Builder.CreateICmp(Pred, LHS, RHS);
    return Builder.CreateZExt(Cmp, i16Ty());
  }

  Value *coerceInteger(Value *V, Type *Ty) {
    if (V->getType() == Ty)
      return V;
    return Builder.CreateSExtOrTrunc(V, Ty);
  }

  Value *coerceScalar(Value *V, Type *Ty) {
    if (V->getType() == Ty)
      return V;
    if (Ty->isDoubleTy() && V->getType()->isIntegerTy())
      return Builder.CreateSIToFP(V, Ty);
    if (Ty->isIntegerTy() && V->getType()->isDoubleTy())
      return Builder.CreateFPToSI(V, Ty);
    if (Ty->isIntegerTy() && V->getType()->isIntegerTy())
      return Builder.CreateSExtOrTrunc(V, Ty);
    return Constant::getNullValue(Ty);
  }

  Type *commonNumericType(Value *LHS, Value *RHS) {
    if (LHS->getType()->isDoubleTy() || RHS->getType()->isDoubleTy())
      return doubleTy();
    return i16Ty();
  }

  Constant *numericConstant(Type *Ty, int64_t Value) {
    if (Ty->isDoubleTy())
      return ConstantFP::get(Ty, static_cast<double>(Value));
    return ConstantInt::get(Ty, Value, true);
  }

  FunctionCallee getZeroDoubleFn(StringRef Name) {
    FunctionType *FT = FunctionType::get(doubleTy(), false);
    return M->getOrInsertFunction(Name, FT);
  }

  FunctionCallee getUnaryDoubleFn(StringRef Name) {
    FunctionType *FT = FunctionType::get(doubleTy(), {doubleTy()}, false);
    return M->getOrInsertFunction(Name, FT);
  }

  FunctionCallee getBinaryDoubleFn(StringRef Name) {
    FunctionType *FT =
        FunctionType::get(doubleTy(), {doubleTy(), doubleTy()}, false);
    return M->getOrInsertFunction(Name, FT);
  }

  FunctionCallee getStrcpy() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(PtrTy, {PtrTy, PtrTy}, false);
    return M->getOrInsertFunction("strcpy", FT);
  }

  FunctionCallee getStrncpy() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(PtrTy, {PtrTy, PtrTy, i64Ty()}, false);
    return M->getOrInsertFunction("strncpy", FT);
  }

  FunctionCallee getStrcat() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(PtrTy, {PtrTy, PtrTy}, false);
    return M->getOrInsertFunction("strcat", FT);
  }

  FunctionCallee getStrstr() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(PtrTy, {PtrTy, PtrTy}, false);
    return M->getOrInsertFunction("strstr", FT);
  }

  FunctionCallee getStrlen() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(i64Ty(), {PtrTy}, false);
    return M->getOrInsertFunction("strlen", FT);
  }

  FunctionCallee getStrcmp() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(i32Ty(), {PtrTy, PtrTy}, false);
    return M->getOrInsertFunction("strcmp", FT);
  }

  FunctionCallee getSnprintf() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(i32Ty(), {PtrTy, i64Ty(), PtrTy}, true);
    return M->getOrInsertFunction("snprintf", FT);
  }

  FunctionCallee getStrtod() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(doubleTy(), {PtrTy, PtrTy}, false);
    return M->getOrInsertFunction("strtod", FT);
  }

  bool getBasicType(const ASTNode &Decl, StringRef BasicType, BasicKind &Kind,
                    Type *&IRTy, uint64_t &StringLength) {
    if (BasicType == "BYTE") {
      Kind = BasicKind::Byte;
      IRTy = Type::getInt8Ty(Context);
      return true;
    }
    if (BasicType == "INTEGER") {
      Kind = BasicKind::Integer;
      IRTy = i16Ty();
      return true;
    }
    if (BasicType == "BOOLEAN") {
      Kind = BasicKind::Integer;
      IRTy = i16Ty();
      return true;
    }
    if (BasicType == "REAL") {
      Kind = BasicKind::Real;
      IRTy = doubleTy();
      return true;
    }
    if (BasicType == "STRING") {
      Kind = BasicKind::String;
      StringLength = stringLength(Decl);
      IRTy = ArrayType::get(Type::getInt8Ty(Context), StringLength + 1);
      return true;
    }
    Kind = BasicKind::Record;
    IRTy = ArrayType::get(doubleTy(), 3);
    return true;
  }

  LocalInfo *getOrCreateScalarLocal(StringRef Name) {
    auto It = Locals.find(Name);
    if (It != Locals.end())
      return &It->second;

    BasicKind Kind = Name.ends_with("$") ? BasicKind::String : BasicKind::Real;
    Type *ElementTy =
        Kind == BasicKind::String
            ? static_cast<Type *>(ArrayType::get(Type::getInt8Ty(Context), 256))
            : doubleTy();
    AllocaInst *Slot = createEntryAlloca(ElementTy, Name);
    Builder.CreateStore(Constant::getNullValue(ElementTy), Slot);
    auto Inserted =
        Locals.insert({Name, {Slot, ElementTy, ElementTy, Kind, false,
                              Kind == BasicKind::String ? 255u : 0u}});
    return &Inserted.first->second;
  }

  AllocaInst *createEntryAlloca(Type *Ty, StringRef Name) {
    IRBuilder<> EntryBuilder(&CurrentFunction->getEntryBlock(),
                             CurrentFunction->getEntryBlock().begin());
    return EntryBuilder.CreateAlloca(Ty, nullptr, Name);
  }

  uint64_t stringLength(const ASTNode &Decl) {
    if (const ASTNode *TypeName = firstChildKind(Decl, "TypeName"))
      if (const ASTNode *Length = firstChildKind(*TypeName, "StringLength"))
        return constantExtent(*Length);
    return 255;
  }

  uint64_t constantExtent(const ASTNode &Node) {
    if (Node.Children.empty())
      return 0;
    const ASTNode &Expr = *Node.Children.front();
    if (Expr.Kind == "Integer")
      return parseInteger(Expr.Text, 10);
    if (Expr.Kind == "HexInteger")
      return parseInteger(StringRef(Expr.Text).drop_front(), 16);
    return 0;
  }

  Value *toBool(Value *V) {
    if (V->getType()->isIntegerTy(1))
      return V;
    if (V->getType()->isDoubleTy())
      return Builder.CreateFCmpONE(V, ConstantFP::get(V->getType(), 0.0));
    return Builder.CreateICmpNE(V, ConstantInt::get(V->getType(), 0));
  }

  FunctionCallee getPrintf() {
    FunctionType *FT =
        FunctionType::get(i32Ty(), PointerType::getUnqual(Context), true);
    return M->getOrInsertFunction("printf", FT);
  }

  FunctionCallee getFflush() {
    FunctionType *FT =
        FunctionType::get(i32Ty(), PointerType::getUnqual(Context), false);
    return M->getOrInsertFunction("fflush", FT);
  }

  FunctionCallee getScanf() {
    FunctionType *FT =
        FunctionType::get(i32Ty(), PointerType::getUnqual(Context), true);
    return M->getOrInsertFunction("scanf", FT);
  }

  static int64_t parseInteger(StringRef Text, unsigned Radix) {
    uint64_t Value = 0;
    Text.getAsInteger(Radix, Value);
    return static_cast<int64_t>(Value);
  }

  static std::string unquote(StringRef Text) {
    if (Text.size() >= 2 && Text.front() == '"' && Text.back() == '"')
      return Text.drop_front().drop_back().str();
    return Text.str();
  }

  static std::string normalizedLabel(StringRef Text) {
    Text = Text.trim();
    if (Text.ends_with(":"))
      Text = Text.drop_back();
    return Text.upper();
  }

  static std::string printUsingLabel(StringRef Text) {
    size_t Begin = Text.find('\'');
    if (Begin == StringRef::npos)
      return std::string();
    size_t End = Text.find('\'', Begin + 1);
    if (End == StringRef::npos || End <= Begin + 1)
      return std::string();
    return Text.slice(Begin + 1, End).str();
  }

  bool constantDataNumber(const ASTNode &Node, double &Result) {
    if (Node.Kind == "Value") {
      if (Node.Children.empty())
        return false;
      return constantDataNumber(*Node.Children.front(), Result);
    }
    if (Node.Kind == "Integer") {
      Result = static_cast<double>(parseInteger(Node.Text, 10));
      return true;
    }
    if (Node.Kind == "HexInteger") {
      Result =
          static_cast<double>(parseInteger(StringRef(Node.Text).drop_front(), 16));
      return true;
    }
    if (Node.Kind == "Real")
      return !StringRef(Node.Text).getAsDouble(Result);
    if (Node.Kind == "Unary" && Node.Text == "-" && Node.Children.size() == 1) {
      if (!constantDataNumber(*Node.Children.front(), Result))
        return false;
      Result = -Result;
      return true;
    }
    return false;
  }
};

} // namespace

bool llvm::basic09::emitLLVMIR(const ASTNode &Root, StringRef ModuleName,
                               StringRef Triple, raw_ostream &OS,
                               raw_ostream &Errs) {
  return IREmitter(ModuleName, Triple, Errs).emit(Root, OS);
}
