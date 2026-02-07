#include "bleach/passes/block-ir-builder.hpp"
#include "bleach/lifter/lifter.hpp"

#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/MachineRegisterInfo.h>
#include <llvm/CodeGen/TargetInstrInfo.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Linker/Linker.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <expected>
#include <format>
#include <fstream>
#include <iostream>
#include <ranges>
#include <set>

namespace bleach::lifter {
namespace ranges = std::ranges;
namespace views = std::views;

PreservedAnalyses block_ir_builder_pass::run(Module &m,
                                             ModuleAnalysisManager &mam) {
  auto &mmi = mam.getResult<MachineModuleAnalysis>(m).getMMI();
  bleach_module(m, mmi, funcs, instrs, state_struct_file, stack_size, finfo,
                lifted_prefix, assume_functions_nop, sections);
  return PreservedAnalyses::none();
}

} // namespace bleach::lifter
