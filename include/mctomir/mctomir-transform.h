#pragma once

#include "mctomir/elf-loader.h"

#include <llvm/CodeGen/MachineBasicBlock.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineInstr.h>
#include <llvm/CodeGen/MachineInstrBuilder.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/MachineOperand.h>
#include <llvm/CodeGen/TargetFrameLowering.h>
#include <llvm/CodeGen/TargetInstrInfo.h>
#include <llvm/CodeGen/TargetRegisterInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCDisassembler/MCDisassembler.h>
#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/MCInstrAnalysis.h>
#include <llvm/MC/MCSymbol.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>

#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace mctomir {
using namespace llvm;
using namespace llvm::object;

struct translated_inst final {
  uint64_t address;    // Original address
  MCInst inst;         // MC instruction
  MachineInstr *minst; // Machine instruction (owned by MachineFunction)
  SmallVector<uint8_t, 8> bytes;
  uint64_t section_offset = 0;    // Locates the bytes within the input section.
  std::optional<uint64_t> target; // Absolute destination if MC resolved it.
  // Only this MC operand can be replaced with a block or function reference.
  std::optional<unsigned> target_operand;
#ifdef MCTOMIR_ENABLE_TMS320
  // TMS320-only packet/timing annotations; unused by other targets.
  unsigned packet_words = 0;  // Packet length, set only on its first word.
  unsigned packet_cycles = 0; // Issue cycles at packet start; zero for IDLE.
  // On the last delay word, points back to the branch that issued the transfer.
  translated_inst *commit_issue = nullptr;
  bool in_delay = false; // A transfer is pending here; reject direct entry.
#endif

  translated_inst(uint64_t addr, const MCInst &i)
      : address(addr), inst(i), minst(nullptr) {}
};

struct translated_block final {
  uint64_t start_address;
  uint64_t end_address;
  MachineBasicBlock *mbb;
  std::vector<translated_inst *> instrs;

  translated_block(uint64_t start)
      : start_address(start), end_address(0), mbb(nullptr) {}
};

struct translated_function final {
  std::vector<translated_inst> insts;
  std::vector<translated_block> blocks;
  uint64_t start = 0;
  uint64_t end = 0;
  std::string name;
  Function *func = nullptr;
  MachineFunction *mfunc = nullptr;
  uint64_t section_index = 0;
  std::map<uint64_t, MachineBasicBlock *> blocks_by_address;
};

struct section_info final {
  enum access : unsigned {
    r = 0b01,
    w = 0b10,
  };
  std::vector<std::byte> data;
  std::string name;
  uint64_t start;
  access acc = access::r;
};

struct translation_result final {
  std::unique_ptr<TargetMachine> tmachine;
  std::unique_ptr<Module> mod;
  std::unique_ptr<MachineModuleInfo> mmi;
  std::vector<translated_function> funcs;
  std::unique_ptr<MCContext> mcctx;
  std::unique_ptr<MCInstrAnalysis> mi_analysis;
  std::vector<section_info> sections;
};

class translator_t final {
public:
  translator_t(LLVMContext &p_ctx) : ctx(p_ctx) {}

  Error initialize(StringRef triple_name, SubtargetFeatures features);

  // `functions` contains only the function symbols defined in this section.
  Error process_disassembly(SectionRef section, StringRef contents,
                            ArrayRef<SymbolRef> functions);

  Error finalize();

  Error write_mir(StringRef output_filename) const;

  void print_mir(raw_ostream &os) const;

  void set_section(StringRef name, StringRef contents, uint64_t start,
                   section_info::access acc);

  const auto &get_funcs() const & { return res.funcs; }
  auto &&get_result() && { return std::move(res); }

private:
  const TargetInstrInfo *tii = nullptr;
  const TargetRegisterInfo *tri = nullptr;
  const TargetSubtargetInfo *tsi = nullptr;

  LLVMContext &ctx;
  translation_result res;

#ifdef MCTOMIR_ENABLE_TMS320
  // TMS320-specific policy and state, kept together for later extraction.
  bool is_c6() const;
  // C6_* pseudo names -> backend opcode IDs, discovered from TargetInstrInfo.
  // Used to serialize packet boundaries and branch issue/commit events in MIR.
  std::map<std::string, unsigned> pseudos;
  // Group p-bit-linked words and locate commits after five delay cycles.
  // Annotates decoded records before CFG construction; rejects unmodeled
  // timing.
  Error analyze_packets(translated_function &func);
#endif

  Error create_module_and_function(StringRef triple_name,
                                   SubtargetFeatures features);
  Error identify_basic_blocks();
  Error create_machine_basic_blocks();
  Error translate_instructions();
  Error link_machine_basic_blocks();

  void add_operand_to_mib(MachineInstrBuilder &mib, const MCOperand &mc_op,
                          unsigned op_idx, const MCInstrDesc &desc);
  MachineInstr *create_machine_instr(const translated_inst &tinst,
                                     translated_function &func,
                                     MachineBasicBlock *mbb);
};

class elf_to_mir_converter {
public:
  elf_to_mir_converter(LLVMContext &ctx)
      : disassembler(std::make_unique<elf_disassembler>(loader)),
        translator(std::make_unique<translator_t>(ctx)) {}

  Error process_file(StringRef file_path);
  Error convert_sections();
  Error read_sections();
  Error write_mir(StringRef output_path);
  void print_mir(raw_ostream &os);
  auto &get_translator() const & { return *translator; }

private:
  elf_loader loader;
  std::unique_ptr<elf_disassembler> disassembler;
  std::unique_ptr<translator_t> translator;
};

void print_mir(raw_ostream &os, const translation_result &res);

translation_result lift_elf_file(LLVMContext &ctx,
                                 std::string_view input_filename);

} // namespace mctomir
