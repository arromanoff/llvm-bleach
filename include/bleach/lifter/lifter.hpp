#pragma once

#include "bleach/lifter/instr-impl.hpp"

#include "mctomir/mctomir-transform.h"
#include "mctomir/symbols.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/Regex.h>
#include <llvm/Target/TargetMachine.h>

#include <iostream>
#include <map>
#include <set>
#include <unordered_set>

namespace llvm {
class Module;
class MachineModuleInfo;
class MachineFunction;
class MachineBasicBlock;
class Function;
class BasicBlock;
} // namespace llvm

namespace bleach::lifter {
using namespace llvm;
class reg2vals final : private std::map<unsigned, Value *> {
public:
  using map::at;
  using map::begin;
  using map::contains;
  using map::end;
  using map::try_emplace;
  using map::operator[];
  void dump() const {
    for (auto &[reg, val] : *this) {
      errs() << "REG: " << reg << " val: " << *val << "\n";
    }
  }
};

class mbb2bb final
    : private std::unordered_map<const MachineBasicBlock *, BasicBlock *> {
public:
  using unordered_map::begin;
  using unordered_map::empty;
  using unordered_map::end;
  using unordered_map::erase;
  using unordered_map::insert;
  using unordered_map::size;
  auto operator[](const MachineBasicBlock *mbb) const {
    auto found = find(mbb);
    if (found == end()) {
      throw std::invalid_argument("Attempt to get Basic Block for unknown MBB");
    }
    return found->second;
  }
};

class register_class final : private std::map<unsigned, std::vector<unsigned>> {
  std::string name;
  llvm::Regex regex;
  const TargetRegisterClass *rclass = nullptr;
  unsigned reg_bitsize = 0;

public:
  using map::at;
  using map::begin;
  using map::contains;
  using map::empty;
  using map::end;
  using map::insert;
  using map::size;

  register_class(std::string &&rcname, llvm::Regex &&rx)
      : name(std::move(rcname)), regex(std::move(rx)) {}

  void add_reg(unsigned reg) { try_emplace(reg); }

  auto &get_regex() const & { return regex; }

  std::string_view get_name() const & { return name; }

  auto *get_target_regclass() const { return rclass; }

  void set_rclass(const TargetRegisterClass *rcl, unsigned regsize) {
    rclass = rcl;
    reg_bitsize = regsize;
  }

  unsigned get_register_size() const { return reg_bitsize; }
};

class register_stats final : std::vector<register_class> {
public:
  using vector::begin;
  using vector::empty;
  using vector::end;
  using vector::erase;
  using vector::size;
  using vector::operator[];

  template <typename It> register_stats(It start, It finish) {
    for (; start != finish; ++start)
      vector::emplace_back(std::string(start->name), Regex(start->regex));
  }

  auto &get_register_class_for(unsigned reg) const {
    auto found = ranges::find_if(*this, [reg](auto &rclass) {
      if (rclass.contains(reg))
        return true;
      auto subreg = ranges::find_if(rclass, [reg](auto &entry) {
        return is_contained(entry.second, reg);
      });
      return subreg != rclass.end();
    });
    if (found == end()) {
      throw std::invalid_argument("Unknown register encountered");
    }
    return *found;
  }

  auto *get_target_register_class_for(unsigned reg) const {
    return get_register_class_for(reg).get_target_regclass();
  }
};

class stack_pointer_tracker final : private std::unordered_set<unsigned> {
public:
  stack_pointer_tracker(unsigned sp) { add(sp); }

  void add(unsigned sp) { unordered_set::insert(sp); }

  bool is_stack_pointer(unsigned reg) const {
    return unordered_set::contains(reg);
  }

  using unordered_set::begin;
  using unordered_set::contains;
  using unordered_set::empty;
  using unordered_set::end;
  using unordered_set::erase;
  using unordered_set::size;
};

void fill_ir_for_bb(MachineBasicBlock &mbb,
                    const mctomir::translated_block *trbinfo, reg2vals &rmap,
                    const instr_impl &instrs, const TargetMachine &tm,
                    const mbb2bb &m2b, StructType &state,
                    const register_stats &reg_stats,
                    stack_pointer_tracker &sptrack, bool functions_nop);

struct basic_block {
  MachineBasicBlock *mbb;
  BasicBlock *bb;
};

void copy_instructions(const MachineBasicBlock &src, MachineBasicBlock &dst);

basic_block clone_basic_block(MachineBasicBlock &src, MachineFunction &dst);

StructType &create_state_type(LLVMContext &ctx);

Module &bleach_module(Module &m, MachineModuleInfo &mmi,
                      std::span<mctomir::translated_function> trfinfo,
                      const instr_impl &instrs,
                      std::string_view state_struct_file, size_t stack_size,
                      const mctomir::file_info *finfo,
                      std::string_view lifted_prefix, bool assume_functions_nop,
                      std::span<std::byte> rodata,
                      std::optional<uint64_t> rodata_start);

} // namespace bleach::lifter
