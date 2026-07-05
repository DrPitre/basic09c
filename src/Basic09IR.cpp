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
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
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
  std::string RecordTypeName;
};

struct ParamInfo {
  std::string Name;
  Type *StorageTy = nullptr;
  Type *ElementTy = nullptr;
  BasicKind Kind = BasicKind::Integer;
  uint64_t StringLength = 0;
  std::string RecordTypeName;
};

struct ProcedureInfo {
  Function *Fn = nullptr;
  std::vector<ParamInfo> Params;
};

struct RecordFieldInfo {
  std::string Name;
  BasicKind Kind = BasicKind::Integer;
  Type *StorageTy = nullptr;
  Type *ElementTy = nullptr;
  uint64_t StringLength = 0;
  bool IsArray = false;
  std::string RecordTypeName;
};

struct RecordTypeInfo {
  std::vector<RecordFieldInfo> Fields;
  StructType *IRTy = nullptr;
  StringMap<unsigned> FieldIndex;
};

struct DesignatorState {
  Value *Ptr = nullptr;
  Type *ElemTy = nullptr;
  Type *StorageTy = nullptr;
  BasicKind Kind = BasicKind::Integer;
  bool IsArray = false;
  std::string RecordTypeName;
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

    for (const std::unique_ptr<ASTNode> &Child : Root.Children) {
      if (Child->Kind != "Type")
        continue;
      const ASTNode *TypeName = firstChildKind(*Child, "TypeName");
      if (!TypeName)
        continue;
      RecordTypes = GlobalRecordTypes;
      if (!buildRecordType(*Child, TypeName->Text))
        return false;
      GlobalRecordTypes = RecordTypes;
    }

    std::vector<const ASTNode *> ProceduresToEmit;
    std::vector<const ASTNode *> TopLevelStatements;
    for (const std::unique_ptr<ASTNode> &Child : Root.Children) {
      if (Child->Kind == "Procedure") {
        if (!declareProcedure(*Child))
          return false;
        ProceduresToEmit.push_back(Child.get());
        continue;
      }
      if (Child->Kind == "Type")
        continue;
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
  StringMap<RecordTypeInfo> RecordTypes;
  StringMap<RecordTypeInfo> GlobalRecordTypes;
  StringMap<BasicBlock *> LabelBlocks;
  std::vector<BasicBlock *> LoopExits;
  std::vector<double> DataValues;
  std::vector<BasicBlock *> GosubReturnBlocks;
  std::vector<SwitchInst *> ReturnSwitches;
  AllocaInst *GosubStack = nullptr;
  AllocaInst *GosubSP = nullptr;
  size_t DataCursor = 0;
  BasicBlock *ErrorTrapBlock = nullptr;
  bool DegreesMode = false;

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
    if (!collectRecordTypes(Procedure))
      return false;
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
    if (!collectRecordTypes(Procedure))
      return false;

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
    ErrorTrapBlock = nullptr;
    initializeGosubStack();

    unsigned Index = 0;
    for (Argument &Arg : CurrentFunction->args()) {
      const ParamInfo &Param = Proc.Params[Index++];
      Arg.setName(Param.Name);
      Locals[Param.Name] = {&Arg,          Param.StorageTy,
                            Param.ElementTy, Param.Kind,
                            false,         Param.StringLength,
                            Param.RecordTypeName};
    }

    const ASTNode *Body = firstChildKind(Procedure, "Block");
    if (Body)
      collectLabels(*Body);
    if (Body && !emitBlock(*Body))
      return false;

    if (Builder.GetInsertBlock()->getTerminator() == nullptr)
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
    ErrorTrapBlock = nullptr;
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
                          Param.StringLength, Param.RecordTypeName))
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
    ErrorTrapBlock = nullptr;
    initializeGosubStack();

    for (const ASTNode *Stmt : Statements)
      if (!emitStatement(*Stmt))
        return false;

    if (Builder.GetInsertBlock()->getTerminator() == nullptr)
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
    ErrorTrapBlock = nullptr;
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
    if (Stmt.Kind == "Shell")
      return emitShell(Stmt);
    if (Stmt.Kind == "Open")
      return emitOpen(Stmt);
    if (Stmt.Kind == "Create")
      return emitCreate(Stmt);
    if (Stmt.Kind == "Close")
      return emitClose(Stmt);
    if (Stmt.Kind == "Delete")
      return emitDelete(Stmt);
    if (Stmt.Kind == "Seek")
      return emitSeek(Stmt);
    if (Stmt.Kind == "GetFile")
      return emitGetFile(Stmt);
    if (Stmt.Kind == "PutFile")
      return emitPutFile(Stmt);
    if (Stmt.Kind == "ReadFile")
      return emitReadFile(Stmt);
    if (Stmt.Kind == "WriteFile")
      return emitWriteFile(Stmt);
    if (Stmt.Kind == "Label")
      return emitLabel(Stmt);
    if (Stmt.Kind == "BranchTarget")
      return emitBranchTarget(Stmt);
    if (Stmt.Kind == "Goto")
      return emitBranchStatement(Stmt);
    if (Stmt.Kind == "Gosub")
      return emitGosub(Stmt);
    if (Stmt.Kind == "OnErrorGoto")
      return emitOnErrorGoto(Stmt);
    if (Stmt.Kind == "OnError")
      return emitOnErrorClear(Stmt);
    if (Stmt.Kind == "Bye")
      return emitBye(Stmt);
    if (Stmt.Kind == "Deg")
      return emitDeg(Stmt);
    if (Stmt.Kind == "Rad")
      return emitRad(Stmt);
    if (Stmt.Kind == "Base")
      return emitBase(Stmt);
    if (Stmt.Kind == "Chd" || Stmt.Kind == "Chx")
      return emitChangeDir(Stmt);
    if (Stmt.Kind == "OnGoto" || Stmt.Kind == "OnGosub")
      return emitComputedBranch(Stmt);
    if (Stmt.Kind == "Type" || Stmt.Kind == "Param")
      return true;
    if (Stmt.Kind == "EndIf")
      return true;
    if (Stmt.Kind == "Return")
      return emitReturn(Stmt);
    if (Stmt.Kind == "End")
      return emitEnd(Stmt);
    if (Stmt.Kind == "Stop") {
      if (Builder.GetInsertBlock()->getTerminator() == nullptr)
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
      std::string RecordTypeName;
      if (!getBasicType(*Decl, BasicType, Kind, ElementTy, StringLength,
                        RecordTypeName))
        return false;

      Type *StorageTy = ElementTy;
      std::vector<uint64_t> Bounds;
      for (const std::unique_ptr<ASTNode> &Child : Decl->Children)
        if (Child->Kind == "Bound")
          Bounds.push_back(constantExtent(*Child));
      for (uint64_t Bound : llvm::reverse(Bounds))
        StorageTy = ArrayType::get(StorageTy, Bound + 1);

      AllocaInst *StackSlot = nullptr;
      Value *Slot = nullptr;
      if (isa<ArrayType>(StorageTy)) {
        uint64_t Size = M->getDataLayout().getTypeAllocSize(StorageTy);
        if (Size > 65536) {
          // Heap-allocate large arrays to avoid stack overflow.
          Slot = Builder.CreateCall(getCalloc(),
                                    {ConstantInt::get(i64Ty(), 1),
                                     ConstantInt::get(i64Ty(), Size)});
        } else {
          StackSlot = Builder.CreateAlloca(StorageTy, nullptr, Decl->Text);
          Builder.CreateMemSet(StackSlot, Builder.getInt8(0),
                               Builder.getInt64(Size), StackSlot->getAlign());
          Slot = StackSlot;
        }
      } else {
        StackSlot = Builder.CreateAlloca(StorageTy, nullptr, Decl->Text);
        Builder.CreateStore(Constant::getNullValue(StorageTy), StackSlot);
        Slot = StackSlot;
      }
      Locals[Decl->Text] = {Slot,   StorageTy,      ElementTy,
                            Kind,   !Bounds.empty(), StringLength,
                            RecordTypeName};
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
    if (Builder.GetInsertBlock()->getTerminator() == nullptr)
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

  bool emitOnErrorGoto(const ASTNode &Stmt) {
    const ASTNode *Target = firstChildKind(Stmt, "BranchTarget");
    if (!Target)
      return error(Stmt, "ON ERROR GOTO is missing a target");
    BasicBlock *TargetBB = lookupLabel(*Target);
    if (!TargetBB)
      return false;
    ErrorTrapBlock = TargetBB;
    return true;
  }

  bool emitOnErrorClear(const ASTNode &Stmt) {
    ErrorTrapBlock = nullptr;
    return true;
  }

  // If an ON ERROR GOTO trap is armed, branches to the handler when Failed is
  // true; otherwise falls through. No-op when no trap is armed, since the
  // underlying runtime call already degrades gracefully (matches the classic
  // OS-9 BASIC09 catch-and-continue semantics from ON ERROR GOTO/ON ERROR).
  void emitErrorTrapCheck(Value *Failed) {
    if (!ErrorTrapBlock)
      return;
    BasicBlock *Continue =
        BasicBlock::Create(Context, "noerr", CurrentFunction);
    Builder.CreateCondBr(Failed, ErrorTrapBlock, Continue);
    Builder.SetInsertPoint(Continue);
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
    if (Builder.GetInsertBlock()->getTerminator() == nullptr)
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
      if (!emitStoreDesignator(*Target,
                               ConstantFP::get(doubleTy(),
                                               DataValues[DataCursor++])))
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
      if (!emitScanDesignator(*Target))
        return false;
    }
    return true;
  }

  bool emitAssign(const ASTNode &Stmt) {
    const ASTNode *LHS = firstChildKind(Stmt, "LHS");
    const ASTNode *RHS = firstChildKind(Stmt, "RHS");
    if (!LHS || !RHS)
      return error(Stmt, "assignment is missing an operand");
    if (LHS->Children.empty())
      return error(*LHS, "assignment target is missing: " + LHS->Text);
    const ASTNode &Root = *LHS->Children.front();
    DesignatorState State;
    if (!resolveOrCreateDesignator(Root, State))
      return false;
    if (State.IsArray || State.Kind == BasicKind::Record)
      return emitAggregateAssign(*LHS, State, *RHS);
    if (State.Kind == BasicKind::String) {
      const ASTNode *String = firstChildKind(*RHS, "String");
      if (String) {
        emitStringStore(State.ElemTy, State.Ptr, unquote(String->Text));
        return true;
      }
      Value *Text = emitExprChild(*RHS);
      if (!Text)
        return false;
      if (!Text->getType()->isPointerTy())
        return error(*RHS, "STRING assignment requires a string expression");
      Builder.CreateCall(getStrcpy(),
                         {stringDataPtr(State.ElemTy, State.Ptr), Text});
      return true;
    }
    Value *V = emitExprChild(*RHS);
    if (!V)
      return false;
    V = coerceScalar(V, State.ElemTy);
    Builder.CreateStore(V, State.Ptr);
    return true;
  }

  // Whole-array or whole-record assignment (e.g. `b := a`, `w.samples :=
  // v.samples`). The right-hand side must itself be a bare designator of the
  // exact same aggregate type; it is copied with a single memcpy rather than
  // field-by-field.
  bool emitAggregateAssign(const ASTNode &LHS, const DesignatorState &Dest,
                           const ASTNode &RHS) {
    const char *Noun = Dest.IsArray ? "array" : "record";
    if (RHS.Children.empty())
      return error(RHS, "assignment is missing an operand");
    const ASTNode &Src = *RHS.Children.front();
    if (Src.Kind != "Var" && Src.Kind != "Call")
      return error(Src, std::string(Noun) +
                            " assignment requires a variable on the "
                            "right-hand side: " +
                            LHS.Text);
    DesignatorState SrcState;
    if (!resolveDesignator(Src, SrcState))
      return false;
    if (SrcState.IsArray != Dest.IsArray || SrcState.Kind != Dest.Kind ||
        SrcState.StorageTy != Dest.StorageTy)
      return error(Src, "type mismatch in " + std::string(Noun) +
                            " assignment: " + LHS.Text + " := " + Src.Text);
    uint64_t Size = M->getDataLayout().getTypeAllocSize(Dest.StorageTy);
    Align Alignment = M->getDataLayout().getABITypeAlign(Dest.StorageTy);
    Builder.CreateMemCpy(Dest.Ptr, Alignment, SrcState.Ptr, Alignment, Size);
    return true;
  }


  bool emitShell(const ASTNode &Stmt) {
    const ASTNode *Argument = firstChildKind(Stmt, "Argument");
    if (!Argument)
      return error(Stmt, "SHELL expects a command string");
    Value *Command = emitExprChild(*Argument);
    if (!Command)
      return false;
    if (!Command->getType()->isPointerTy())
      return error(*Argument, "SHELL expects a string command");
    Value *Status = Builder.CreateCall(getSystem(), {Command});
    Value *Failed = Builder.CreateICmpNE(Status, ConstantInt::get(i32Ty(), 0));
    emitErrorTrapCheck(Failed);
    return true;
  }

  Value *emitPathHandle(const ASTNode &PathNode) {
    Value *V = emitExprChild(PathNode);
    if (!V)
      return nullptr;
    return coerceScalar(V, i32Ty());
  }

  static std::string fileOpenMode(bool ForCreate, StringRef ModeText) {
    std::string Mode = ModeText.upper();
    if (ForCreate)
      return Mode == "UPDATE" ? "w+b" : "wb";
    return (Mode == "WRITE" || Mode == "UPDATE") ? "r+b" : "rb";
  }

  bool emitOpenOrCreate(const ASTNode &Stmt, bool ForCreate) {
    const ASTNode *PathNode = firstChildKind(Stmt, "Path");
    const ASTNode *FileName = firstChildKind(Stmt, "FileName");
    const ASTNode *Mode = firstChildKind(Stmt, "Mode");
    if (!PathNode || !FileName)
      return error(Stmt, Stmt.Kind + " requires a path and file name");
    Value *NameVal = emitExprChild(*FileName);
    if (!NameVal)
      return false;
    if (!NameVal->getType()->isPointerTy())
      return error(*FileName, Stmt.Kind + " expects a string file name");
    std::string ModeStr =
        fileOpenMode(ForCreate, Mode ? StringRef(Mode->Text) : "");
    Value *Handle = Builder.CreateCall(
        getFileOpenFn(), {NameVal, Builder.CreateGlobalString(ModeStr)});
    if (!emitStoreDesignator(*PathNode, Handle))
      return false;
    Value *Failed = Builder.CreateICmpEQ(Handle, ConstantInt::get(i32Ty(), 0));
    emitErrorTrapCheck(Failed);
    return true;
  }

  bool emitOpen(const ASTNode &Stmt) { return emitOpenOrCreate(Stmt, false); }
  bool emitCreate(const ASTNode &Stmt) { return emitOpenOrCreate(Stmt, true); }

  bool emitClose(const ASTNode &Stmt) {
    const ASTNode *PathNode = firstChildKind(Stmt, "Path");
    if (!PathNode)
      return error(Stmt, "CLOSE requires a path");
    Value *Handle = emitPathHandle(*PathNode);
    if (!Handle)
      return false;
    Builder.CreateCall(getFileCloseFn(), {Handle});
    return true;
  }

  bool emitDelete(const ASTNode &Stmt) {
    const ASTNode *Argument = firstChildKind(Stmt, "Argument");
    if (!Argument)
      return error(Stmt, "DELETE requires a file name");
    Value *NameVal = emitExprChild(*Argument);
    if (!NameVal)
      return false;
    if (!NameVal->getType()->isPointerTy())
      return error(*Argument, "DELETE expects a string file name");
    Builder.CreateCall(getFileDeleteFn(), {NameVal});
    return true;
  }

  bool emitBye(const ASTNode &Stmt) {
    Builder.CreateCall(getExit(), {ConstantInt::get(i32Ty(), 0)});
    Builder.CreateUnreachable();
    BasicBlock *AfterBye =
        BasicBlock::Create(Context, "after.bye", CurrentFunction);
    Builder.SetInsertPoint(AfterBye);
    return true;
  }

  bool emitDeg(const ASTNode &Stmt) {
    DegreesMode = true;
    return true;
  }

  bool emitRad(const ASTNode &Stmt) {
    DegreesMode = false;
    return true;
  }

  bool emitBase(const ASTNode &Stmt) {
    const ASTNode *Argument = firstChildKind(Stmt, "Argument");
    if (!Argument || Argument->Children.empty())
      return true;
    double Base = 0;
    if (!constantDataNumber(*Argument->Children.front(), Base) || Base != 0)
      return error(*Argument, "BASE only supports 0 (arrays are always "
                              "zero-based)");
    return true;
  }

  bool emitChangeDir(const ASTNode &Stmt) {
    const ASTNode *Argument = firstChildKind(Stmt, "Argument");
    if (!Argument)
      return error(Stmt, Stmt.Kind + " requires a directory path");
    Value *PathVal = emitExprChild(*Argument);
    if (!PathVal)
      return false;
    if (!PathVal->getType()->isPointerTy())
      return error(*Argument, Stmt.Kind + " expects a string path");
    Value *Status = Builder.CreateCall(getChdir(), {PathVal});
    Value *Failed = Builder.CreateICmpNE(Status, ConstantInt::get(i32Ty(), 0));
    emitErrorTrapCheck(Failed);
    return true;
  }

  bool emitSeek(const ASTNode &Stmt) {
    const ASTNode *PathNode = firstChildKind(Stmt, "Path");
    const ASTNode *Position = firstChildKind(Stmt, "Position");
    if (!PathNode || !Position)
      return error(Stmt, "SEEK requires a path and position");
    Value *Handle = emitPathHandle(*PathNode);
    Value *Pos = emitExprChild(*Position);
    if (!Handle || !Pos)
      return false;
    Pos = coerceScalar(Pos, i64Ty());
    Builder.CreateCall(getFileSeekFn(), {Handle, Pos});
    return true;
  }

  Value *resolveBufferPtr(const ASTNode &DesignatorNode, uint64_t &Size) {
    if (DesignatorNode.Children.empty()) {
      error(DesignatorNode, "designator is missing: " + DesignatorNode.Text);
      return nullptr;
    }
    const ASTNode &Root = *DesignatorNode.Children.front();
    DesignatorState State;
    if (!resolveOrCreateDesignator(Root, State))
      return nullptr;
    Type *SizeTy = State.IsArray ? State.StorageTy : State.ElemTy;
    Size = M->getDataLayout().getTypeAllocSize(SizeTy);
    return State.Ptr;
  }

  bool emitGetFile(const ASTNode &Stmt) {
    const ASTNode *PathNode = firstChildKind(Stmt, "Path");
    const ASTNode *Target = firstChildKind(Stmt, "Target");
    if (!PathNode || !Target)
      return error(Stmt, "GET requires a path and target");
    Value *Handle = emitPathHandle(*PathNode);
    if (!Handle)
      return false;
    uint64_t Size = 0;
    Value *Ptr = resolveBufferPtr(*Target, Size);
    if (!Ptr)
      return false;
    Builder.CreateCall(getFileGetFn(),
                       {Handle, Ptr, ConstantInt::get(i64Ty(), Size)});
    return true;
  }

  bool emitPutFile(const ASTNode &Stmt) {
    const ASTNode *PathNode = firstChildKind(Stmt, "Path");
    const ASTNode *ValueNode = firstChildKind(Stmt, "Value");
    if (!PathNode || !ValueNode)
      return error(Stmt, "PUT requires a path and value");
    Value *Handle = emitPathHandle(*PathNode);
    if (!Handle)
      return false;
    uint64_t Size = 0;
    Value *Ptr = resolveBufferPtr(*ValueNode, Size);
    if (!Ptr)
      return false;
    Builder.CreateCall(getFilePutFn(),
                       {Handle, Ptr, ConstantInt::get(i64Ty(), Size)});
    return true;
  }

  bool emitWriteFile(const ASTNode &Stmt) {
    const ASTNode *PathNode = firstChildKind(Stmt, "Path");
    if (!PathNode)
      return error(Stmt, "WRITE requires a path");
    Value *Handle = emitPathHandle(*PathNode);
    if (!Handle)
      return false;
    for (const std::unique_ptr<ASTNode> &Item : Stmt.Children) {
      if (Item->Kind != "Value")
        continue;
      Value *V = emitExprChild(*Item);
      if (!V)
        return false;
      if (V->getType()->isPointerTy()) {
        Builder.CreateCall(getFileWriteStrFn(), {Handle, V});
      } else if (V->getType()->isDoubleTy()) {
        Builder.CreateCall(getFileWriteRealFn(), {Handle, V});
      } else if (V->getType()->isIntegerTy()) {
        Builder.CreateCall(getFileWriteIntFn(),
                           {Handle, Builder.CreateSExtOrTrunc(V, i64Ty())});
      } else {
        return error(*Item, "WRITE expression has unsupported IR type");
      }
    }
    Builder.CreateCall(getFileNewlineFn(), {Handle});
    return true;
  }

  bool emitReadFileDesignator(Value *Handle, const ASTNode &DesignatorNode) {
    if (DesignatorNode.Children.empty())
      return error(DesignatorNode,
                   "designator is missing: " + DesignatorNode.Text);
    const ASTNode &Root = *DesignatorNode.Children.front();
    DesignatorState State;
    if (!resolveOrCreateDesignator(Root, State))
      return false;
    if (State.IsArray)
      return error(DesignatorNode, "READ target requires an index");

    if (State.Kind == BasicKind::String) {
      Value *Data = stringDataPtr(State.ElemTy, State.Ptr);
      uint64_t Capacity = cast<ArrayType>(State.ElemTy)->getNumElements();
      Builder.CreateCall(getFileReadLineFn(),
                         {Handle, Data, ConstantInt::get(i32Ty(), Capacity)});
      return true;
    }
    if (State.ElemTy->isDoubleTy()) {
      Value *V = Builder.CreateCall(getFileReadRealFn(), {Handle});
      Builder.CreateStore(V, State.Ptr);
      return true;
    }
    Value *V = Builder.CreateCall(getFileReadIntFn(), {Handle});
    Builder.CreateStore(coerceScalar(V, State.ElemTy), State.Ptr);
    return true;
  }

  bool emitReadFile(const ASTNode &Stmt) {
    const ASTNode *PathNode = firstChildKind(Stmt, "Path");
    if (!PathNode)
      return error(Stmt, "READ requires a path");
    Value *Handle = emitPathHandle(*PathNode);
    if (!Handle)
      return false;
    for (const std::unique_ptr<ASTNode> &Target : Stmt.Children) {
      if (Target->Kind != "Target")
        continue;
      if (!emitReadFileDesignator(Handle, *Target))
        return false;
    }
    return true;
  }

  bool emitEnd(const ASTNode &Stmt) {
    if (const ASTNode *EndValue = firstChildKind(Stmt, "Value")) {
      Value *V = emitExprChild(*EndValue);
      if (!V)
        return false;
      if (!emitPrintValue(*EndValue, V))
        return false;
      Builder.CreateCall(getPrintf(), {Builder.CreateGlobalString("\n")});
    }
    if (Builder.GetInsertBlock()->getTerminator() == nullptr)
      Builder.CreateRet(ConstantInt::get(i32Ty(), 0));
    return true;
  }

  bool emitPrintValue(const ASTNode &At, Value *V) {
    if (V->getType()->isPointerTy()) {
      Builder.CreateCall(getPrintf(), {Builder.CreateGlobalString("%s"), V});
      return true;
    }
    if (V->getType()->isDoubleTy()) {
      Builder.CreateCall(getPrintf(), {Builder.CreateGlobalString("%g"), V});
      return true;
    }
    if (V->getType()->isIntegerTy()) {
      Builder.CreateCall(
          getPrintf(), {Builder.CreateGlobalString("%d"),
                        Builder.CreateSExtOrTrunc(V, i32Ty())});
      return true;
    }
    return error(At, "END expression has unsupported IR type");
  }

  bool emitPrint(const ASTNode &Stmt) {
    FunctionCallee Printf = getPrintf();

    for (size_t I = 0; I < Stmt.Children.size(); ++I) {
      const std::unique_ptr<ASTNode> &Child = Stmt.Children[I];
      if (Child->Kind == "Separator")
        continue;
      if (Child->Kind != "Item")
        continue;
      const ASTNode *Expr = firstChildKind(*Child, "String");
      if (Expr) {
        Builder.CreateCall(Printf,
                           {Builder.CreateGlobalString("%s"),
                            Builder.CreateGlobalString(unquote(Expr->Text))});
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
        if (needsNumericSpacerBeforeNextItem(Stmt, I))
          Builder.CreateCall(Printf, {Builder.CreateGlobalString(" ")});
      } else if (V->getType()->isIntegerTy()) {
        V = Builder.CreateSExtOrTrunc(V, i32Ty());
        Builder.CreateCall(Printf, {Builder.CreateGlobalString("%d"), V});
        if (needsNumericSpacerBeforeNextItem(Stmt, I))
          Builder.CreateCall(Printf, {Builder.CreateGlobalString(" ")});
      } else {
        return error(*Child, "PRINT expression has unsupported IR type");
      }
    }

    if (shouldPrintNewline(Stmt))
      Builder.CreateCall(Printf, {Builder.CreateGlobalString("\n")});
    return true;
  }

  bool needsNumericSpacerBeforeNextItem(const ASTNode &Stmt,
                                        size_t CurrentIndex) const {
    for (size_t I = CurrentIndex + 1; I < Stmt.Children.size(); ++I) {
      const ASTNode &Next = *Stmt.Children[I];
      if (Next.Kind == "Separator")
        continue;
      if (Next.Kind != "Item")
        return false;
      const ASTNode *String = firstChildKind(Next, "String");
      if (!String)
        return false;
      std::string Text = unquote(String->Text);
      if (Text.empty())
        return false;
      return !std::isspace(static_cast<unsigned char>(Text.front()));
    }
    return false;
  }

  bool shouldPrintNewline(const ASTNode &Stmt) const {
    if (Stmt.Children.empty())
      return true;
    const ASTNode &Last = *Stmt.Children.back();
    return Last.Kind != "Separator";
  }

  struct PrintUsingPart {
    enum PartKind { Literal, Field };
    PartKind Kind = Literal;
    std::string Text;
    char Code = 0;
    int Width = 0;
    int Precision = -1;
  };

  static PrintUsingPart printUsingLiteral(std::string Text) {
    PrintUsingPart Part;
    Part.Kind = PrintUsingPart::Literal;
    Part.Text = std::move(Text);
    return Part;
  }

  static PrintUsingPart printUsingField(char Code, int Width, int Precision) {
    PrintUsingPart Part;
    Part.Kind = PrintUsingPart::Field;
    Part.Code = std::toupper(static_cast<unsigned char>(Code));
    Part.Width = Width;
    Part.Precision = Precision;
    return Part;
  }

  static std::vector<PrintUsingPart> parsePrintUsingFormat(StringRef Format) {
    std::vector<PrintUsingPart> Parts;
    std::string Text = Format.str();
    for (size_t I = 0; I < Text.size();) {
      if (Text[I] == ',' || std::isspace(static_cast<unsigned char>(Text[I]))) {
        ++I;
        continue;
      }

      if (Text[I] == '\'') {
        size_t End = Text.find('\'', I + 1);
        if (End == std::string::npos) {
          Parts.push_back(printUsingLiteral(Text.substr(I + 1)));
          break;
        }
        Parts.push_back(printUsingLiteral(Text.substr(I + 1, End - I - 1)));
        I = End + 1;
        continue;
      }

      char Code = std::toupper(static_cast<unsigned char>(Text[I]));
      if (Code == 'X') {
        ++I;
        int Width = 1;
        size_t Begin = I;
        while (I < Text.size() &&
               std::isdigit(static_cast<unsigned char>(Text[I])))
          ++I;
        if (I > Begin)
          Width = std::stoi(Text.substr(Begin, I - Begin));
        Parts.push_back(printUsingLiteral(std::string(Width, ' ')));
        continue;
      }
      if (Code != 'H' && Code != 'I' && Code != 'R' && Code != 'S') {
        ++I;
        continue;
      }

      ++I;
      size_t Begin = I;
      while (I < Text.size() &&
             std::isdigit(static_cast<unsigned char>(Text[I])))
        ++I;
      int Width = I > Begin ? std::stoi(Text.substr(Begin, I - Begin)) : 0;
      int Precision = -1;
      if (I < Text.size() && Text[I] == '.') {
        ++I;
        Begin = I;
        while (I < Text.size() &&
               std::isdigit(static_cast<unsigned char>(Text[I])))
          ++I;
        if (I > Begin)
          Precision = std::stoi(Text.substr(Begin, I - Begin));
      }
      while (I < Text.size() && Text[I] == '<')
        ++I;
      Parts.push_back(printUsingField(Code, Width, Precision));
    }
    return Parts;
  }

  static std::string printfFormat(const PrintUsingPart &Part) {
    std::string Format = "%";
    if (Part.Code == 'H')
      Format += "0";
    int Width = Part.Width;
    if (Part.Code == 'R' && Part.Precision >= 0 && Width > 0)
      Width = std::max(0, Width - Part.Precision - 1);
    if (Width > 0)
      Format += std::to_string(Width);
    if (Part.Precision >= 0 && Part.Code == 'R')
      Format += "." + std::to_string(Part.Precision);

    switch (Part.Code) {
    case 'H':
      Format += "X";
      break;
    case 'I':
      Format += "d";
      break;
    case 'S':
      Format += "s";
      break;
    case 'R':
    default:
      Format += Part.Precision >= 0 ? "f" : "g";
      break;
    }
    return Format;
  }

  static std::vector<const ASTNode *> printItems(const ASTNode &Stmt) {
    std::vector<const ASTNode *> Items;
    for (const std::unique_ptr<ASTNode> &Child : Stmt.Children)
      if (Child->Kind == "Item")
        Items.push_back(Child.get());
    return Items;
  }

  bool emitPrintUsing(const ASTNode &Stmt) {
    std::vector<PrintUsingPart> Parts;
    if (const ASTNode *Format = firstChildKind(Stmt, "Format"))
      if (const ASTNode *String = firstChildKind(*Format, "String"))
        Parts = parsePrintUsingFormat(unquote(String->Text));

    auto Items = printItems(Stmt);
    size_t ItemIndex = 0;
    for (const PrintUsingPart &Part : Parts) {
      if (Part.Kind == PrintUsingPart::Literal) {
        Builder.CreateCall(getPrintf(), {Builder.CreateGlobalString("%s"),
                                         Builder.CreateGlobalString(Part.Text)});
        continue;
      }

      if (ItemIndex >= Items.size())
        break;
      Value *V = emitExprChild(*Items[ItemIndex++]);
      if (!V)
        return false;

      switch (Part.Code) {
      case 'H':
        Builder.CreateCall(getPrintf(),
                           {Builder.CreateGlobalString(printfFormat(Part)),
                            coerceScalar(V, i32Ty())});
        break;
      case 'I':
        Builder.CreateCall(getPrintf(),
                           {Builder.CreateGlobalString(printfFormat(Part)),
                            coerceScalar(V, i32Ty())});
        break;
      case 'S':
        if (!V->getType()->isPointerTy())
          return error(*Items[ItemIndex - 1], "S print format expects string");
        Builder.CreateCall(getPrintf(),
                           {Builder.CreateGlobalString(printfFormat(Part)), V});
        break;
      case 'R':
      default:
        Builder.CreateCall(getPrintf(),
                           {Builder.CreateGlobalString(printfFormat(Part)),
                            coerceScalar(V, Type::getDoubleTy(Context))});
        break;
      }
    }

    for (; ItemIndex < Items.size(); ++ItemIndex) {
      Value *V = emitExprChild(*Items[ItemIndex]);
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
    if (shouldPrintNewline(Stmt))
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

  bool emitStoreDesignator(const ASTNode &DesignatorNode, Value *V) {
    if (DesignatorNode.Children.empty())
      return error(DesignatorNode,
                   "designator is missing: " + DesignatorNode.Text);
    const ASTNode &Root = *DesignatorNode.Children.front();
    DesignatorState State;
    if (!resolveOrCreateDesignator(Root, State))
      return false;
    if (State.IsArray)
      return error(DesignatorNode, "assignment target requires an index");
    Builder.CreateStore(coerceScalar(V, State.ElemTy), State.Ptr);
    return true;
  }

  bool emitScanDesignator(const ASTNode &DesignatorNode) {
    if (DesignatorNode.Children.empty())
      return error(DesignatorNode,
                   "designator is missing: " + DesignatorNode.Text);
    const ASTNode &Root = *DesignatorNode.Children.front();
    DesignatorState State;
    if (!resolveOrCreateDesignator(Root, State))
      return false;
    if (State.IsArray)
      return error(DesignatorNode, "INPUT target requires an index");
    Value *Ptr = State.Ptr;
    Type *ElemTy = State.ElemTy;
    BasicKind Kind = State.Kind;

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
    if (Builder.GetInsertBlock()->getTerminator() == nullptr)
      Builder.CreateBr(MergeBB);

    if (ElseBB) {
      Builder.SetInsertPoint(ElseBB);
      if (!emitBlock(*ElseBlock))
        return false;
      if (Builder.GetInsertBlock()->getTerminator() == nullptr)
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
    if (Builder.GetInsertBlock()->getTerminator() == nullptr)
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
    if (Builder.GetInsertBlock()->getTerminator() == nullptr)
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
    if (Builder.GetInsertBlock()->getTerminator() == nullptr)
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
    if (Builder.GetInsertBlock()->getTerminator() == nullptr)
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
    if (Builder.GetInsertBlock()->getTerminator() == nullptr)
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
    if (Call->Text == "SDL")
      return emitSdlCall(*Call);

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

  bool emitSdlCall(const ASTNode &Call) {
    if (Call.Children.empty())
      return error(Call, "SDL expects a command string");
    const ASTNode &CommandArg = *Call.Children.front();
    if (CommandArg.Kind != "String")
      return error(CommandArg, "SDL command must be a string literal");

    std::string Command = StringRef(unquote(CommandArg.Text)).lower();
    ArrayRef<std::unique_ptr<ASTNode>> Args(Call.Children);
    Args = Args.drop_front();

    auto EmitI32Arg = [&](const ASTNode &Arg) -> Value * {
      Value *V = emitExpr(Arg);
      if (!V)
        return nullptr;
      return coerceScalar(V, i32Ty());
    };

    auto EmitNoArg = [&](StringRef Name) -> bool {
      if (!Args.empty())
        return error(Call, ("SDL command '" + Command +
                            "' expects no arguments").c_str());
      Builder.CreateCall(getSdlFn(Name, Type::getVoidTy(Context), {}));
      return true;
    };

    auto EmitOneI32 = [&](StringRef Name) -> bool {
      if (Args.size() != 1)
        return error(Call, ("SDL command '" + Command +
                            "' expects one argument").c_str());
      Value *A = EmitI32Arg(*Args[0]);
      if (!A)
        return false;
      Builder.CreateCall(getSdlFn(Name, Type::getVoidTy(Context), {i32Ty()}),
                         {A});
      return true;
    };

    if (Command == "open") {
      if (Args.size() != 2)
        return error(Call, "SDL command 'open' expects width and height");
      Value *Width = EmitI32Arg(*Args[0]);
      Value *Height = EmitI32Arg(*Args[1]);
      if (!Width || !Height)
        return false;
      Builder.CreateCall(
          getSdlFn("basic09_sdl_open", Type::getVoidTy(Context),
                   {i32Ty(), i32Ty()}),
          {Width, Height});
      return true;
    }
    if (Command == "clear")
      return EmitOneI32("basic09_sdl_clear");
    if (Command == "poll")
      return EmitNoArg("basic09_sdl_poll");
    if (Command == "present")
      return EmitNoArg("basic09_sdl_present");
    if (Command == "close")
      return EmitNoArg("basic09_sdl_close");
    if (Command == "delay")
      return EmitOneI32("basic09_sdl_delay");
    if (Command == "pset") {
      if (Args.size() != 3)
        return error(Call, "SDL command 'pset' expects x, y, color");
      Value *X = EmitI32Arg(*Args[0]);
      Value *Y = EmitI32Arg(*Args[1]);
      Value *Color = EmitI32Arg(*Args[2]);
      if (!X || !Y || !Color)
        return false;
      Builder.CreateCall(
          getSdlFn("basic09_sdl_pset", Type::getVoidTy(Context),
                   {i32Ty(), i32Ty(), i32Ty()}),
          {X, Y, Color});
      return true;
    }
    if (Command == "fillbox") {
      if (Args.size() != 5)
        return error(Call, "SDL command 'fillbox' expects x, y, width, height, color");
      Value *X = EmitI32Arg(*Args[0]);
      Value *Y = EmitI32Arg(*Args[1]);
      Value *Width = EmitI32Arg(*Args[2]);
      Value *Height = EmitI32Arg(*Args[3]);
      Value *Color = EmitI32Arg(*Args[4]);
      if (!X || !Y || !Width || !Height || !Color)
        return false;
      Builder.CreateCall(
          getSdlFn("basic09_sdl_fillbox", Type::getVoidTy(Context),
                   {i32Ty(), i32Ty(), i32Ty(), i32Ty(), i32Ty()}),
          {X, Y, Width, Height, Color});
      return true;
    }
    if (Command == "line") {
      if (Args.size() != 5)
        return error(Call, "SDL command 'line' expects x1, y1, x2, y2, color");
      Value *X1 = EmitI32Arg(*Args[0]);
      Value *Y1 = EmitI32Arg(*Args[1]);
      Value *X2 = EmitI32Arg(*Args[2]);
      Value *Y2 = EmitI32Arg(*Args[3]);
      Value *Color = EmitI32Arg(*Args[4]);
      if (!X1 || !Y1 || !X2 || !Y2 || !Color)
        return false;
      Builder.CreateCall(
          getSdlFn("basic09_sdl_line", Type::getVoidTy(Context),
                   {i32Ty(), i32Ty(), i32Ty(), i32Ty(), i32Ty()}),
          {X1, Y1, X2, Y2, Color});
      return true;
    }

    return error(CommandArg, ("unknown SDL command: " + Command).c_str());
  }

  Value *emitSdlFunction(const ASTNode &Call) {
    if (Call.Children.empty()) {
      error(Call, "SDL expects a command string");
      return nullptr;
    }
    const ASTNode &CommandArg = *Call.Children.front();
    if (CommandArg.Kind != "String") {
      error(CommandArg, "SDL command must be a string literal");
      return nullptr;
    }
    if (Call.Children.size() != 1) {
      error(Call, "SDL query expects only a command string");
      return nullptr;
    }

    std::string Command = StringRef(unquote(CommandArg.Text)).lower();
    if (Command == "quit")
      return Builder.CreateTrunc(
          Builder.CreateCall(getSdlFn("basic09_sdl_quit", i32Ty(), {})),
          i16Ty());
    if (Command == "key")
      return Builder.CreateTrunc(
          Builder.CreateCall(getSdlFn("basic09_sdl_key", i32Ty(), {})),
          i16Ty());

    error(CommandArg, ("unknown SDL query: " + Command).c_str());
    return nullptr;
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
      if (Expr.Text == "DATE$")
        return emitDate(Expr);
      if (firstChildKind(Expr, "Field")) {
        LocalInfo *Local = getOrCreateScalarLocal(Expr.Text);
        if (!Local)
          return nullptr;
        if (Local->Kind != BasicKind::Record) {
          error(Expr, "value is not a record: " + Expr.Text);
          return nullptr;
        }
        DesignatorState State;
        if (!resolveDesignator(Expr, State))
          return nullptr;
        return loadDesignatorValue(State, Expr);
      }
      LocalInfo *Local = getOrCreateScalarLocal(Expr.Text);
      if (!Local)
        return nullptr;
      if (Local->IsArray) {
        error(Expr, "array value requires an index: " + Expr.Text);
        return nullptr;
      }
      if (Local->Kind == BasicKind::Record) {
        error(Expr, "record value requires a field: " + Expr.Text);
        return nullptr;
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
      DesignatorState State;
      if (!resolveDesignator(Expr, State))
        return nullptr;
      return loadDesignatorValue(State, Expr);
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

    if (Expr.Text == "SDL")
      return emitSdlFunction(Expr);

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
    if (Expr.Text == "DATE$")
      return emitDate(Expr);
    if (Expr.Text == "EOF")
      return emitEof(Expr);

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
      return Builder.CreateCall(getUnaryDoubleFn("cos"), {emitToRadians(Arg)});
    if (Expr.Text == "SIN")
      return Builder.CreateCall(getUnaryDoubleFn("sin"), {emitToRadians(Arg)});
    if (Expr.Text == "ATN")
      return emitFromRadians(
          Builder.CreateCall(getUnaryDoubleFn("atan"), {Arg}));
    if (Expr.Text == "ACS")
      return emitFromRadians(
          Builder.CreateCall(getUnaryDoubleFn("acos"), {Arg}));
    if (Expr.Text == "ASN")
      return emitFromRadians(
          Builder.CreateCall(getUnaryDoubleFn("asin"), {Arg}));
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

  Value *emitDate(const ASTNode &Expr) {
    Value *TimeVar = createEntryAlloca(i64Ty(), "date.time");
    Builder.CreateCall(getTimeFunc(), {TimeVar});
    Value *TmPtr = Builder.CreateCall(getLocaltimeFunc(), {TimeVar});
    Value *Dest = createTempString();
    Builder.CreateCall(getStrftimeFunc(),
                       {Dest, ConstantInt::get(i64Ty(), 256),
                        Builder.CreateGlobalString("%Y/%m/%d %H:%M:%S"), TmPtr});
    return Dest;
  }

  Value *emitEof(const ASTNode &Expr) {
    if (Expr.Children.size() != 1) {
      error(Expr, "EOF expects one argument");
      return nullptr;
    }
    Value *Handle = emitExpr(*Expr.Children.front());
    if (!Handle)
      return nullptr;
    Handle = coerceScalar(Handle, i32Ty());
    Value *Result = Builder.CreateCall(getFileEofFn(), {Handle});
    return Builder.CreateSExtOrTrunc(Result, i16Ty());
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

  // Trig builtins operate on radians internally; DEG makes them accept and
  // return degrees instead, matching OS-9 BASIC09's DEG/RAD mode toggle.
  Value *emitToRadians(Value *Arg) {
    if (!DegreesMode)
      return Arg;
    return Builder.CreateFMul(
        Arg, ConstantFP::get(doubleTy(), 3.14159265358979323846 / 180.0));
  }

  Value *emitFromRadians(Value *Arg) {
    if (!DegreesMode)
      return Arg;
    return Builder.CreateFMul(
        Arg, ConstantFP::get(doubleTy(), 180.0 / 3.14159265358979323846));
  }

  Value *emitArrayElementPtr(const ASTNode &Call) {
    auto It = Locals.find(Call.Text);
    if (It == Locals.end() || !It->second.IsArray) {
      error(Call, "unknown array: " + Call.Text);
      return nullptr;
    }
    DesignatorState State;
    if (!resolveDesignator(Call, State))
      return nullptr;
    return State.Ptr;
  }

  Value *stringDataPtr(Type *StringTy, Value *Ptr) {
    return Builder.CreateInBoundsGEP(
        StringTy, Ptr,
        {ConstantInt::get(i32Ty(), 0), ConstantInt::get(i32Ty(), 0)});
  }

  Value *createTempString() {
    AllocaInst *Temp =
        createEntryAlloca(ArrayType::get(Type::getInt8Ty(Context), 256), "tmp.str");
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

  FunctionCallee getSdlFn(StringRef Name, Type *ReturnTy,
                          ArrayRef<Type *> ArgTys) {
    FunctionType *FT = FunctionType::get(ReturnTy, ArgTys, false);
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

  FunctionCallee getTimeFunc() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(i64Ty(), {PtrTy}, false);
    return M->getOrInsertFunction("time", FT);
  }

  FunctionCallee getLocaltimeFunc() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(PtrTy, {PtrTy}, false);
    return M->getOrInsertFunction("localtime", FT);
  }

  FunctionCallee getStrftimeFunc() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT =
        FunctionType::get(i64Ty(), {PtrTy, i64Ty(), PtrTy, PtrTy}, false);
    return M->getOrInsertFunction("strftime", FT);
  }

  FunctionCallee getCalloc() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(PtrTy, {i64Ty(), i64Ty()}, false);
    return M->getOrInsertFunction("calloc", FT);
  }

  bool getBasicType(const ASTNode &Decl, StringRef BasicType, BasicKind &Kind,
                    Type *&IRTy, uint64_t &StringLength) {
    std::string RecordTypeName;
    return getBasicType(Decl, BasicType, Kind, IRTy, StringLength,
                        RecordTypeName);
  }

  bool getBasicType(const ASTNode &Decl, StringRef BasicType, BasicKind &Kind,
                    Type *&IRTy, uint64_t &StringLength,
                    std::string &RecordTypeName) {
    RecordTypeName.clear();
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
    auto TypeIt = RecordTypes.find(BasicType);
    if (TypeIt == RecordTypes.end())
      return error(Decl, ("unknown type: " + BasicType).str());
    Kind = BasicKind::Record;
    IRTy = TypeIt->second.IRTy;
    RecordTypeName = BasicType.str();
    return true;
  }

  bool collectRecordTypes(const ASTNode &Procedure) {
    RecordTypes = GlobalRecordTypes;
    const ASTNode *Body = firstChildKind(Procedure, "Block");
    if (!Body)
      return true;
    for (const std::unique_ptr<ASTNode> &Stmt : Body->Children) {
      if (Stmt->Kind != "Type")
        continue;
      const ASTNode *TypeName = firstChildKind(*Stmt, "TypeName");
      if (!TypeName)
        continue;
      if (!buildRecordType(*Stmt, TypeName->Text))
        return false;
    }
    return true;
  }

  bool buildRecordType(const ASTNode &TypeNode, StringRef Name) {
    RecordTypeInfo Info;
    std::vector<Type *> FieldTys;
    for (const std::unique_ptr<ASTNode> &Field : TypeNode.Children) {
      if (Field->Kind != "Field")
        continue;
      const ASTNode *FieldTypeName = firstChildKind(*Field, "TypeName");
      StringRef FieldBasicType =
          FieldTypeName ? StringRef(FieldTypeName->Text) : "INTEGER";
      RecordFieldInfo FieldInfo;
      FieldInfo.Name = Field->Text;
      if (!getBasicType(*Field, FieldBasicType, FieldInfo.Kind,
                        FieldInfo.ElementTy, FieldInfo.StringLength,
                        FieldInfo.RecordTypeName))
        return false;

      Type *FieldStorageTy = FieldInfo.ElementTy;
      std::vector<uint64_t> Bounds;
      for (const std::unique_ptr<ASTNode> &Child : Field->Children)
        if (Child->Kind == "Bound")
          Bounds.push_back(constantExtent(*Child));
      for (uint64_t Bound : llvm::reverse(Bounds))
        FieldStorageTy = ArrayType::get(FieldStorageTy, Bound + 1);
      FieldInfo.StorageTy = FieldStorageTy;
      FieldInfo.IsArray = !Bounds.empty();

      Info.FieldIndex[FieldInfo.Name] = FieldTys.size();
      FieldTys.push_back(FieldStorageTy);
      Info.Fields.push_back(std::move(FieldInfo));
    }
    Info.IRTy = StructType::create(Context, FieldTys,
                                   ("type." + Name).str());
    RecordTypes[Name] = std::move(Info);
    return true;
  }

  const RecordFieldInfo *findRecordField(StringRef TypeName, StringRef Field,
                                         unsigned &Index) {
    auto TypeIt = RecordTypes.find(TypeName);
    if (TypeIt == RecordTypes.end())
      return nullptr;
    auto FieldIt = TypeIt->second.FieldIndex.find(Field.upper());
    if (FieldIt == TypeIt->second.FieldIndex.end())
      return nullptr;
    Index = FieldIt->second;
    return &TypeIt->second.Fields[Index];
  }

  // Walks a parsed designator tree (a Var/Call root with optional trailing
  // Index-argument children and/or a chain of nested Field children, e.g.
  // `arr(i).b.c(j)`) and resolves it to a pointer plus type/kind info. The
  // named local must already exist.
  bool resolveDesignator(const ASTNode &Root, DesignatorState &State) {
    auto It = Locals.find(Root.Text);
    if (It == Locals.end())
      return error(Root, "unknown variable: " + Root.Text);
    LocalInfo &Local = It->second;

    State.Ptr = Local.Slot;
    State.ElemTy = Local.ElementTy;
    State.StorageTy = Local.StorageTy;
    State.Kind = Local.Kind;
    State.IsArray = Local.IsArray;
    State.RecordTypeName = Local.RecordTypeName;

    if (State.IsArray) {
      std::vector<Value *> Indices;
      Indices.push_back(ConstantInt::get(i32Ty(), 0));
      for (const std::unique_ptr<ASTNode> &Child : Root.Children) {
        if (Child->Kind == "Field")
          continue;
        Value *Idx = emitExpr(*Child);
        if (!Idx)
          return false;
        Indices.push_back(coerceScalar(Idx, i32Ty()));
      }
      if (Indices.size() > 1) {
        State.Ptr = Builder.CreateInBoundsGEP(State.StorageTy, State.Ptr, Indices);
        State.StorageTy = State.ElemTy;
        State.IsArray = false;
      }
    }

    const ASTNode *FieldNode = firstChildKind(Root, "Field");
    while (FieldNode) {
      if (State.IsArray)
        return error(*FieldNode,
                     "array reference requires an index before field access: " +
                         FieldNode->Text);
      if (State.Kind != BasicKind::Record)
        return error(*FieldNode, "value is not a record: " + FieldNode->Text);
      unsigned Index = 0;
      const RecordFieldInfo *FieldInfo =
          findRecordField(State.RecordTypeName, FieldNode->Text, Index);
      if (!FieldInfo)
        return error(*FieldNode, "unknown field: " + FieldNode->Text);

      State.Ptr = Builder.CreateInBoundsGEP(
          State.StorageTy, State.Ptr,
          {ConstantInt::get(i32Ty(), 0), ConstantInt::get(i32Ty(), Index)});
      State.Kind = FieldInfo->Kind;
      State.ElemTy = FieldInfo->ElementTy;
      State.StorageTy = FieldInfo->StorageTy;
      State.IsArray = FieldInfo->IsArray;
      State.RecordTypeName = FieldInfo->RecordTypeName;

      if (State.IsArray) {
        std::vector<Value *> Indices;
        Indices.push_back(ConstantInt::get(i32Ty(), 0));
        for (const std::unique_ptr<ASTNode> &Child : FieldNode->Children) {
          if (Child->Kind == "Field")
            continue;
          Value *Idx = emitExpr(*Child);
          if (!Idx)
            return false;
          Indices.push_back(coerceScalar(Idx, i32Ty()));
        }
        if (Indices.size() > 1) {
          State.Ptr =
              Builder.CreateInBoundsGEP(State.StorageTy, State.Ptr, Indices);
          State.StorageTy = State.ElemTy;
          State.IsArray = false;
        }
      }

      FieldNode = firstChildKind(*FieldNode, "Field");
    }
    return true;
  }

  // Like resolveDesignator, but auto-creates an implicit scalar local when
  // Root is a bare, undeclared identifier (matching BASIC09's implicit
  // variable declaration rules).
  bool resolveOrCreateDesignator(const ASTNode &Root, DesignatorState &State) {
    if (!Locals.count(Root.Text)) {
      if (Root.Kind != "Var" || !Root.Children.empty())
        return error(Root, "unknown variable: " + Root.Text);
      LocalInfo *Local = getOrCreateScalarLocal(Root.Text);
      if (!Local)
        return false;
      State.Ptr = Local->Slot;
      State.ElemTy = Local->ElementTy;
      State.StorageTy = Local->StorageTy;
      State.Kind = Local->Kind;
      State.IsArray = false;
      State.RecordTypeName = Local->RecordTypeName;
      return true;
    }
    return resolveDesignator(Root, State);
  }

  Value *loadDesignatorValue(const DesignatorState &State, const ASTNode &At) {
    if (State.IsArray) {
      error(At, "array value requires an index");
      return nullptr;
    }
    if (State.Kind == BasicKind::String)
      return stringDataPtr(State.ElemTy, State.Ptr);
    return Builder.CreateLoad(State.ElemTy, State.Ptr);
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

  FunctionCallee getSystem() {
    FunctionType *FT =
        FunctionType::get(i32Ty(), PointerType::getUnqual(Context), false);
    return M->getOrInsertFunction("system", FT);
  }

  FunctionCallee getExit() {
    FunctionType *FT = FunctionType::get(Type::getVoidTy(Context), {i32Ty()}, false);
    return M->getOrInsertFunction("exit", FT);
  }

  FunctionCallee getChdir() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(i32Ty(), {PtrTy}, false);
    return M->getOrInsertFunction("chdir", FT);
  }

  FunctionCallee getFileOpenFn() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(i32Ty(), {PtrTy, PtrTy}, false);
    return M->getOrInsertFunction("basic09_file_open", FT);
  }

  FunctionCallee getFileCloseFn() {
    FunctionType *FT = FunctionType::get(Type::getVoidTy(Context), {i32Ty()}, false);
    return M->getOrInsertFunction("basic09_file_close", FT);
  }

  FunctionCallee getFileDeleteFn() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(Type::getVoidTy(Context), {PtrTy}, false);
    return M->getOrInsertFunction("basic09_file_delete", FT);
  }

  FunctionCallee getFileSeekFn() {
    FunctionType *FT = FunctionType::get(Type::getVoidTy(Context),
                                        {i32Ty(), i64Ty()}, false);
    return M->getOrInsertFunction("basic09_file_seek", FT);
  }

  FunctionCallee getFileGetFn() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(Type::getVoidTy(Context),
                                        {i32Ty(), PtrTy, i64Ty()}, false);
    return M->getOrInsertFunction("basic09_file_get", FT);
  }

  FunctionCallee getFilePutFn() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(Type::getVoidTy(Context),
                                        {i32Ty(), PtrTy, i64Ty()}, false);
    return M->getOrInsertFunction("basic09_file_put", FT);
  }

  FunctionCallee getFileWriteStrFn() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(Type::getVoidTy(Context),
                                        {i32Ty(), PtrTy}, false);
    return M->getOrInsertFunction("basic09_file_write_str", FT);
  }

  FunctionCallee getFileWriteRealFn() {
    FunctionType *FT = FunctionType::get(Type::getVoidTy(Context),
                                        {i32Ty(), doubleTy()}, false);
    return M->getOrInsertFunction("basic09_file_write_real", FT);
  }

  FunctionCallee getFileWriteIntFn() {
    FunctionType *FT = FunctionType::get(Type::getVoidTy(Context),
                                        {i32Ty(), i64Ty()}, false);
    return M->getOrInsertFunction("basic09_file_write_int", FT);
  }

  FunctionCallee getFileNewlineFn() {
    FunctionType *FT = FunctionType::get(Type::getVoidTy(Context), {i32Ty()}, false);
    return M->getOrInsertFunction("basic09_file_newline", FT);
  }

  FunctionCallee getFileReadLineFn() {
    Type *PtrTy = PointerType::getUnqual(Context);
    FunctionType *FT = FunctionType::get(Type::getVoidTy(Context),
                                        {i32Ty(), PtrTy, i32Ty()}, false);
    return M->getOrInsertFunction("basic09_file_readline", FT);
  }

  FunctionCallee getFileReadRealFn() {
    FunctionType *FT = FunctionType::get(doubleTy(), {i32Ty()}, false);
    return M->getOrInsertFunction("basic09_file_read_real", FT);
  }

  FunctionCallee getFileReadIntFn() {
    FunctionType *FT = FunctionType::get(i64Ty(), {i32Ty()}, false);
    return M->getOrInsertFunction("basic09_file_read_int", FT);
  }

  FunctionCallee getFileEofFn() {
    FunctionType *FT = FunctionType::get(i32Ty(), {i32Ty()}, false);
    return M->getOrInsertFunction("basic09_file_eof", FT);
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
