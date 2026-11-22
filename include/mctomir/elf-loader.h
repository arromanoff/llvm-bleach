#pragma once

#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCDisassembler/MCDisassembler.h>
#include <llvm/MC/MCInst.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/MC/MCTargetOptions.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ELF.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <memory>

namespace mctomir {
using namespace llvm;
using namespace llvm::object;

/// ELFLoader provides an interface for loading and validating ELF files.
class elf_loader {
public:
  elf_loader() = default;

  Error load_file(StringRef file_path);

  Expected<ELFObjectFileBase *> get_elf_object() const;

  Expected<Triple> get_triple() const;

  Error get_section_by_name(StringRef section_name, SectionRef &section) const;

  Expected<StringRef> get_section_contents(const SectionRef &section) const;

  Expected<unsigned> get_file_type() const;

  Expected<unsigned> get_machine_type() const;

  Expected<SubtargetFeatures> get_features() const;

  ObjectFile &get_object() const { return *obj_file; }

private:
  std::unique_ptr<MemoryBuffer> buffer;
  std::unique_ptr<ObjectFile> obj_file;
  ELFObjectFileBase *elf_obj = nullptr;
};
/// elf_disassembler provides an interface for disassembling ELF sections.
class elf_disassembler {
public:
  elf_disassembler(const elf_loader &loader);

  Error initialize();

  Error disassemble_section(StringRef section_name, raw_ostream &os,
                            bool show_bytes = false);

  Error dump_section(StringRef section_name, raw_ostream &os,
                     bool show_address = true);

  Error disassemble_bytes(ArrayRef<uint8_t> bytes, uint64_t address,
                          raw_ostream &os, bool show_bytes = false);

  Expected<SubtargetFeatures> get_disasm_features() const;

private:
  const elf_loader &loader;

  // Disassembler components
  MCTargetOptions options;
  std::unique_ptr<const MCRegisterInfo> mri;
  std::unique_ptr<const MCAsmInfo> mai;
  std::unique_ptr<const MCSubtargetInfo> sti;
  std::unique_ptr<const MCInstrInfo> mii;
  std::unique_ptr<MCContext> ctx;
  std::unique_ptr<MCDisassembler> dis_asm;
  std::unique_ptr<MCInstPrinter> inst_printer;
  const Target *the_target = nullptr;
  bool initialized = false;
};
} // namespace mctomir
