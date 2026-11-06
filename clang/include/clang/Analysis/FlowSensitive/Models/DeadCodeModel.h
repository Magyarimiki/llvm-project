//===-- DeadCodeModel.h -----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef CLANG_ANALYSIS_FLOWSENSITIVE_MODELS_DEADCODEMODEL_H
#define CLANG_ANALYSIS_FLOWSENSITIVE_MODELS_DEADCODEMODEL_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/Stmt.h"
#include "clang/Analysis/CFG.h"
#include "clang/Analysis/FlowSensitive/CFGMatchSwitch.h"
#include "clang/Analysis/FlowSensitive/DataflowAnalysis.h"
#include "clang/Analysis/FlowSensitive/DataflowEnvironment.h"
#include "clang/Analysis/FlowSensitive/NoopLattice.h"
#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/SmallVector.h"

namespace clang::dataflow {

class DeadCodeModel : public DataflowAnalysis<DeadCodeModel, NoopLattice> {
public:
  explicit DeadCodeModel(ASTContext &Context)
      : DataflowAnalysis<DeadCodeModel, NoopLattice>(Context) {}

  static NoopLattice initialElement() { return {}; }

  void transfer(const CFGElement &E, NoopLattice &State, Environment &Env) {}

  void transferBranch(bool Branch, const Stmt *S, NoopLattice &State,
                      Environment &Env);
};

class DeadCodeDiagnoser {
public:
  /// Returns source locations that are not reachable, based on dataflow analysis
  enum class DiagnosticType { Unreachable };

  struct DiagnosticEntry {
    SourceLocation Location;
    DiagnosticType Type;
  };
  using ResultType = llvm::SmallVector<DiagnosticEntry>;

private:
  CFGMatchSwitch<const Environment, ResultType> DiagnoseMatchSwitch;

public:
  DeadCodeDiagnoser();

  ResultType operator()(const CFGElement &Elt, ASTContext &Ctx,
                        const TransferStateForDiagnostics<NoopLattice> &State);
};

} // namespace clang::dataflow

#endif