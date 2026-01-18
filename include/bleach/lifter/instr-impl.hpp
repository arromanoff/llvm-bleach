#pragma once

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnull-dereference"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"
#else
#endif
#include <llvm/CodeGen/TargetRegisterInfo.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#else
#endif
#include <vector>

namespace bleach::lifter {
using namespace llvm;

struct return_info final : private std::vector<std::string> {
  using vector::begin;
  using vector::empty;
  using vector::end;
  using vector::size;

  void add(const std::string &rx) { vector::push_back(rx); }
};

struct instruction final {
  std::string name;
  std::unique_ptr<Module> ir_module;
  std::optional<return_info> retinfo;
  bool is_pc_relative = false;
  bool is_indirect_branch = false;
};

struct constant_reg final {
  std::string name;
  uint64_t value;
};

struct regclass final {
  std::string name;
  std::string regex;
};

// @class instr_impl
// @brief map from instr opcode to llvm ir impl
class instr_impl final : private std::vector<instruction> {
  std::string stack_pointer;
  std::vector<constant_reg> const_regs;
  std::vector<regclass> regclasses;
  std::unordered_map<std::string, std::string> subregisters;

public:
  instr_impl() = default;
  using vector::at;
  using vector::begin;
  using vector::empty;
  using vector::end;
  using vector::size;
  using vector::operator[];
  using vector::emplace_back;
  using vector::push_back;

  StringRef get_stack_pointer() const & { return stack_pointer; }

  auto &get_const_regs() & { return const_regs; }
  auto &get_const_regs() const & { return const_regs; }

  auto &get_regclasses() const & { return regclasses; }
  auto &get_regclasses() & { return regclasses; }

  auto &get_subregs() const & { return subregisters; }
  auto &get_subregs() & { return subregisters; }

  void set_stack_pointer(std::string sp) { stack_pointer = std::move(sp); }

  auto find(std::string_view name) const & {
    auto found = std::find_if(begin(), end(),
                              [name](auto &pair) { return pair.name == name; });
    return found;
  }
};

instr_impl load_from_yaml(std::string, LLVMContext &ctx);

std::string save_to_yaml(const instr_impl &);

} // namespace bleach::lifter
