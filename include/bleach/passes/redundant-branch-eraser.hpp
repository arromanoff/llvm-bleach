#pragma once

#include <llvm/IR/Analysis.h>
#include <llvm/IR/PassManager.h>

namespace bleach::lifter {
using namespace llvm;

class redundant_branch_eraser : public OptionalPassInfoMixin<redundant_branch_eraser> {
public:
  redundant_branch_eraser() = default;
  PreservedAnalyses run(Function &m, FunctionAnalysisManager &fam);
};
} // namespace bleach::lifter
