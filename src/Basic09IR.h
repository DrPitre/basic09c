//===- Basic09IR.h - BASIC09 LLVM IR lowering ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_BASIC09C_BASIC09IR_H
#define LLVM_TOOLS_BASIC09C_BASIC09IR_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {
namespace basic09 {

struct ASTNode;

bool emitLLVMIR(const ASTNode &Root, StringRef ModuleName, StringRef Triple,
                raw_ostream &OS, raw_ostream &Errs);

} // namespace basic09
} // namespace llvm

#endif
