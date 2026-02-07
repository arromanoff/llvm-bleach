#pragma once

#include "bleach/lifter/instr-impl.hpp"
#include "mctomir/mctomir-transform.h"
#include "mctomir/symbols.h"

#include <llvm/IR/PassManager.h>

namespace bleach::lifter {
using namespace llvm;

class block_ir_builder_pass : public PassInfoMixin<block_ir_builder_pass> {
  const instr_impl &instrs;
  std::vector<mctomir::translated_function> funcs;
  std::vector<mctomir::section_info> sections;
  std::string state_struct_file;
  const mctomir::file_info *finfo;
  unsigned stack_size;
  std::string lifted_prefix;
  bool assume_functions_nop;

public:
  block_ir_builder_pass(const instr_impl &insts,
                        std::vector<mctomir::translated_function> &&fs,
                        bool functions_nop, std::string_view state_file,
                        const mctomir::file_info *file_info, unsigned stack_sz,
                        std::string_view prefix,
                        std::vector<mctomir::section_info> &&secs)
      : instrs(insts), funcs(std::move(fs)), sections(std::move(secs)),
        state_struct_file(state_file), finfo(file_info), stack_size(stack_sz),
        lifted_prefix(prefix), assume_functions_nop(functions_nop) {}

  PreservedAnalyses run(Module &m, ModuleAnalysisManager &mam);
};

} // namespace bleach::lifter
