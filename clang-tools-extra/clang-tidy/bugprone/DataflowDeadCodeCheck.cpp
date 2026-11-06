//===--- DataflowDeadCodeCheck.cpp - clang-tidy-------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DataflowDeadCodeCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Analysis/FlowSensitive/Models/DeadCodeModel.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <cassert>
#include <optional>
#include <utility>

namespace clang::tidy::bugprone {

using ast_matchers::MatchFinder;
using dataflow::DeadCodeDiagnoser;
using dataflow::DeadCodeModel;
using Diagnoser = DeadCodeDiagnoser;
using Diagnostic = DeadCodeDiagnoser::DiagnosticEntry;
using ResultType = Diagnoser::ResultType;

static constexpr llvm::StringLiteral FuncID("fun");

static std::optional<ResultType> analyzeFunction(const FunctionDecl &FuncDecl) {
  ASTContext &ASTCtx = FuncDecl.getASTContext();

  if (!FuncDecl.doesThisDeclarationHaveABody()) {
    return std::nullopt;
  }

  Diagnoser DiagnoserInstance;
  ResultType Diagnostics;

  clang::dataflow::DiagnosisCallbacks<DeadCodeModel, Diagnostic> Callbacks = {
    nullptr,
    DiagnoserInstance
  };

  if (llvm::Error E =
          dataflow::diagnoseFunction<DeadCodeModel, Diagnostic>(
              FuncDecl, ASTCtx, Callbacks)
              .moveInto(Diagnostics)) {
    llvm::dbgs() << "Dataflow analysis failed: " << llvm::toString(std::move(E))
                 << ".\n";
    return std::nullopt;
  }

  return Diagnostics;
}

void DataflowDeadCodeCheck::registerMatchers(MatchFinder *Finder) {
  using namespace ast_matchers;
  Finder->addMatcher(
      decl(
          anyOf(functionDecl(unless(isExpansionInSystemHeader()),
                             // FIXME: Remove the filter below when lambdas are
                             // well supported by the check.
                             unless(hasDeclContext(cxxRecordDecl(isLambda())))),
                cxxConstructorDecl(
                    unless(hasDeclContext(cxxRecordDecl(isLambda()))))))
          .bind(FuncID),
      this);
}

void DataflowDeadCodeCheck::check(const MatchFinder::MatchResult &Result) {
  if (Result.SourceManager->getDiagnostics().hasUncompilableErrorOccurred())
    return;

  const auto *FuncDecl = Result.Nodes.getNodeAs<FunctionDecl>(FuncID);
  assert(FuncDecl && "invalid FuncDecl matcher");
  if (FuncDecl->isImplicit()
    || Result.SourceManager->isInSystemHeader(FuncDecl->getLocation())
    || FuncDecl->isTemplated())
    return;

  if (const auto Diagnostics = analyzeFunction(*FuncDecl)) {
    for (const auto &[Loc, Type] : *Diagnostics) {
      switch (Type) {
      case Diagnoser::DiagnosticType::Unreachable:
        diag(Loc, "unreachable code");
        break;
      }
    }
  }
}

} // namespace clang::tidy::bugprone
