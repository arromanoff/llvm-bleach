#include "bleach/lifter/lifter.hpp"
#include "bleach/lifter/instr-impl.hpp"
#include "mctomir/symbols.h"

#include "symtab-ir.h"

#include <algorithm>
#include <iterator>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/MachineRegisterInfo.h>
#include <llvm/CodeGen/TargetInstrInfo.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/MC/MCInstrAnalysis.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <expected>
#include <format>
#include <fstream>
#include <iostream>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>

namespace bleach::lifter {
namespace ranges = std::ranges;
namespace views = std::views;

void copy_instructions(const MachineBasicBlock &src, MachineBasicBlock &dst) {
  auto *mf = dst.getParent();
  assert(mf);
  auto *instr_info = mf->getSubtarget().getInstrInfo();
  assert(instr_info);
  for (auto &instr : src.instrs() | views::filter([](auto &instr) {
                       return !instr.isBundledWithPred();
                     })) {
    auto &instr_copy = instr_info->duplicate(dst, dst.end(), instr);
    if (instr_copy.isBranch())
      for (auto &op : instr_copy.operands())
        if (op.isMBB() && op.getMBB() == &src)
          op.setMBB(&dst);
  }
}

basic_block clone_basic_block(MachineBasicBlock &src, MachineFunction &dst) {
  auto &func = dst.getFunction();
  auto *new_block = BasicBlock::Create(func.getContext(), "", &func);
  assert(new_block);
  // Dummy basic block is necessary due to bug in
  // MachineFunction::CreateMachineBasicBlock which dereferences nullptr if BB
  // does not contain terminator.
  // FIXME: remove after fix in upstream
  auto *dummy_bb = BasicBlock::Create(func.getContext(), "dummy", &func);
  IRBuilder builder(func.getContext());
  builder.SetInsertPoint(new_block);
  builder.CreateBr(dummy_bb);

  auto *new_mblock = dst.CreateMachineBasicBlock(new_block);
  assert(new_mblock);
  dst.push_back(new_mblock);
  copy_instructions(src, *new_mblock);

  for (auto it = src.succ_begin(); it != src.succ_end(); ++it)
    new_mblock->copySuccessor(&src, it);
  // remove dummy instruction
  assert(!new_block->empty());
  new_block->begin()->eraseFromParent();
  dummy_bb->eraseFromParent();
  return {new_mblock, new_block};
}

// Cannot use MachineBasicBlock::ReplaceUsesOfBlockWith because it only replaces
// the uses of block in Terminate instructions and for some reason
// RISCV::JAL/RISCV::JALR are not marked as terminators
static void replace_uses_of_block_with(MachineBasicBlock &mbb,
                                       MachineBasicBlock &old_block,
                                       MachineBasicBlock &new_block) {
  for (auto &instr : mbb) {
    for (auto &op : instr.operands()) {
      if (op.isMBB() && op.getMBB() == &old_block)
        op.setMBB(&new_block);
    }
  }
  mbb.replaceSuccessor(&old_block, &new_block);
}

void create_basic_blocks_for_mfunc(MachineFunction &src, MachineFunction &dst,
                                   mbb2bb &m2b) {
  std::unordered_map<MachineBasicBlock *, MachineBasicBlock *> block_map;
  // One cannot simply loop over mfunc as we insert new blocks into it
  for (auto &mbb : src) {
    assert(all_of(mbb.successors(),
                  [&](auto *succ) { return succ->getParent() == &src; }));
    auto [new_mblock, new_block] = clone_basic_block(mbb, dst);
    m2b.insert({new_mblock, new_block});
    block_map.try_emplace(&mbb, new_mblock);
  }
  for (auto &mbb : dst) {
    for (auto *succ : mbb.successors()) {
      replace_uses_of_block_with(mbb, *succ, *block_map.at(succ));
    }
  }
  for (auto &block : dst.getFunction()) {
    for (auto &inst : block)
      if (auto *call = dyn_cast<CallInst>(&inst)) {
        if (auto *callee = call->getCalledFunction();
            callee && callee->getName() == dst.getName()) {
          call->setCalledFunction(&dst.getFunction());
        }
      }
  }
  assert(ranges::all_of(dst, [](auto &mbb) {
    return ranges::all_of(mbb.successors(), [&mbb](auto *succ) {
      return succ->getParent() == mbb.getParent();
    });
  }));
}

void fill_module_with_instrs(Module &m, const instr_impl &instrs) {
  assert(!instrs.empty());
  auto first = CloneModule(*instrs.begin()->ir_module);
  for (auto &&i : drop_begin(instrs)) {
    if (!i.ir_module)
      continue;
    auto module_clone = CloneModule(*i.ir_module);
    Linker::linkModules(*first, std::move(module_clone));
  }
  Linker::linkModules(m, std::move(first));
}

auto get_current_state(Function &func) -> Value * {
  constexpr auto gpr_array_idx = 0u;
  return func.getArg(gpr_array_idx);
}

void materialize_registers(MachineFunction &mf, Function &func, reg2vals &rmap,
                           StructType &state, const register_stats &reg_stats) {
  if (mf.empty())
    return;
  auto &ctx = func.getContext();
  assert(!func.empty());
  auto &first_block = func.front();
  auto builder = IRBuilder(ctx);
  builder.SetInsertPoint(&first_block);
  // pointer to a single state
  auto *const_zero = ConstantInt::get(ctx, APInt(32, 0));
  auto *state_arg = get_current_state(func);
  for (auto &&[idx, rclass] : reg_stats | views::enumerate) {
    auto *array_type = state.getElementType(idx);
    auto *arr_idx = ConstantInt::get(ctx, APInt(32, idx));
    auto *array_ptr = builder.CreateGEP(&state, state_arg,
                                        ArrayRef<Value *>{const_zero, arr_idx},
                                        rclass.get_name());
    auto sorted_regs = std::set<unsigned>();
    ranges::transform(rclass, std::inserter(sorted_regs, sorted_regs.end()),
                      [](auto &&entry) { return entry.first; });
    for (auto &&[reg_idx, reg] : sorted_regs | views::enumerate) {
      auto *array_reg_idx = ConstantInt::get(ctx, APInt(32, reg_idx));
      auto *reg_src_addr = builder.CreateInBoundsGEP(
          array_type, array_ptr, ArrayRef<Value *>{const_zero, array_reg_idx});
      auto *reg_dst_addr =
          builder.CreateAlloca(Type::getIntNTy(ctx, rclass.get_register_size()),
                               /*array size*/ nullptr);

      auto *reg_val = builder.CreateLoad(
          Type::getIntNTy(ctx, rclass.get_register_size()), reg_src_addr);
      builder.CreateStore(reg_val, reg_dst_addr);
      rmap.try_emplace(reg, reg_dst_addr);
      auto &&subregs = rclass.at(reg);
      ranges::for_each(subregs, [&](std::integral auto r) {
        rmap.try_emplace(r, reg_dst_addr);
      });
    }
  }
}

static std::string bleach_symtab_lookup_name = "bleach_symtab_lookup";
static std::string bleach_symtab_add_name = "bleach_symtab_add";

auto *create_bleach_symtab_add_function_decl(Module &m) {
  auto &ctx = m.getContext();
  auto *ret_type = Type::getVoidTy(ctx);
  auto *ptr_type = PointerType::getUnqual(ctx);
  auto *addr_type = Type::getInt64Ty(ctx);
  auto *ftype =
      FunctionType::get(ret_type, ArrayRef<Type *>{addr_type, ptr_type}, false);
  return Function::Create(ftype, Function::ExternalLinkage,
                          bleach_symtab_add_name, m);
}

auto *create_bleach_symtab_lookup_function_decl(Module &m) {
  auto &ctx = m.getContext();
  auto *ptr_type = PointerType::getUnqual(ctx);
  auto *addr_type = Type::getInt64Ty(ctx);
  auto *ftype = FunctionType::get(ptr_type, ArrayRef<Type *>{addr_type}, false);
  return Function::Create(ftype, Function::ExternalLinkage,
                          bleach_symtab_lookup_name, m);
}
auto *generate_function_object(Module &m, MachineFunction &mf) {
  auto *ret_type = mf.getFunction().getFunctionType()->getReturnType();
  ret_type = Type::getVoidTy(m.getContext());
  auto *func_type = FunctionType::get(
      ret_type, ArrayRef<Type *>{PointerType::getUnqual(m.getContext())},
      /* is var arg */ false);
  auto name_str = mf.getName().str();
  mf.getFunction().setName(mf.getName().str() + ".old");
  auto *func =
      Function::Create(func_type, Function::ExternalLinkage, name_str, m);
  return func;
}

void save_registers(BasicBlock &block, BasicBlock::iterator pos, reg2vals &rmap,
                    const TargetMachine &tmachine, StructType &state,
                    const register_stats &reg_stats) {
  auto &ctx = block.getContext();
  auto *stinfo = tmachine.getSubtargetImpl(*block.getParent());
  assert(stinfo);
  auto *rinfo = stinfo->getRegisterInfo();
  assert(rinfo);
  IRBuilder builder(block.getContext());
  if (pos == block.end())
    builder.SetInsertPoint(&block);
  else
    builder.SetInsertPoint(pos);
  auto *const_zero = ConstantInt::get(ctx, APInt(32, 0));
  auto *state_arg = get_current_state(*block.getParent());
  for (auto &&[idx, rclass] : reg_stats | views::enumerate) {
    auto *array_type = state.getElementType(idx);
    auto *arr_idx = ConstantInt::get(ctx, APInt(32, idx));
    auto *array_ptr = builder.CreateGEP(&state, state_arg,
                                        ArrayRef<Value *>{const_zero, arr_idx},
                                        rclass.get_name());
    for (auto &&[reg_idx, val] : rmap | views::filter([&rclass](auto r) {
                                   return rclass.contains(r.first);
                                 }) | views::enumerate) {
      auto *array_idx = ConstantInt::get(ctx, APInt(32, reg_idx));
      auto &&[reg, addr] = val;
      auto *reg_dst_addr = builder.CreateInBoundsGEP(
          array_type, array_ptr, ArrayRef<Value *>{const_zero, array_idx});
      auto *reg_val = builder.CreateLoad(
          Type::getIntNTy(ctx, rclass.get_register_size()), addr);
      builder.CreateStore(reg_val, reg_dst_addr);
    }
  }
}

void load_registers(BasicBlock &block, BasicBlock::iterator pos, reg2vals &rmap,
                    const TargetMachine &tmachine, StructType &state,
                    const register_stats &reg_stats) {
  auto &ctx = block.getContext();
  auto *stinfo = tmachine.getSubtargetImpl(*block.getParent());
  assert(stinfo);
  auto *rinfo = stinfo->getRegisterInfo();
  assert(rinfo);
  IRBuilder builder(block.getContext());
  if (pos == block.end())
    builder.SetInsertPoint(&block);
  else
    builder.SetInsertPoint(pos);
  auto *const_zero = ConstantInt::get(ctx, APInt(32, 0));
  auto *state_arg = get_current_state(*block.getParent());
  for (auto &&[idx, rclass] : reg_stats | views::enumerate) {
    auto *array_type = state.getElementType(idx);
    auto *arr_idx = ConstantInt::get(ctx, APInt(32, idx));
    auto *array_ptr = builder.CreateGEP(&state, state_arg,
                                        ArrayRef<Value *>{const_zero, arr_idx},
                                        rclass.get_name());
    for (auto &&[reg_idx, val] : rmap | views::filter([&rclass](auto r) {
                                   return rclass.contains(r.first);
                                 }) | views::enumerate) {
      auto *array_idx = ConstantInt::get(ctx, APInt(32, reg_idx));
      auto &&[reg, addr] = val;
      auto *reg_src_addr = builder.CreateInBoundsGEP(
          array_type, array_ptr, ArrayRef<Value *>{const_zero, array_idx});
      auto *reg_val = builder.CreateLoad(
          Type::getIntNTy(ctx, rclass.get_register_size()), reg_src_addr);
      builder.CreateStore(reg_val, addr);
    }
  }
}

void save_registers_before_return(Function &func, reg2vals &rmap,
                                  const TargetMachine &tmachine,
                                  StructType &state,
                                  const register_stats &reg_stats) {
  for (auto &block : func) {
    auto ret = ranges::find_if(
        block, [](auto &inst) { return isa<ReturnInst>(inst); });
    if (ret == block.end())
      continue;
    save_registers(block, ret, rmap, tmachine, state, reg_stats);
  }
}

void fill_symtab_at(Function *func, ArrayRef<Function *> funcs,
                    const mctomir::file_info &finfo) {
  auto *m = func->getParent();
  auto *symtab_add = m->getFunction(bleach_symtab_add_name);
  assert(symtab_add);
  for (auto *callee :
       funcs | views::filter([](auto *f) { return !f->empty(); })) {
    auto &front_bb = func->front();
    auto &ctx = func->getContext();
    IRBuilder builder(ctx);
    builder.SetInsertPoint(front_bb.begin());
    auto *addr = ConstantInt::get(
        ctx, APInt(64, finfo.get_start_of(callee->getName().str())));
    std::vector<Value *> args = {addr, callee};
    builder.CreateCall(symtab_add->getFunctionType(), symtab_add, args);
  }
}

std::optional<unsigned> find_register_by_name(const MCRegisterInfo *reg_info,
                                              StringRef name) {
  for (auto &&rclass : reg_info->regclasses()) {
    auto reg_it = ranges::find_if(
        rclass, [&](auto &reg) { return name == reg_info->getName(reg); });
    if (reg_it != rclass.end())
      return *reg_it;
  }
  return std::nullopt;
}
struct clone_function_result final {
  mbb2bb blocks;
  MachineFunction *mfunc = nullptr;
};

auto generate_function(MachineFunction &mf, clone_function_result &dst_info,
                       const instr_impl &instrs, MachineModuleInfo &mmi,
                       StructType &state, const register_stats &reg_stats,
                       bool functions_nop) {
  reg2vals rmap;
  auto &func = dst_info.mfunc->getFunction();
  materialize_registers(mf, func, rmap, state, reg_stats);
  auto &tmachine = mmi.getTarget();
  auto *reg_info = tmachine.getMCRegisterInfo();
  auto sp_reg = find_register_by_name(reg_info, instrs.get_stack_pointer());
  if (!sp_reg)
    throw std::runtime_error("Could not find stack pointer under the name \"" +
                             instrs.get_stack_pointer().str() + "\"");
  stack_pointer_tracker sptrack(*sp_reg);
  for (auto &mbb : *dst_info.mfunc)
    fill_ir_for_bb(mbb, rmap, instrs, tmachine, dst_info.blocks, state,
                   reg_stats, sptrack, functions_nop);
  save_registers_before_return(func, rmap, tmachine, state, reg_stats);
}

std::string get_instruction_name(const MachineInstr &minst,
                                 const MCInstrInfo &instr_info) {
  return instr_info.getName(minst.getOpcode()).str();
}

StructType &create_state_type(LLVMContext &ctx, const register_stats &stats,
                              size_t stack_size_bytes) {

  std::vector<Type *> members;
  ranges::transform(stats, std::back_inserter(members), [&ctx](auto &rclass) {
    auto num_regs = rclass.size();
    auto reg_size = rclass.get_register_size();
    return ArrayType::get(Type::getIntNTy(ctx, reg_size), num_regs);
  });
  auto largest_regsize =
      ranges::max_element(stats, ranges::less{}, [](auto &rclass) {
        return rclass.get_register_size();
      });
  assert(largest_regsize != stats.end() && "Empty regstats");
  auto largest_regsize_bytes = largest_regsize->get_register_size() / CHAR_BIT;
  members.push_back(
      ArrayType::get(Type::getIntNTy(ctx, largest_regsize->get_register_size()),
                     stack_size_bytes / largest_regsize_bytes));
  auto *struct_type = StructType::create(ctx, members, "register_state");
  assert(struct_type);
  return *struct_type;
}

auto clone_machine_function(Module &m, MachineModuleInfo &mmi,
                            MachineFunction &mfunc) {
  auto *new_func = generate_function_object(m, mfunc);
  assert(new_func);
  auto &new_mfunc = mmi.getOrCreateMachineFunction(*new_func);
  clone_function_result res = {{}, &new_mfunc};
  create_basic_blocks_for_mfunc(mfunc, new_mfunc, res.blocks);
  // mfunc.getFunction().eraseFromParent();
  return res;
}

void collect_register_stats_for(const MachineFunction &mf,
                                const TargetMachine &tmachine,
                                register_stats &stats) {
  auto *stinfo = tmachine.getSubtargetImpl(mf.getFunction());
  assert(stinfo);
  auto *rinfo = stinfo->getRegisterInfo();
  assert(rinfo);
  for (auto &mblock : mf) {
    for (auto &minst : mblock) {
      for (auto &mop : minst.operands()) {
        if (mop.isReg()) {
          StringRef rname = rinfo->getRegAsmName(mop.getReg());
          auto matching = ranges::find_if(stats, [rname](auto &rclass) -> bool {
            return rclass.get_regex().match(rname);
          });
          if (matching == stats.end())
            throw std::runtime_error("No register class found to match: \"" +
                                     rname.str() + "\"");
          matching->add_reg(mop.getReg());
        }
      }
    }
  }
}

void assign_register_classes(const TargetSubtargetInfo &stinfo,
                             register_stats &stats) {
  auto *rinfo = stinfo.getRegisterInfo();
  assert(rinfo);
  for (auto &&[idx, reg_class] : enumerate(stats)) {
    auto found = find_if(rinfo->regclasses(), [&reg_class](auto &rclass) {
      return all_of(reg_class, [&rclass](auto &&reg) {
        return is_contained(*rclass, reg.first);
      });
    });
    if (found == rinfo->regclass_end()) {
      throw std::runtime_error(
          "Could not find llvm register class for specified register class: " +
          std::to_string(idx));
    }
    reg_class.set_rclass(*found, rinfo->getRegSizeInBits(**found));
    for (auto reg : **found) {
      if (reg_class.get_regex().sub("", rinfo->getRegAsmName(reg)).empty())
        reg_class.add_reg(reg);
    }
  }
}

register_stats collect_register_stats(const instr_impl &instr, Module &m,
                                      MachineModuleInfo &mmi) {
  auto &rclasses = instr.get_regclasses();
  if (rclasses.empty())
    throw std::runtime_error(
        "register-classes were not specified in input YAML");
  register_stats stats(rclasses.begin(), rclasses.end());
  auto *stinfo = mmi.getTarget().getSubtargetImpl(*m.begin());
  for (auto &f : m) {
    auto &mf = mmi.getOrCreateMachineFunction(f);
    collect_register_stats_for(mf, mmi.getTarget(), stats);
  }
  assert(!m.empty());
  assert(stinfo);
  assign_register_classes(*stinfo, stats);
  stats.erase(std::remove_if(stats.begin(), stats.end(),
                             [](auto &rclass) { return rclass.empty(); }),
              stats.end());
  for (auto &&[subregclass, aliasto] : instr.get_subregs()) {
    auto found = ranges::find_if(stats, [&subregclass](auto &rclass) {
      return rclass.get_name() == subregclass;
    });
    if (found != stats.end()) {
      auto found_aliasto = ranges::find_if(stats, [&aliasto](auto &rclass) {
        return rclass.get_name() == aliasto;
      });
      if (found_aliasto != stats.end()) {
        for (auto &&[entry, subreg] : views::zip(*found_aliasto, *found)) {
          auto &&[_, subregs] = entry;
          subregs.push_back(subreg.first);
        }
        stats.erase(found);
      }
    }
  }
  return stats;
}

static void print_state_struct_definition(std::ostream &os,
                                          const StructType &type,
                                          const register_stats &rstats) {
  auto elements = type.elements();
  assert(rstats.size() == elements.size() - 1);
  os << "#include <stdint.h>\n";
  os << "struct register_state {\n";
  for (auto &&[rclass, member] : views::zip(rstats, elements)) {
    auto *arr = dyn_cast<ArrayType>(member);
    assert(arr);
    auto *elem = dyn_cast<IntegerType>(arr->getElementType());
    assert(elem);
    auto n_elements = arr->getNumElements();
    assert(n_elements > 0);
    os << std::format("  int{0}_t {1}[{2}];\n", elem->getBitWidth(),
                      rclass.get_name(), n_elements);
  }
  // Last member is stack;
  assert(!elements.empty());
  auto *stack_type = elements.back();
  auto *arr = dyn_cast<ArrayType>(stack_type);
  assert(arr);
  auto *stack_elem = arr->getElementType();
  auto *elem = dyn_cast<IntegerType>(stack_elem);
  assert(elem);
  os << std::format("  int{0}_t stack[{1}];\n", elem->getBitWidth(),
                    arr->getNumElements());
  os << "};";
}

auto generate_jump(const MachineInstr &minst, IRBuilder<> &builder,
                   const mbb2bb &m2b) -> Instruction * {
  auto mbb_op =
      llvm::find_if(minst.operands(), [](auto &op) { return op.isMBB(); });
  if (minst.getIterator() != minst.getParent()->begin()) {
    if (std::prev(minst.getIterator())->isConditionalBranch())
      return nullptr;
  }
  assert(mbb_op != minst.operands_end());
  auto *mbb = mbb_op->getMBB();
  assert(mbb);
  auto *target_bb = m2b[mbb];
  assert(target_bb);
  return builder.CreateBr(target_bb);
}

auto get_if_true_block(const MachineInstr &minst, const mbb2bb &m2b)
    -> BasicBlock * {
  assert(minst.isConditionalBranch());
  auto &op = minst.getOperand(2);
  assert(op.isMBB());
  return m2b[op.getMBB()];
}

auto get_if_false_block(const MachineInstr &minst, const mbb2bb &m2b)
    -> BasicBlock * {

  assert(minst.isConditionalBranch());
  assert(minst.getNumOperands() >= 2);
  auto next = std::next(minst.getIterator());
  if (next != minst.getParent()->end()) {
    if (next->isUnconditionalBranch()) {
      auto &op = next->getOperand(0);
      assert(op.isMBB());
      return m2b[op.getMBB()];
    }
    auto *func = m2b[minst.getParent()]->getParent();
    return BasicBlock::Create(func->getContext(), "", func);
  }
  return m2b[std::addressof(*std::next(minst.getParent()->getIterator()))];
}

auto operand_to_value(const MachineOperand &mop, BasicBlock &block,
                      const TargetMachine &tmachine, reg2vals &rmap,
                      const register_stats &reg_stats, const instr_impl &insts)
    -> Value * {
  IRBuilder builder(block.getContext());
  builder.SetInsertPoint(&block);
  if (mop.isReg()) {
    auto &const_regs = insts.get_const_regs();
    auto *reg_info = tmachine.getMCRegisterInfo();
    auto found = ranges::find_if(const_regs, [&](auto &r) {
      return r.name == reg_info->getName(mop.getReg());
    });
    if (found != const_regs.end()) {
      auto val = found->value;
      return ConstantInt::get(block.getContext(), APInt(64, val));
    }
    auto *stinfo = tmachine.getSubtargetImpl(*block.getParent());
    assert(stinfo);
    auto *rinfo = stinfo->getRegisterInfo();
    assert(rinfo);
    if (mop.isDef() && mop.isImplicit())
      return nullptr;
    assert(mop.isUse());
    auto &rclass = reg_stats.get_register_class_for(mop.getReg());
    auto *reg_addr = rmap[mop.getReg()];
    auto *reg_val = builder.CreateLoad(
        Type::getIntNTy(block.getContext(), rclass.get_register_size()),
        reg_addr);
    return reg_val;
  }
  if (mop.isImm()) {
    auto val = mop.getImm();
    auto *imm = ConstantInt::get(block.getContext(), APInt(64, val));
    return imm;
  }
  throw std::runtime_error("Unsupported operand type");
}

auto generate_indirect_branch(Module &m, BasicBlock &bb,
                              const MachineInstr &minst, IRBuilder<> &builder,
                              const TargetMachine &tmachine, reg2vals &rmap,
                              StructType &state, const register_stats &rstats,
                              const instr_impl &insts) -> Instruction * {
  auto *symtab_lookup = m.getFunction(bleach_symtab_lookup_name);
  assert(symtab_lookup);
  auto op_to_val = [&](auto &mop) {
    return operand_to_value(mop, bb, tmachine, rmap, rstats, insts);
  };
  auto values = minst.uses() |
                views::filter([](auto &&mop) { return !mop.isMBB(); }) |
                views::transform(op_to_val);

  std::vector<Value *> args(values.begin(), values.end());
  // FIXME: AAAA REMOVE THIS
  args.erase(std::next(args.begin()), args.end());
  auto *addr =
      builder.CreateCall(symtab_lookup->getFunctionType(), symtab_lookup, args);
  // All lifted function have the same type. Might as well take the type of
  // the current function.
  auto *ftype = bb.getParent()->getFunctionType();
  auto *call = builder.CreateCall(
      ftype, addr, ArrayRef<Value *>{get_current_state(*bb.getParent())});
  // mark as 'musttail' because we don't want RA to be updated
  call->setTailCallKind(llvm::CallInst::TCK_MustTail);
  save_registers(bb, call->getIterator(), rmap, tmachine, state, rstats);
  load_registers(bb, std::next(call->getIterator()), rmap, tmachine, state,
                 rstats);
  return call;
}

auto generate_branch(const MachineInstr &minst, BasicBlock &bb,
                     IRBuilder<> &builder, reg2vals &rmap,
                     const TargetMachine &target_machine, const mbb2bb &m2b,
                     const register_stats &reg_stats, const instr_impl &insts)
    -> Instruction * {
  auto *iinfo = target_machine.getMCInstrInfo();
  auto name = get_instruction_name(minst, *iinfo);
  auto *m = bb.getParent()->getParent();
  auto *func = m->getFunction(name);
  if (!func) {
    std::string instr;
    raw_string_ostream ss(instr);
    minst.print(ss);
    throw std::runtime_error("Could not find \"" + name +
                             "\" in module: " + instr);
  }
  auto op_to_val = [&](auto &mop) {
    return operand_to_value(mop, bb, target_machine, rmap, reg_stats, insts);
  };
  auto values = minst.uses() |
                views::filter([](auto &&mop) { return !mop.isMBB(); }) |
                views::transform(op_to_val);
  std::vector<Value *> args(values.begin(), values.end());
  auto *cond = builder.CreateCall(func->getFunctionType(), func, args);
  auto *if_true = get_if_true_block(minst, m2b);
  auto *if_false = get_if_false_block(minst, m2b);
  auto *br = builder.CreateCondBr(cond, if_true, if_false);
  builder.SetInsertPoint(if_false);
  return br;
}

static auto write_value_to_register(Value *val, MCRegister reg,
                                    IRBuilder<> &builder, reg2vals &rmap,
                                    const instr_impl &instrs,
                                    const TargetMachine &tmachine,
                                    const register_stats &reg_stats)
    -> Value * {
  auto *reg_addr = rmap.at(reg);
  auto *reg_info = tmachine.getMCRegisterInfo();
  auto &const_regs = instrs.get_const_regs();
  auto const_regs_vals = std::unordered_map<unsigned, uint64_t>();
  ranges::transform(
      const_regs, std::inserter(const_regs_vals, const_regs_vals.end()),
      [reg_info](auto &e) -> std::pair<unsigned, uint64_t> {
        auto special_reg = find_register_by_name(reg_info, e.name);
        if (!special_reg)
          throw std::runtime_error(
              "Could not find constant register under the name \"" + e.name +
              "\"");
        return {*special_reg, e.value};
      });
  auto *stinfo =
      tmachine.getSubtargetImpl(*builder.saveIP().getBlock()->getParent());
  assert(stinfo);
  auto *rinfo = stinfo->getRegisterInfo();
  assert(rinfo);
  auto &rclass = reg_stats.get_register_class_for(reg);
  auto found = ranges::find_if(
      const_regs, [&](auto &r) { return r.name == reg_info->getName(reg); });
  if (found != const_regs.end()) {
    return builder.CreateStore(
        ConstantInt::get(val->getContext(), APInt(rclass.get_register_size(),
                                                  const_regs_vals.at(reg))),
        reg_addr);
  }
  builder.CreateStore(val, reg_addr);
  return val;
}

static auto read_register_value(MCRegister reg, const TargetMachine &tmachine,
                                IRBuilder<> &builder, reg2vals &rmap,
                                const register_stats &reg_stats) -> Value * {
  auto *reg_addr = rmap.at(reg);
  auto &ctx = builder.getContext();
  auto *stinfo =
      tmachine.getSubtargetImpl(*builder.saveIP().getBlock()->getParent());
  assert(stinfo);
  auto *rinfo = stinfo->getRegisterInfo();
  assert(rinfo);
  auto &rclass = reg_stats.get_register_class_for(reg);
  return builder.CreateLoad(Type::getIntNTy(ctx, rclass.get_register_size()),
                            reg_addr);
}

static auto generate_return(const MachineInstr &minst,
                            const TargetMachine &tmachine, IRBuilder<> &builder,
                            reg2vals &rmap, const register_stats &reg_stats,
                            const Function *func) -> Value * {
  // TODO: properly handle return operand
  // non-void case
  if (false && minst.getNumOperands() >= 1) {
    auto ret = minst.getOperand(0);
    assert(ret.isReg());
    auto *reg_val =
        read_register_value(ret.getReg(), tmachine, builder, rmap, reg_stats);
    auto *res = builder.CreateBitCast(reg_val,
                                      func->getFunctionType()->getReturnType());
    return builder.CreateRet(res);
  }
  return builder.CreateRetVoid();
}

static auto generate_call(const MachineInstr &minst, IRBuilder<> &builder,
                          BasicBlock &bb, reg2vals &rmap,
                          const TargetMachine &tmachine, StructType &state,
                          const register_stats &reg_stats, bool functions_nop)
    -> Value * {
  assert(minst.getNumOperands() >= 1);
  auto ops = minst.operands();
  auto op = ranges::find_if(ops, [](auto &o) { return o.isGlobal(); });
  assert(op != ops.end());
  auto *callee = const_cast<Function *>(dyn_cast<Function>(op->getGlobal()));
  assert(callee);
  auto *call =
      builder.CreateCall(callee->getFunctionType(), callee,
                         ArrayRef<Value *>{get_current_state(*bb.getParent())});
  save_registers(bb, call->getIterator(), rmap, tmachine, state, reg_stats);
  if (!functions_nop)
    load_registers(bb, std::next(call->getIterator()), rmap, tmachine, state,
                   reg_stats);
  return call;
}

std::optional<unsigned> get_stack_pointer(const instr_impl &instrs,
                                          const TargetMachine &tmachine) {
  auto *reg_info = tmachine.getMCRegisterInfo();
  auto sp_reg = find_register_by_name(reg_info, instrs.get_stack_pointer());
  return sp_reg;
}

auto get_stack_pointer_value(const instr_impl &instrs, reg2vals &rmap,
                             IRBuilder<> &builder,
                             const TargetMachine &tmachine,
                             const register_stats &reg_stats) -> Value * {
  auto sp_reg = get_stack_pointer(instrs, tmachine);
  if (!sp_reg)
    throw std::runtime_error("Could not find stack pointer under the name \"" +
                             instrs.get_stack_pointer().str() + "\"");
  return read_register_value(*sp_reg, tmachine, builder, rmap, reg_stats);
}

static auto generate_load_store_from_stack(
    const MachineInstr &minst, IRBuilder<> &builder, BasicBlock &bb,
    reg2vals &rmap, const instr_impl &instrs,
    const TargetMachine &target_machine, StructType &state,
    const register_stats &reg_stats) -> Value * {
  auto &ctx = bb.getContext();
  auto uses = minst.uses();
  auto sp_reg = get_stack_pointer(instrs, target_machine);
  if (!sp_reg)
    throw std::runtime_error("Could not find stack pointer under the name \"" +
                             instrs.get_stack_pointer().str() + "\"");
  auto &rclass = reg_stats.get_register_class_for(*sp_reg);
  auto reg_bitsize = rclass.get_register_size();
  auto reg_bytesize = reg_bitsize / CHAR_BIT;
  auto offset_it =
      ranges::find_if(uses, [](auto &&mop) { return mop.isImm(); });
  auto *offset = [&] -> Value * {
    if (offset_it == uses.end())
      return ConstantInt::get(ctx, APInt(64, 0));
    auto off = -offset_it->getImm() / reg_bytesize;
    return ConstantInt::get(ctx, APInt(64, off));
  }();
  auto *sp =
      get_stack_pointer_value(instrs, rmap, builder, target_machine, reg_stats);
  auto *idx = builder.CreateAdd(sp, offset);
  auto *state_arg = get_current_state(*bb.getParent());
  auto *stack_type = *std::next(state.element_begin(), reg_stats.size());
  auto *const_zero = ConstantInt::get(ctx, APInt(32, 0));
  auto *stack_addr = builder.CreateGEP(
      &state, state_arg,
      ArrayRef<Value *>{const_zero,
                        ConstantInt::get(ctx, APInt(32, reg_stats.size()))});
  auto *addr = builder.CreateInBoundsGEP(stack_type, stack_addr,
                                         ArrayRef<Value *>{const_zero, idx});
  auto *iinfo = target_machine.getMCInstrInfo();
  auto name = get_instruction_name(minst, *iinfo);
  auto *m = bb.getParent()->getParent();

  auto *inserted = [&] -> Value * {
    // Loading value from stack
    if (minst.mayLoad()) {
      if (minst.mayStore())
        throw std::runtime_error("Unsupported stack manipulation. Instruction "
                                 "can both load and store");
      auto *func = m->getFunction(name);
      if (!func) {
        std::string instr;
        raw_string_ostream ss(instr);
        minst.print(ss);
        throw std::runtime_error("Could not find \"" + name +
                                 "\" in module: " + instr);
      }
      auto *ret_type = func->getFunctionType()->getReturnType();
      return builder.CreateLoad(ret_type, addr);
    }
    assert(minst.mayStore());
    auto op_to_val = [&](auto &mop) {
      return operand_to_value(mop, bb, target_machine, rmap, reg_stats, instrs);
    };
    // Value, addr reg and offset expected
    assert(ranges::size(minst.uses()) == 3);
    // Skip source register and offset.
    auto &&value = op_to_val(*minst.uses().begin());
    return builder.CreateStore(value, addr);
  }();
  // save result to register
  if (!minst.defs().empty()) {
    builder.CreateStore(inserted, rmap.at(minst.defs().begin()->getReg()));
  }
  return inserted;
}

auto generate_stack_pointer_modification(
    const MachineInstr &minst, IRBuilder<> &builder, BasicBlock &bb,
    const TargetMachine &tmachine, reg2vals &rmap, const instr_impl &instrs,
    const register_stats &reg_stats) -> Value * {
  auto &ctx = bb.getContext();
  auto uses = minst.uses();
  auto imm = ranges::find_if(uses, [](auto &mop) { return mop.isImm(); });
  if (imm == uses.end()) {
    std::string err;
    raw_string_ostream os(err);
    os << "SP is expected to be used only as a stack pointer: \"";
    minst.print(os);
    os << "\"";
    throw std::runtime_error(err);
  }
  std::vector<Value *> args;
  ranges::transform(
      minst.uses(), std::back_inserter(args), [&](auto &mop) -> Value * {
        if (!mop.isImm() || mop.getImm() != imm->getImm())
          return operand_to_value(mop, bb, tmachine, rmap, reg_stats, instrs);
        auto val = mop.getImm();
        return ConstantInt::get(ctx, APInt(64, -val / 8));
      });
  auto *iinfo = tmachine.getMCInstrInfo();
  auto name = get_instruction_name(minst, *iinfo);
  auto *m = bb.getParent()->getParent();
  auto *func = m->getFunction(name);
  auto *call = builder.CreateCall(func->getFunctionType(), func, args);
  for (auto &def : minst.defs()) {
    assert(def.isDef());
    if (!def.isReg())
      throw std::runtime_error("Only register operands are supported");
    write_value_to_register(call, def.getReg(), builder, rmap, instrs, tmachine,
                            reg_stats);
  }
  return call;
}

// Operation is considered a stack manipulation if it is a load or store and
// its source register is stack pointer
bool is_stack_manipulation(const MachineInstr &minst,
                           stack_pointer_tracker &sptrack) {
  if (!minst.mayLoadOrStore())
    return false;
  auto operands = minst.uses();
  auto regops =
      operands | views::filter([](auto &mop) { return mop.isReg(); }) |
      views::transform([](auto &mop) -> unsigned { return mop.getReg(); });
  auto used_sp = ranges::find_if(
      regops, [&](auto reg) { return sptrack.is_stack_pointer(reg); });
  return used_sp != regops.end();
}

static bool match_return(const MachineInstr &minst, const instruction &instr,
                         const TargetMachine &tmachine) {
  if (!instr.retinfo)
    return false;
  std::vector<llvm::Regex> rxs;
  ranges::transform(*instr.retinfo, std::back_inserter(rxs),
                    [](StringRef rx) { return llvm::Regex(rx); });
  auto *mfunc = minst.getParent()->getParent();
  auto *stinfo = tmachine.getSubtargetImpl(mfunc->getFunction());
  assert(stinfo);
  auto *rinfo = stinfo->getRegisterInfo();
  assert(rinfo);
  auto ops = minst.operands();
  return ranges::all_of(views::zip(ops, rxs), [rinfo](auto &&pair) {
    auto &&[op, rx] = pair;
    auto print = [](auto &mop) {
      std::string res;
      raw_string_ostream ss(res);
      mop.print(ss);
      return res;
    };
    std::string op_dump =
        op.isReg() ? rinfo->getRegAsmName(op.getReg()).str() : print(op);
    SmallVector<StringRef> matches;
    bool match = rx.match(op_dump, &matches);
    // match whole string
    return match && matches.size() == 1 && matches[0].size() == op_dump.size();
  });
}

static bool is_return(const MachineInstr &minst, const instr_impl &instrs,
                      const TargetMachine &tmachine) {
  auto *iinfo = tmachine.getMCInstrInfo();
  auto name = get_instruction_name(minst, *iinfo);
  auto instr = instrs.find(name);
  return minst.isReturn() ||
         (instr != instrs.end() && match_return(minst, *instr, tmachine));
}

static bool is_call(const MachineInstr &minst, const TargetMachine &tmachine) {
  if (minst.isCall())
    return true;
  // TODO: create mia only once
  MCInst inst;
  inst.setOpcode(minst.getOpcode());
  std::unique_ptr<MCInstrAnalysis> mia(
      tmachine.getTarget().createMCInstrAnalysis(tmachine.getMCInstrInfo()));
  assert(mia);
  return (mia->isCall(inst) || minst.isUnconditionalBranch()) &&
         std::find_if(minst.operands_begin(), minst.operands_end(),
                      [](auto &o) { return o.isGlobal(); }) !=
             minst.operands_end();
}
static bool is_indirect_branch(const MachineInstr &minst,
                               const instr_impl &instrs,
                               const TargetMachine &tmachine) {
  if (minst.isIndirectBranch())
    return true;
  auto *iinfo = tmachine.getMCInstrInfo();
  auto name = get_instruction_name(minst, *iinfo);
  auto instr = instrs.find(name);
  return (instr != instrs.end()) && instr->is_indirect_branch;
}

static bool is_branch(const MachineInstr &minst,
                      const TargetMachine &tmachine) {
  if (minst.isBranch() &&
      std::find_if(minst.operands_begin(), minst.operands_end(),
                   [](auto &o) { return o.isMBB(); }) != minst.operands_end())
    return true;
  // TODO: create mia only once
  MCInst inst;
  inst.setOpcode(minst.getOpcode());
  std::unique_ptr<MCInstrAnalysis> mia(
      tmachine.getTarget().createMCInstrAnalysis(tmachine.getMCInstrInfo()));
  assert(mia);
  return mia->isCall(inst) && std::find_if(minst.operands_begin(),
                                           minst.operands_end(), [](auto &o) {
                                             return o.isMBB();
                                           }) != minst.operands_end();
}

static Value *cast_value_to(IRBuilder<> &builder, Value *val, Type *ty) {
  auto from_id = val->getType()->getTypeID();
  auto to_id = ty->getTypeID();
  if (from_id != Type::TypeID::IntegerTyID)
    throw std::invalid_argument(
        "currently casting is only supported for integral types");
  if (to_id != Type::TypeID::IntegerTyID)
    throw std::invalid_argument(
        "currently casting is only supported for integral types");
  auto *from_int = dyn_cast<IntegerType>(val->getType());
  auto *to_int = dyn_cast<IntegerType>(ty);
  auto from_width = from_int->getBitWidth();
  auto to_width = to_int->getBitWidth();
  if (from_width == to_width)
    return val;
  return builder.CreateSExtOrTrunc(val, ty);
}

auto generate_instruction(const MachineInstr &minst, BasicBlock &bb,
                          IRBuilder<> &builder, reg2vals &rmap,
                          const instr_impl &instrs,
                          const TargetMachine &target_machine,
                          const mbb2bb &m2b, StructType &state,
                          const register_stats &reg_stats,
                          stack_pointer_tracker &sptrack, bool functions_nop)
    -> Value * {
  auto *iinfo = target_machine.getMCInstrInfo();
  auto name = get_instruction_name(minst, *iinfo);
  if (is_return(minst, instrs, target_machine))
    return generate_return(minst, target_machine, builder, rmap, reg_stats,
                           bb.getParent());
  if (is_indirect_branch(minst, instrs, target_machine))
    return generate_indirect_branch(*bb.getParent()->getParent(), bb, minst,
                                    builder, target_machine, rmap, state,
                                    reg_stats, instrs);
  if (is_branch(minst, target_machine)) {
    if (!minst.isConditionalBranch())
      return generate_jump(minst, builder, m2b);
    assert(minst.isConditionalBranch());
    return generate_branch(minst, bb, builder, rmap, target_machine, m2b,
                           reg_stats, instrs);
  }
  if (is_return(minst, instrs, target_machine))
    return generate_return(minst, target_machine, builder, rmap, reg_stats,
                           bb.getParent());
  if (is_call(minst, target_machine))
    return generate_call(minst, builder, bb, rmap, target_machine, state,
                         reg_stats, functions_nop);
  if (is_stack_manipulation(minst, sptrack)) {
    return generate_load_store_from_stack(minst, builder, bb, rmap, instrs,
                                          target_machine, state, reg_stats);
  }

  // TODO: track stack pointerS
  auto *m = bb.getParent()->getParent();
  auto *func = m->getFunction(name);
  if (!func) {
    std::string instr;
    raw_string_ostream ss(instr);
    minst.print(ss);
    throw std::runtime_error("Could not find \"" + name +
                             "\" in module: " + instr);
  }
  auto uses = minst.uses();
  bool uses_sp = ranges::find_if(uses, [&](auto &u) {
                   return u.isReg() && sptrack.is_stack_pointer(u.getReg());
                 }) != uses.end();
  bool is_stack_pointer_manipulation =
      !minst.defs().empty() && minst.defs().begin()->isReg() && uses_sp;
  if (is_stack_pointer_manipulation) {
    auto dst = minst.defs().begin()->getReg();
    sptrack.add(dst);
    return generate_stack_pointer_modification(
        minst, builder, bb, target_machine, rmap, instrs, reg_stats);
  }
  auto *func_type = func->getFunctionType();
  assert(func_type);
  std::vector<Type *> param_types(func_type->param_begin(),
                                  func_type->param_end());
  auto args = minst.uses() | views::filter([](auto &mi) {
                return !mi.isReg() || (!mi.isDef() && !mi.isImplicit());
              }) |
              views::transform([&](auto &mop) -> Value * {
                return operand_to_value(mop, bb, target_machine, rmap,
                                        reg_stats, instrs);
              }) |
              ranges::to<std::vector>();
  auto casted_args = views::zip(args, param_types) |
                     views::transform([&](auto &&arg) -> Value * {
                       auto &&[val, ty] = arg;
                       return cast_value_to(builder, val, ty);
                     }) |
                     ranges::to<std::vector>();

  auto *call = builder.CreateCall(func->getFunctionType(), func, casted_args);
  for (auto &def : minst.defs()) {
    assert(def.isDef());
    if (!def.isReg())
      throw std::runtime_error("Only register operands are supported");
    auto dst = def.getReg();
    sptrack.erase(dst);
    write_value_to_register(call, dst, builder, rmap, instrs, target_machine,
                            reg_stats);
  }
  return call;
}

void fill_ir_for_bb(MachineBasicBlock &mbb, reg2vals &rmap,
                    const instr_impl &instrs,
                    const TargetMachine &target_machine, const mbb2bb &m2b,
                    StructType &state, const register_stats &reg_stats,
                    stack_pointer_tracker &sptrack, bool functions_nop) {
  assert(!mbb.empty());
  auto *bb = m2b[&mbb];
  IRBuilder builder(bb->getContext());
  builder.SetInsertPoint(bb);
  assert(bb);
  for (auto &minst : mbb) {
    generate_instruction(minst, *bb, builder, rmap, instrs, target_machine, m2b,
                         state, reg_stats, sptrack, functions_nop);
  }
  auto last = std::prev(mbb.end());
  if (!is_branch(*last, target_machine) &&
      !is_return(*last, instrs, target_machine)) {
    if (is_call(*last, target_machine)) {
      builder.CreateRetVoid();
    } else {
      auto succ = std::next(mbb.getIterator());
      builder.CreateBr(m2b[std::addressof(*succ)]);
    }
  }
}

static std::string get_c_type_string(Type *ty) {
  if (auto *intgr = dyn_cast<IntegerType>(ty))
    return std::format("int{}_t", intgr->getBitWidth());
  if (ty->getTypeID() == Type::TypeID::FloatTyID)
    return "float";
  if (ty->getTypeID() == Type::TypeID::DoubleTyID)
    return "double";
  if (ty->getTypeID() == Type::TypeID::VoidTyID)
    return "void";
  throw std::invalid_argument("Unsupported type encountered");
}

static bool has_special_chars(std::string_view str) {
  std::array special_chars = {'$', '.', ','};
  return std::ranges::any_of(special_chars,
                             [str](auto ch) { return str.contains(ch); });
}

static void save_function_declarations(std::ostream &os,
                                       std::span<Function *> funcs) {
  os << '\n';
  for (auto *f : funcs | views::filter([](auto *f) {
                   return !has_special_chars(f->getName());
                 })) {
    auto *ret_type = f->getReturnType();
    os << "extern " << get_c_type_string(ret_type) << ' ' << f->getName().str()
       << "(struct register_state *state);\n\n";
  }
}

static void print_state_header(std::ostream &os, std::span<Function *> funcs,
                               const StructType &state,
                               const register_stats &rstats) {
  print_state_struct_definition(os, state, rstats);
  save_function_declarations(os, funcs);
}

Module &bleach_module(Module &m, MachineModuleInfo &mmi,
                      const instr_impl &instrs,
                      std::string_view state_struct_file, size_t stack_size,
                      const mctomir::file_info *finfo,
                      std::string_view lifted_prefix,
                      bool assume_functions_nop) {
  auto reg_stats = collect_register_stats(instrs, m, mmi);
  std::set<Function *> target_functions;
  ranges::transform(m, std::inserter(target_functions, target_functions.end()),
                    [](auto &f) { return &f; });
  fill_module_with_instrs(m, instrs);
  std::unordered_map<MachineFunction *, clone_function_result> funcs;
  for (auto *f : target_functions) {
    auto &mf = mmi.getOrCreateMachineFunction(*f);
    funcs.try_emplace(&mf, clone_machine_function(m, mmi, mf));
  }
  for (auto &&[oldf, func_info] : funcs) {
    for (auto &block : *func_info.mfunc) {
      for (auto &inst : block) {
        if (!is_call(inst, mmi.getTarget()))
          continue;
        auto ops = inst.operands();
        auto global_op =
            ranges::find_if(ops, [](auto &op) { return op.isGlobal(); });
        if (global_op == ops.end())
          continue;
        assert(global_op != ops.end());
        auto *callee_global = global_op->getGlobal();
        auto *callee = dyn_cast<Function>(callee_global);
        assert(callee);
        auto *new_callee = funcs.at(mmi.getMachineFunction(*callee)).mfunc;
        global_op->ChangeToGA(&new_callee->getFunction(), /*Offset=*/0);
      }
    }
  }

  std::vector<Function *> translated;
  ranges::transform(funcs, std::back_inserter(translated), [](auto &f) {
    auto &&[oldf, newf] = f;
    return &newf.mfunc->getFunction();
  });

  create_bleach_symtab_add_function_decl(m);
  create_bleach_symtab_lookup_function_decl(m);

  auto &state = create_state_type(m.getContext(), reg_stats, stack_size);
  for (auto &&[oldf, func_info] : funcs)
    generate_function(*oldf, func_info, instrs, mmi, state, reg_stats,
                      assume_functions_nop);
  if (finfo) {
    for (auto *func :
         translated | views::filter([](auto *f) { return !f->empty(); }))
      fill_symtab_at(func, translated, *finfo);
  }
  if (finfo) {
    SmallVector<char> ir_buf(symtab_ir_string.begin(), symtab_ir_string.end());
    auto mem_buf = SmallVectorMemoryBuffer(std::move(ir_buf), "symtab-ir.ll");
    SMDiagnostic err;
    auto extra = parseIR(mem_buf, err, m.getContext());
    if (!extra) {
      std::string err_str;
      raw_string_ostream ss(err_str);
      err.print("symtab.ll", ss);
      throw std::runtime_error("Failed to parse LLVM IR" + err_str);
    }
    Linker::linkModules(m, std::move(extra));
  }
  // rename functions
  for (auto *func : translated)
    func->setName(std::format("{}{}", lifted_prefix, func->getName().data()));
  // create state struct and generate header
  if (!state_struct_file.empty()) {
    if (state_struct_file == "-") {
      print_state_header(std::cout, translated, state, reg_stats);
    } else {
      std::fstream fs(std::string(state_struct_file), std::fstream::out);
      if (!fs.is_open())
        throw std::runtime_error(
            std::format("Could not open file \"{}\"", state_struct_file));
      print_state_header(fs, translated, state, reg_stats);
    }
  }
  for (auto *f : target_functions)
    f->eraseFromParent();
  // Remove unused/inlined instr impl functions
  // TODO: make it a separate pass after inliner
  auto unused_funcs =
      m | views::filter([&translated](auto &func) {
        auto found_translated = ranges::find_if(
            translated, [target = &func](auto *f) { return f == target; });
        return (found_translated == translated.end()) && func.user_empty();
      }) |
      views::transform([](auto &f) { return &f; }) | ranges::to<std::vector>();
  for (auto *func : unused_funcs) {
    func->eraseFromParent();
  }
  return m;
}
} // namespace bleach::lifter
