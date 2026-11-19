//===-- DeadCodeModel.cpp ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Analysis/FlowSensitive/Models/DeadCodeModel.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/AST/Stmt.h"
#include "clang/Analysis/CFG.h"
#include "clang/Analysis/FlowSensitive/DataflowEnvironment.h"
#include "clang/Analysis/FlowSensitive/Formula.h"
#include "clang/Analysis/FlowSensitive/NoopLattice.h"
#include "clang/Analysis/FlowSensitive/Value.h"

namespace clang::dataflow {
namespace {

enum class EvaluatedBool { Unknown, True, False };

const Formula *buildFormula(const Expr *E, const Environment &Env) {
  if (!E)
    return nullptr;

  E = E->IgnoreParenImpCasts();

  Value *V = Env.getValue(*E);
  if (!V) {
    if (const auto *Ref = dyn_cast<DeclRefExpr>(E))
      V = Env.getValue(*Ref->getDecl());
  }

  if (const auto *BV = dyn_cast_or_null<BoolValue>(V))
    return &BV->formula();

  auto &Arena = Env.arena();

  if (const auto *BL = dyn_cast<CXXBoolLiteralExpr>(E))
    return &Arena.makeLiteral(BL->getValue());

  if (const auto *U = dyn_cast<UnaryOperator>(E)) {
    if (U->getOpcode() == UO_LNot) {
      if (const Formula *Sub = buildFormula(U->getSubExpr(), Env))
        return &Arena.makeNot(*Sub);
    }
  }

  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    const Formula *Left = buildFormula(BO->getLHS(), Env);
    const Formula *Right = buildFormula(BO->getRHS(), Env);
    if (Left && Right) {
      switch (BO->getOpcode()) {
      case BO_LAnd:
        return &Arena.makeAnd(*Left, *Right);
      case BO_LOr:
        return &Arena.makeOr(*Left, *Right);
      case BO_EQ:
        return &Arena.makeEquals(*Left, *Right);
      case BO_NE:
        return &Arena.makeNot(Arena.makeEquals(*Left, *Right));
      default:
        break;
      }
    }
  }

  return nullptr;
}

EvaluatedBool evaluateCondition(const Expr *Cond, const Environment &Env) {
  const Formula *F = buildFormula(Cond, Env);
  if (!F)
    return EvaluatedBool::Unknown;

  if (Env.proves(*F))
    return EvaluatedBool::True;

  if (Env.proves(Env.arena().makeNot(*F)))
    return EvaluatedBool::False;

  return EvaluatedBool::Unknown;
}

const IfStmt *getEnclosingIf(const Stmt *S, ASTContext &Ctx) {
  const Stmt *Curr = S;
  while (Curr) {
    const auto &Parents = Ctx.getParents(*Curr);
    if (Parents.empty())
      return nullptr;

    // FIXME : Parents[0] is not always the correct choice
    const Stmt *ParentStmt = Parents[0].get<Stmt>();
    if (!ParentStmt)
      return nullptr;

    if (const auto *If = dyn_cast<IfStmt>(ParentStmt)) {
      if (If->getCond() == Curr)
        return If;
      return nullptr;
    }

    if (isa<ImplicitCastExpr>(ParentStmt) || isa<ParenExpr>(ParentStmt) ||
        isa<ExprWithCleanups>(ParentStmt) || isa<UnaryOperator>(ParentStmt) ||
        isa<BinaryOperator>(ParentStmt)) {
      Curr = ParentStmt;
      continue;
    }

    return nullptr;
  }
  return nullptr;
}

} // namespace

DeadCodeDiagnoser::DeadCodeDiagnoser() = default;

DeadCodeDiagnoser::ResultType DeadCodeDiagnoser::operator()(
    const CFGElement &Elt, ASTContext &Ctx,
    const TransferStateForDiagnostics<NoopLattice> &State) {

  auto StmtElt = Elt.getAs<CFGStmt>();
  if (!StmtElt)
    return {};

  const IfStmt *If = getEnclosingIf(StmtElt->getStmt(), Ctx);
  if (!If)
    return {};

  if (If->getBeginLoc().isInvalid() ||
      Ctx.getSourceManager().isInSystemHeader(If->getBeginLoc()))
    return {};

  if (!State.Env.allows(State.Env.arena().makeLiteral(true)))
    return {};

  if (DiagnosedIfs.contains(If))
    return {};

  EvaluatedBool CondResult = evaluateCondition(If->getCond(), State.Env);

  ResultType Diagnostics;
  // FIXME: add markers to the first stmt of a compound stmt
  if (CondResult == EvaluatedBool::True && If->getElse()) {
    DiagnosedIfs.insert(If);
    Diagnostics.push_back(
        {If->getElse()->getBeginLoc(), DiagnosticType::Unreachable});
  } else if (CondResult == EvaluatedBool::False) {
    DiagnosedIfs.insert(If);
    Diagnostics.push_back(
        {If->getThen()->getBeginLoc(), DiagnosticType::Unreachable});
  }

  return Diagnostics;
}

} // namespace clang::dataflow
