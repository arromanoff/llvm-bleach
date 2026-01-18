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
  std::string state_struct_file;
  std::vector<std::byte> rodata;
  const mctomir::file_info *finfo;
  unsigned stack_size;
  std::string lifted_prefix;
  uint64_t rodata_start;
  bool assume_functions_nop;

public:
  block_ir_builder_pass(const instr_impl &insts,
                        std::vector<mctomir::translated_function> &&fs,
                        bool functions_nop, std::string_view state_file,
                        const mctomir::file_info *file_info, unsigned stack_sz,
                        std::string_view prefix, std::vector<std::byte> &&rod,
                        uint64_t rod_start)
      : instrs(insts), funcs(std::move(fs)), state_struct_file(state_file),
        rodata(rod), finfo(file_info), stack_size(stack_sz),
        lifted_prefix(prefix), rodata_start(rod_start),
        assume_functions_nop(functions_nop) {}

  PreservedAnalyses run(Module &m, ModuleAnalysisManager &mam);
};

} // namespace bleach::lifter
