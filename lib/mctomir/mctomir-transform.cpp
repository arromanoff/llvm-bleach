#include "mctomir/mctomir-transform.h"

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/CodeGen/MIRPrinter.h>
#include <llvm/CodeGen/MachineInstrBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/MC/MCInstrAnalysis.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Endian.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/SubtargetFeature.h>

#include <algorithm>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string_view>

namespace mctomir {
using namespace llvm;
namespace ranges = std::ranges;

Error translator_t::initialize(StringRef triple_name,
                               SubtargetFeatures features) {
  if (Error err = create_module_and_function(triple_name, features))
    return err;

  if (!res.mod || !res.tmachine)
    return createStringError(inconvertibleErrorCode(),
                             "Module or target machine not initialized");
  FunctionType *func_ty = FunctionType::get(Type::getVoidTy(ctx), false);
  auto *dummy = Function::Create(func_ty, Function::ExternalLinkage,
                                 "__dummy__", *res.mod);
  tsi = res.tmachine->getSubtargetImpl(*dummy);
  dummy->eraseFromParent();
  if (!tsi)
    return createStringError(inconvertibleErrorCode(),
                             "Failed to get subtarget info");

  tii = tsi->getInstrInfo();
  if (!tii)
    return createStringError(inconvertibleErrorCode(),
                             "Failed to get instruction info");

  tri = tsi->getRegisterInfo();
  if (!tri)
    return createStringError(inconvertibleErrorCode(),
                             "Failed to get register info");

  res.mmi = std::make_unique<MachineModuleInfo>(res.tmachine.get());
  res.mmi->initialize();

  return Error::success();
}

Error translator_t::create_module_and_function(StringRef triple_name,
                                               SubtargetFeatures features) {
  Triple the_triple(Triple::normalize(triple_name));

  std::string err_msg;
  const Target *the_target = TargetRegistry::lookupTarget(the_triple, err_msg);
  if (!the_target)
    return createStringError(inconvertibleErrorCode(),
                             "Cannot find target for triple %s: %s",
                             triple_name.data(), err_msg.c_str());

  res.mod = std::make_unique<Module>("DisassembledModule", ctx);
  res.mod->setTargetTriple(the_triple);

  TargetOptions options;
  res.tmachine.reset(the_target->createTargetMachine(
      the_triple, "", features.getString(), options, std::nullopt));
  if (!res.tmachine)
    return createStringError(inconvertibleErrorCode(),
                             "Cannot create target machine for %s",
                             triple_name.data());

  res.mod->setDataLayout(res.tmachine->createDataLayout());
  return Error::success();
}

Error translator_t::process_disassembly(SectionRef section, StringRef contents,
                                        ArrayRef<SymbolRef> functions) {
  if (!res.tmachine || !tii || !tri || !tsi)
    return createStringError(inconvertibleErrorCode(),
                             "translator not properly initialized");
  uint64_t section_addr = section.getAddress();

  ArrayRef<uint8_t> bytes(reinterpret_cast<const uint8_t *>(contents.data()),
                          contents.size());

  // mia used to identify branches, calls, etc.
  std::unique_ptr<MCInstrAnalysis> mia(
      res.tmachine->getTarget().createMCInstrAnalysis(
          res.tmachine->getMCInstrInfo()));
  if (!mia)
    return createStringError(inconvertibleErrorCode(),
                             "Failed to create MCInstrAnalysis");
  res.mi_analysis = std::move(mia);

  if (!res.mcctx) {
    res.mcctx = std::make_unique<MCContext>(
        Triple(res.tmachine->getTargetTriple()), res.tmachine->getMCAsmInfo(),
        res.tmachine->getMCRegisterInfo(), res.tmachine->getMCSubtargetInfo());
  }

  std::unique_ptr<const MCDisassembler> dis_asm(
      res.tmachine->getTarget().createMCDisassembler(*tsi, *res.mcctx));
  if (!dis_asm)
    return createStringError(inconvertibleErrorCode(),
                             "Failed to create disassembler");
  if (contents.size() > UINT64_MAX - section_addr)
    return createStringError("executable section address overflow");
  const uint64_t limit = section_addr + contents.size();
  const size_t first = res.funcs.size();
  for (SymbolRef s : functions) {
    auto name = s.getName();
    if (!name)
      return name.takeError();
    if (name->empty())
      return createStringError("unnamed ELF function");
    auto addr = s.getAddress();
    if (!addr)
      return addr.takeError();
    const auto size = ELFSymbolRef(s).getSize();
    if (*addr < section_addr || *addr >= limit || size > limit - *addr)
      return createStringError("function range outside executable section: " +
                               *name);
    res.funcs.push_back({{}, {}, *addr, *addr + size, name->str()});
    res.funcs.back().section_index = section.getIndex();
  }
  auto local = MutableArrayRef(res.funcs).drop_front(first);
  ranges::sort(local, {}, &translated_function::start);
  for (size_t i = 0; i < local.size(); ++i) {
    auto &finfo = local[i];
    uint64_t next = i + 1 == local.size() ? limit : local[i + 1].start;
    if (finfo.start == next || finfo.end > next)
      return createStringError("aliased or overlapping function: " +
                               finfo.name);
    // For a zero-size symbol, stop at the next function or the section end.
    if (finfo.end == finfo.start)
      finfo.end = next;
    if (res.mod->getNamedValue(finfo.name))
      return createStringError("duplicate function name: " + finfo.name);
    // FIXME: don't always return i64
    finfo.func =
        Function::Create(FunctionType::get(Type::getInt64Ty(ctx), false),
                         Function::ExternalLinkage, finfo.name, *res.mod);
    finfo.mfunc = &res.mmi->getOrCreateMachineFunction(*finfo.func);
    finfo.mfunc->getProperties().reset(
        MachineFunctionProperties::Property::TracksLiveness);
    for (size_t addr = finfo.start; addr < finfo.end;) {
      auto diff = addr - section_addr;
      auto remaining = bytes.slice(diff, finfo.end - addr);
      MCInst inst;
      uint64_t size = 0;
      auto status =
          dis_asm->getInstruction(inst, size, remaining, addr, nulls());
      if (status != MCDisassembler::Success || !size || size > remaining.size())
        return createStringError(finfo.name + ": " +
                                 (status == MCDisassembler::SoftFail
                                      ? "soft-failed"
                                      : "invalid or truncated") +
                                 " instruction at 0x" + utohexstr(addr));
      auto &record = finfo.insts.emplace_back(addr, inst);
      record.section_offset = diff;
      record.bytes.append(remaining.begin(), remaining.begin() + size);
      uint64_t target;
      if (res.mi_analysis->evaluateBranch(inst, addr, size, target)) {
        record.target = target;
        const auto &desc = tii->get(inst.getOpcode());
        for (unsigned j = 0;
             j < std::min<unsigned>(inst.size(), desc.getNumOperands()); ++j) {
          const auto &op = desc.operands()[j];
          if (inst.getOperand(j).isImm() &&
              (op.isBranchTarget() || op.OperandType == MCOI::OPERAND_PCREL))
            record.target_operand = j;
        }
      }
      addr += size;
    }
  }
  return Error::success();
}

Error translator_t::finalize() {
  if (Error err = identify_basic_blocks())
    return err;
  if (Error err = create_machine_basic_blocks())
    return err;
  if (Error err = translate_instructions())
    return err;
  if (Error err = link_machine_basic_blocks())
    return err;
  return Error::success();
}

Error translator_t::identify_basic_blocks() {
  std::set<uint64_t> leaders;
  for (auto &func : res.funcs) {
    leaders.clear();
    if (!func.insts.empty())
      leaders.insert(func.insts.front().address);
    for (size_t i = 0; i < func.insts.size(); ++i) {
      const translated_inst &tinst = func.insts[i];
      const MCInst &inst = tinst.inst;
      if (res.mi_analysis->isBranch(inst) && tinst.target &&
          *tinst.target >= func.start && *tinst.target < func.end) {
        leaders.insert(*tinst.target);
      }
      if ((res.mi_analysis->isBranch(inst) ||
           res.mi_analysis->isReturn(inst))) {
        if (i + 1 < func.insts.size()) {
          leaders.insert(func.insts[i + 1].address);
        }
      }
    }

    for (translated_block *current_block = nullptr; auto &tinst : func.insts) {
      uint64_t current_addr = tinst.address;
      if (leaders.count(current_addr) > 0) {
        if (current_block)
          current_block->end_address = current_addr;

        func.blocks.emplace_back(current_addr);
        current_block = &func.blocks.back();
      }

      if (current_block)
        current_block->instrs.push_back(&tinst);
      else {
        return createStringError(inconvertibleErrorCode(),
                                 "Failed to create basic block");
      }
    }

    if (!func.blocks.empty()) {
      const translated_inst &last_inst = func.insts.back();
      func.blocks.back().end_address =
          last_inst.address + last_inst.bytes.size();
    }
  }
  return Error::success();
}

Error translator_t::create_machine_basic_blocks() {
  for (auto &func : res.funcs) {
    for (auto &block : func.blocks) {
      MachineBasicBlock *mbb = func.mfunc->CreateMachineBasicBlock();
      func.mfunc->push_back(mbb);
      block.mbb = mbb;
      func.blocks_by_address[block.start_address] = mbb;
    }
  }

  return Error::success();
}

Error translator_t::translate_instructions() {
  if (!tii)
    return createStringError(inconvertibleErrorCode(),
                             "target instruction info is not initialized");

  for (auto &finfo : res.funcs) {
    for (auto &block : finfo.blocks) {
      for (auto *tinst : block.instrs) {
        for (const auto &op : tinst->inst)
          if (!op.isReg() && !op.isImm())
            return createStringError("unsupported MC operand at 0x" +
                                     utohexstr(tinst->address));
        MachineInstr *minst = create_machine_instr(*tinst, finfo, block.mbb);
        tinst->minst = minst;
        if (!minst)
          return createStringError(inconvertibleErrorCode(),
                                   "Failed to convert instruction at 0x%llx",
                                   tinst->address);
      }
    }
  }

  return Error::success();
}

MachineInstr *translator_t::create_machine_instr(const translated_inst &tinst,
                                                 translated_function &func,
                                                 MachineBasicBlock *mbb) {
  if (!tii || !mbb)
    return nullptr;

  auto &inst = tinst.inst;
  unsigned opcode = inst.getOpcode();
  const MCInstrDesc &desc = tii->get(opcode);
  auto mib = BuildMI(*mbb, mbb->end(), DebugLoc(), desc);
  for (unsigned i = 0; i < inst.size(); ++i) {
    if (tinst.target && i == tinst.target_operand) {
      if (res.mi_analysis->isCall(inst)) {
        const auto callee = llvm::find_if(res.funcs, [&](const auto &f) {
          return f.start == *tinst.target &&
                 f.section_index == func.section_index;
        });
        if (callee != res.funcs.end()) {
          mib.addGlobalAddress(callee->func);
          continue;
        }
      } else if (auto it = func.blocks_by_address.find(*tinst.target);
                 it != func.blocks_by_address.end()) {
        mib.addMBB(it->second);
        mbb->addSuccessor(it->second);
        continue;
      }
    }
    add_operand_to_mib(mib, inst.getOperand(i), i, desc);
  }
  return mib.getInstr();
}

void translator_t::add_operand_to_mib(MachineInstrBuilder &mib,
                                      const MCOperand &mc_op, unsigned op_idx,
                                      const MCInstrDesc &desc) {
  if (mc_op.isReg()) {
    unsigned reg = mc_op.getReg();

    bool is_def = op_idx < desc.getNumDefs();

    mib.addReg(reg, is_def ? RegState::Define : RegState::NoFlags);
  } else if (mc_op.isImm()) {
    mib.addImm(mc_op.getImm());
  } else
    llvm_unreachable("Unsupported operand");
}

Error translator_t::link_machine_basic_blocks() {
  for (auto &finfo : res.funcs) {
    // The last block has no fallthrough within this function.
    for (size_t i = 0; i + 1 < finfo.blocks.size(); ++i) {
      auto &block = finfo.blocks[i];
      MachineBasicBlock *mbb = block.mbb;

      if (block.instrs.empty())
        continue;

      auto *last_t_inst = block.instrs.back();
      const auto *transfer = last_t_inst;
      if (transfer && (res.mi_analysis->isUnconditionalBranch(transfer->inst) ||
                       res.mi_analysis->isReturn(transfer->inst)))
        continue;
      auto *next = finfo.blocks[i + 1].mbb;
      if (!mbb->isSuccessor(next))
        mbb->addSuccessor(next);
    }
  }
  return Error::success();
}

static section_info::access get_access(SectionRef section) {
  auto acc = section_info::access::r;
  ELFSectionRef elfsec(section);
  auto flags = elfsec.getFlags();
  if (flags & (ELF::SHF_WRITE))
    acc = static_cast<section_info::access>(acc | section_info::access::w);
  return acc;
}

void translator_t::set_section(StringRef name, StringRef contents,
                               uint64_t start, section_info::access acc) {
  res.sections.push_back(
      {.data = {}, .name = name.str(), .start = start, .acc = acc});
  auto &data = res.sections.back().data;
  data.resize(contents.size());
  ranges::transform(contents, data.begin(), [](char b) {
    return std::byte{static_cast<unsigned char>(b)};
  });
}

static bool is_data_section(SectionRef section) {
  if (section.isData())
    return true;
  ELFSectionRef elf_section(section);
  auto flags = elf_section.getFlags();
  return (flags & ELF::SHF_ALLOC);
}

Error elf_to_mir_converter::read_sections() {
  auto elf_object_or_err = loader.get_elf_object();
  if (auto err = elf_object_or_err.takeError())
    return err;
  auto *elf = *elf_object_or_err;
  for (auto &section : elf->sections()) {
    if (is_data_section(section)) {
      Expected<StringRef> contents_or_err =
          loader.get_section_contents(section);
      if (!contents_or_err)
        return contents_or_err.takeError();

      StringRef contents = contents_or_err.get();
      auto name = section.getName();
      if (auto err = name.takeError())
        return err;
      translator->set_section(*name, contents, section.getAddress(),
                              get_access(section));
    }
  }
  return Error::success();
}

Error translator_t::write_mir(StringRef output_filename) const {
  std::error_code ec;
  raw_fd_ostream os(output_filename, ec);
  if (ec)
    return createFileError(output_filename, errorCodeToError(ec));

  print_mir(os);

  return Error::success();
}

void translator_t::print_mir(raw_ostream &os) const {
  if (!res.mod)
    throw std::runtime_error("Module does not exist");
  llvm::printMIR(os, *res.mod);
  for (auto &finfo : res.funcs) {
    llvm::printMIR(os, *res.mmi, *finfo.mfunc);
  }
}

// Hide main for now
Error elf_to_mir_converter::process_file(StringRef file_path) {
  if (Error err = loader.load_file(file_path))
    return err;

  disassembler = std::make_unique<elf_disassembler>(loader);
  if (Error err = disassembler->initialize())
    return err;
  Expected<Triple> triple_or_err = loader.get_triple();
  if (!triple_or_err)
    return triple_or_err.takeError();

  Expected<SubtargetFeatures> features_or_err =
      disassembler->get_disasm_features();
  if (!features_or_err)
    return features_or_err.takeError();
  SubtargetFeatures features = features_or_err.get();
  if (Error err = translator->initialize(triple_or_err.get().str(), features))
    return err;

  return Error::success();
}

Error elf_to_mir_converter::convert_sections() {
  if (!disassembler || !translator)
    return createStringError(inconvertibleErrorCode(),
                             "disassembler or translator not initialized");

  auto obj = loader.get_elf_object();
  if (!obj)
    return obj.takeError();
  std::map<uint64_t, std::vector<SymbolRef>> functions;
  for (auto sym : (*obj)->symbols()) {
    auto type = sym.getType();
    if (!type)
      return type.takeError();
    if (*type != SymbolRef::ST_Function)
      continue;
    auto sec = sym.getSection();
    if (!sec)
      return sec.takeError();
    if (*sec != (*obj)->section_end() && (**sec).isText())
      functions[(**sec).getIndex()].push_back(sym);
  }
  if (functions.empty())
    return createStringError(
        "no defined ELF function symbols in executable sections");
  for (auto section : (*obj)->sections()) {
    auto it = functions.find(section.getIndex());
    if (it == functions.end())
      continue;
    auto contents = section.getContents();
    if (!contents)
      return contents.takeError();
    if (auto err =
            translator->process_disassembly(section, *contents, it->second))
      return err;
  }

  if (Error err = translator->finalize())
    return err;

  return Error::success();
}

Error elf_to_mir_converter::write_mir(StringRef output_path) {
  if (!translator)
    return createStringError(inconvertibleErrorCode(),
                             "translator not initialized");

  return translator->write_mir(output_path);
}

void elf_to_mir_converter::print_mir(raw_ostream &os) {
  if (translator)
    translator->print_mir(os);
}

translation_result lift_elf_file(LLVMContext &ctx,
                                 std::string_view input_filename) {
  elf_to_mir_converter converter(ctx);

  if (Error err = converter.process_file(input_filename))
    throw std::runtime_error("Failed to process file: " +
                             toString(std::move(err)));

  if (Error err = converter.convert_sections())
    throw std::runtime_error("Failed to convert section: " +
                             toString(std::move(err)));
  if (Error err = converter.read_sections())
    throw std::runtime_error("Failed to read data sections" +
                             toString(std::move(err)));

  return std::move(converter.get_translator()).get_result();
}

void print_mir(raw_ostream &os, const translation_result &res) {
  if (!res.mod)
    throw std::runtime_error("Module does not exist");
  llvm::printMIR(os, *res.mod);
  for (auto &finfo : res.funcs) {
    llvm::printMIR(os, *res.mmi, *finfo.mfunc);
  }
}
} // namespace mctomir
