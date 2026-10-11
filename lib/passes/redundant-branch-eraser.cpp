#include "bleach/passes/redundant-branch-eraser.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnull-dereference"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"
#endif

#include <llvm/IR/Analysis.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>

namespace bleach::lifter {
using namespace llvm;
namespace ranges = std::ranges;
PreservedAnalyses redundant_branch_eraser::run(Function &f,
                                               FunctionAnalysisManager &) {
  for (auto &bb : f) {
    auto branch =
        ranges::find_if(bb, [](auto &inst) { return isa<CondBrInst>(&inst); });
    if (branch != bb.end())
      bb.erase(std::next(branch), bb.end());
  }
  return PreservedAnalyses::none();
}
} // namespace bleach::lifter
