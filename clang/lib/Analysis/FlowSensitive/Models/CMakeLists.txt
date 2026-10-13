add_clang_library(clangAnalysisFlowSensitiveModels
  ChromiumCheckModel.cpp
  NullPointerAnalysisModel.cpp
  DeadCodeModel.cpp
  UncheckedOptionalAccessModel.cpp
  UncheckedStatusOrAccessModel.cpp
  GtestModelHelpers.cpp

  LINK_LIBS
  clangAnalysis
  clangAnalysisFlowSensitive
  clangAST
  clangASTMatchers
  clangBasic
  )
