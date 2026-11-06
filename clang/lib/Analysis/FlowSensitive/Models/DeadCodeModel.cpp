//===-- DeadCodeModel.cpp ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Analysis/FlowSensitive/Models/DeadCodeModel.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Analysis/CFG.h"
#include "clang/Analysis/FlowSensitive/CFGMatchSwitch.h"
#include "clang/Analysis/FlowSensitive/DataflowAnalysis.h"
#include "clang/Analysis/FlowSensitive/DataflowEnvironment.h"
#include "clang/Analysis/FlowSensitive/DataflowLattice.h"
#include "clang/Analysis/FlowSensitive/MapLattice.h"
#include "clang/Analysis/FlowSensitive/NoopLattice.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

namespace clang::dataflow {
namespace {
using namespace ast_matchers;
using Diagnoser = DeadCodeDiagnoser;

constexpr char ifWithStmt[] = "ifWithStmt";
constexpr char ifWithCompoundStmt[] = "ifWithCompoundStmt";

auto conditionMatcher() {
  return 
    stmt(
      anyOf(
        hasParent(
          ifStmt().bind(ifWithStmt)
        ),
        hasParent(
          compoundStmt(
            hasParent(
              ifStmt().bind(ifWithCompoundStmt)
            )
          )
        )
      )
    );
}

DeadCodeDiagnoser::ResultType
diagnoseAnyDeadCondition(const Stmt *S, const MatchFinder::MatchResult &Result,
                         const Environment &Env) {
  //llvm::outs() << "\n========== CFG CALLBACK ==========\n";

  const IfStmt *If = nullptr;
  If = Result.Nodes.getNodeAs<IfStmt>(ifWithCompoundStmt);
  if (!If) {
    If = Result.Nodes.getNodeAs<IfStmt>(ifWithStmt);
  } 
  if (!If) {
    llvm::outs() << "Missing if\n";
    return {};
  }

  const Stmt *Then = If->getThen();
  const Stmt *Else = If->getElse();

  //If->dumpColor();

  //llvm::outs() << "Statement type: " << If->getStmtClassName() << "\n";

  if (If->getBeginLoc().isValid()) {
    If->getBeginLoc().print(llvm::outs(), *Result.SourceManager);
    //llvm::outs() << "\n";
  }

  const Expr *Condition = If->getCond();
  Value *Val = Env.getValue(*Condition);
  if (!Val) {
    //llvm::outs() << "Null expr value\n";
    return {};
  }

  auto BoolVal = dyn_cast<BoolValue>(Val);
  if (!BoolVal) {
    //llvm::outs() << "Null bool value\n";
    return {};
  }

  if (Env.proves(Env.makeNot(*BoolVal).formula())) {
    return {{Then->getBeginLoc(), Diagnoser::DiagnosticType::Unreachable}};
  }

  if (Else && Env.proves(BoolVal->formula())) {
    return {{Else->getBeginLoc(), Diagnoser::DiagnosticType::Unreachable}};
  }

  return {};
}

auto buildDiagnoseMatchSwitch() {
   return CFGMatchSwitchBuilder<const Environment, Diagnoser::ResultType>()
       .CaseOfCFGStmt<Stmt>(conditionMatcher(), diagnoseAnyDeadCondition)
       .Build();
 }
} // namespace

void DeadCodeModel::transferBranch(bool Branch, const Stmt *S,
                                   NoopLattice &State, Environment &Env) {
  
}

DeadCodeDiagnoser::DeadCodeDiagnoser()
    : DiagnoseMatchSwitch(buildDiagnoseMatchSwitch()) {}

DeadCodeDiagnoser::ResultType DeadCodeDiagnoser::operator()(
    const CFGElement &Elt, ASTContext &Ctx,
    const TransferStateForDiagnostics<NoopLattice> &State) {
  return DiagnoseMatchSwitch(Elt, Ctx, State.Env);
}
} // namespace clang::dataflow