
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
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
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

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

#include <iterator>
#include <vector>

namespace clang::dataflow {
namespace {

using namespace ast_matchers;
using Diagnoser = DeadCodeDiagnoser;

//auto conditionMatcher() { return stmt(); }

class IfStmtCollector : public RecursiveASTVisitor<IfStmtCollector> {
public:
  explicit IfStmtCollector(ASTContext &Context) : Context(&Context) {}

  bool VisitIfStmt(IfStmt *If) {
    if (If->getBeginLoc().isInvalid())
      return true;

    if (Context->getSourceManager().isInSystemHeader(If->getBeginLoc()))
      return true;

    IfStatements.push_back(If);
    return true;
  }

  std::vector<IfStmt *> IfStatements;

private:
  ASTContext *Context;
};


class StmtFinder : public clang::RecursiveASTVisitor<StmtFinder> {
public:
  explicit StmtFinder(const clang::Stmt *Target) : Target(Target) {}

  bool Found = false;
  bool VisitStmt(clang::Stmt *S) {
    if (S == Target) {
      Found = true;
      return false;
    }

    return true;
  }

private:
  const clang::Stmt *Target;
};

static bool isInside(const clang::Stmt *Root, const clang::Stmt *Target) {
  if (!Root || !Target)
    return false;

  StmtFinder Finder(Target);
  Finder.TraverseStmt(const_cast<clang::Stmt *>(Root));

  return Finder.Found;
}

DeadCodeDiagnoser::ResultType
diagnoseAnyDeadCondition(const Stmt *S, const MatchFinder::MatchResult &Result,
                         const Environment &Env,
                         llvm::ArrayRef<const IfStmt *> IfStatements) {

  if (!Result.Context || !Result.SourceManager)
    return {};

  enum class BoolKnowledge { Unknown, True, False };

  auto EvaluateBool = [&](auto &&Self, const Expr *E) -> BoolKnowledge {
    if (!E)
      return BoolKnowledge::Unknown;

    E = E->IgnoreParenImpCasts();

    if (const auto *BL = dyn_cast<CXXBoolLiteralExpr>(E)) {
      return BL->getValue() ? BoolKnowledge::True : BoolKnowledge::False;
    }

    if (const auto *U = dyn_cast<UnaryOperator>(E)) {
      if (U->getOpcode() == UO_LNot) {
        BoolKnowledge Result = Self(Self, U->getSubExpr());

        if (Result == BoolKnowledge::True)
          return BoolKnowledge::False;

        if (Result == BoolKnowledge::False)
          return BoolKnowledge::True;

        return BoolKnowledge::Unknown;
      }
    }

    if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
      if (BO->getOpcode() == BO_LAnd || BO->getOpcode() == BO_LOr) {
        BoolKnowledge L = Self(Self, BO->getLHS());
        BoolKnowledge R = Self(Self, BO->getRHS());

        if (BO->getOpcode() == BO_LAnd) {
          if (L == BoolKnowledge::False || R == BoolKnowledge::False)
            return BoolKnowledge::False;

          if (L == BoolKnowledge::True && R == BoolKnowledge::True)
            return BoolKnowledge::True;

          return BoolKnowledge::Unknown;
        }

        if (BO->getOpcode() == BO_LOr) {
          if (L == BoolKnowledge::True || R == BoolKnowledge::True)
            return BoolKnowledge::True;

          if (L == BoolKnowledge::False && R == BoolKnowledge::False)
            return BoolKnowledge::False;

          return BoolKnowledge::Unknown;
        }
      }

      if (BO->getOpcode() == BO_EQ || BO->getOpcode() == BO_NE) {
        BoolKnowledge L = Self(Self, BO->getLHS());
        BoolKnowledge R = Self(Self, BO->getRHS());

        if (L != BoolKnowledge::Unknown && R != BoolKnowledge::Unknown) {
          bool Equal = (L == R);

          if (BO->getOpcode() == BO_EQ) {
            return Equal ? BoolKnowledge::True : BoolKnowledge::False;
          }

          return Equal ? BoolKnowledge::False : BoolKnowledge::True;
        }
      }
    }

    Value *V = Env.getValue(*E);
    if (!V) {
      if (const auto *Ref = dyn_cast<DeclRefExpr>(E))
        V = Env.getValue(*Ref->getDecl());
    }

    if (const auto *BV = dyn_cast_or_null<BoolValue>(V)) {
      if (Env.proves(BV->formula()))
        return BoolKnowledge::True;

      if (Env.proves(Env.arena().makeNot(BV->formula())))
        return BoolKnowledge::False;
    }

    return BoolKnowledge::Unknown;
  };

  for (auto It = IfStatements.rbegin(); It != IfStatements.rend(); ++It) {
    const IfStmt *If = *It;
    const Stmt *Then = If->getThen();
    const Stmt *Else = If->getElse();
    const bool InThen = isInside(Then, S);
    const bool InElse = isInside(Else, S);
    const bool InCondition = isInside(If->getCond(), S);

    if (!InThen && !InElse && !InCondition)
      continue;

    // Only evaluate the condition when the current CFG statement
    // belongs to this IfStmt's condition.
    if (!InCondition)
      continue;

    // Process only the innermost enclosing if statement.
    bool IsInsideInnerIf = false;

    for (auto InnerIt = IfStatements.rbegin(); InnerIt != It; ++InnerIt) {
      const IfStmt *InnerIf = *InnerIt;

      if (isInside(InnerIf->getCond(), S) || isInside(InnerIf->getThen(), S) ||
          isInside(InnerIf->getElse(), S)) {

        IsInsideInnerIf = true;
        break;
      }
    }

    if (IsInsideInnerIf)
      continue;

    if (!Env.allows(Env.arena().makeLiteral(true)))
      continue;

    const Expr *Condition = If->getCond();
    BoolKnowledge ConditionResult = EvaluateBool(EvaluateBool, Condition);
    const bool ConditionTrue = ConditionResult == BoolKnowledge::True;
    const bool ConditionFalse = ConditionResult == BoolKnowledge::False;

    if (ConditionTrue && Else) {
      return {{Else->getBeginLoc(), Diagnoser::DiagnosticType::Unreachable}};
    }

    if (ConditionFalse) {
      return {{Then->getBeginLoc(), Diagnoser::DiagnosticType::Unreachable}};
    }
  }

  return {};
}
} // namespace

void DeadCodeModel::transferBranch(bool Branch, const Stmt *S,
                                   NoopLattice &State, Environment &Env) {}

DeadCodeDiagnoser::DeadCodeDiagnoser()
    : DiagnoseMatchSwitch(
          CFGMatchSwitchBuilder<const Environment, ResultType>()
              .CaseOfCFGStmt<Stmt>(
                  conditionMatcher(),
                  [this](const Stmt *S, const MatchFinder::MatchResult &Result,
                         const Environment &Env) {
                    return diagnoseAnyDeadCondition(S, Result, Env,
                                                    IfStatements);
                  })
              .Build()) {}

DeadCodeDiagnoser::ResultType DeadCodeDiagnoser::operator()(
    const CFGElement &Elt, ASTContext &Ctx,
    const TransferStateForDiagnostics<NoopLattice> &State) {

  // Collect IfStmt nodes only once for each ASTContext.
  if (CachedContext != &Ctx) {
    IfStatements.clear();
    IfStmtCollector Collector(Ctx);

    Collector.TraverseDecl(Ctx.getTranslationUnitDecl());

    IfStatements.append(Collector.IfStatements.begin(),
                        Collector.IfStatements.end());

    CachedContext = &Ctx;
  }

  return DiagnoseMatchSwitch(Elt, Ctx, State.Env);
}
} // namespace clang::dataflow
