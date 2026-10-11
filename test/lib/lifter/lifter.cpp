#include "bleach/lifter/lifter.hpp"

#include <gtest/gtest.h>

#include <llvm/CodeGen/CommandFlags.h>
#include <llvm/CodeGen/MIRParser/MIRParser.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

namespace {
using namespace llvm;

constexpr std::string_view foo_mir = R"(
--- |
  ; ModuleID = 'foo.ll'
  target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128"
  target triple = "riscv64-unknown-linux-gnu"
  
  ; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none) uwtable
  define dso_local signext void @foo(i64 noundef signext %0, i64 noundef signext %1) local_unnamed_addr #0 {
   ret void 
  }
...
---
name:            foo
alignment:       2
tracksRegLiveness: true
liveins:
  - { reg: '$x10' }
  - { reg: '$x11' }
body:             |
  bb.0 (%ir-block.2):
    liveins: $x10, $x11

    renamable $x10 = ADDW renamable $x11, killed renamable $x10
    renamable $x11 = ADDW $x10, $x10
    renamable $x11 = SUBW killed renamable $x11, renamable $x10 

...
)";

codegen::RegisterCodeGenFlags cfg;

auto parse_mir(LLVMContext &ctx) {
  SMDiagnostic err;
  auto mir_parser =
      createMIRParser(std::make_unique<SmallVectorMemoryBuffer>(
                          SmallVector<char>(foo_mir.begin(), foo_mir.end())),
                      ctx);
  if (!mir_parser)
    throw std::runtime_error(err.getMessage().str());

  Triple triple;
  std::unique_ptr<TargetMachine> target_machine;
  auto set_data_layout = [&](StringRef data_layout_target_triple,
                             StringRef) -> std::optional<std::string> {
    auto target_triple_str = data_layout_target_triple.str();
    if (!target_triple_str.empty())
      target_triple_str = Triple::normalize(target_triple_str);
    triple = Triple(target_triple_str);
    if (triple.getTriple().empty()) {
      triple.setTriple(sys::getDefaultTargetTriple());
    }
    auto tm = codegen::createTargetMachineForTriple(triple);
    if (auto err_tm = tm.takeError())
      throw std::runtime_error(toString(std::move(err_tm)));
    target_machine = std::unique_ptr<TargetMachine>(
        static_cast<TargetMachine *>(tm->release()));
    return target_machine->createDataLayout().getStringRepresentation();
  };
  auto m = mir_parser->parseIRModule(set_data_layout);
  auto machine_module_info =
      std::make_unique<MachineModuleInfo>(target_machine.get());
  assert(m);
  if (mir_parser->parseMachineFunctions(*m, *machine_module_info))
    throw std::runtime_error("Failed to parse machine functions");

  return std::make_tuple(std::move(m), std::move(target_machine),
                         std::move(machine_module_info));
}

TEST(lifter_basic, copy_instructions) {
  InitializeAllTargetInfos();
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  LLVMContext ctx;
  auto [m, target_machine, machine_module_info] = parse_mir(ctx);
  for (auto &f : *m) {
    auto &mf = machine_module_info->getOrCreateMachineFunction(f);
    assert(!mf.empty());
    auto &src = *mf.begin();
    auto *dst = mf.CreateMachineBasicBlock();
    bleach::lifter::copy_instructions(src, *dst);
    for (auto &&[inst, inst_copy] : zip(src, *dst)) {
      std::string str1;
      raw_string_ostream os1(str1);
      std::string str2;
      raw_string_ostream os2(str2);
      inst.print(os1);
      inst_copy.print(os2);
      std::cout << "original: " << str1 << "\n";
      std::cout << "copied: " << str2 << "\n";
      EXPECT_EQ(str1, str2);
    }
  }
}

TEST(lifter_basic, clone_basic_block) {
  InitializeAllTargetInfos();
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  LLVMContext ctx;
  auto [m, target_machine, machine_module_info] = parse_mir(ctx);
  for (auto &f : *m) {
    auto &mf = machine_module_info->getOrCreateMachineFunction(f);
    for (auto &mbb : make_early_inc_range(mf)) {
      auto [copy_mbb, copy_bb] = bleach::lifter::clone_basic_block(mbb, mf);
      for (auto &&[inst, copy_inst] : zip(mbb, *copy_mbb)) {
        std::string str1;
        raw_string_ostream os1(str1);
        std::string str2;
        raw_string_ostream os2(str2);
        inst.print(os1);
        copy_inst.print(os2);
        std::cout << "original: " << str1 << "\n";
        std::cout << "copied: " << str2 << "\n";
        EXPECT_EQ(str1, str2);
      }
    }
  }
}

} // namespace
