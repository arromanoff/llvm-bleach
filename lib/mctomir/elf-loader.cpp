#include "mctomir/elf-loader.h"

#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/Error.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/WithColor.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/TargetParser/Triple.h>

using namespace llvm;
using namespace llvm::object;
namespace mctomir {

Error elf_loader::load_file(StringRef file_path) {
  buffer.reset();
  obj_file.reset();
  elf_obj = nullptr;

  if (!sys::fs::exists(file_path))
    return createStringError(inconvertibleErrorCode(),
                             "File '%s' doesn't exist",
                             file_path.str().c_str());

  ErrorOr<std::unique_ptr<MemoryBuffer>> buffer_or_err =
      MemoryBuffer::getFile(file_path);
  if (std::error_code ec = buffer_or_err.getError())
    return createFileError(file_path, errorCodeToError(ec));

  buffer = std::move(buffer_or_err.get());

  Expected<std::unique_ptr<ObjectFile>> obj_or_err =
      ObjectFile::createObjectFile(buffer->getMemBufferRef());
  if (!obj_or_err)
    return obj_or_err.takeError();

  obj_file = std::move(obj_or_err.get());

  if (!obj_file->isELF())
    return createStringError(inconvertibleErrorCode(),
                             "'%s' is not an ELF file",
                             file_path.str().c_str());

  elf_obj = dyn_cast<ELFObjectFileBase>(obj_file.get());
  if (!elf_obj)
    return createStringError(inconvertibleErrorCode(),
                             "Failed to cast to ELFObjectFileBase");

  return Error::success();
}

Expected<ELFObjectFileBase *> elf_loader::get_elf_object() const {
  if (!elf_obj)
    return createStringError(inconvertibleErrorCode(),
                             "No ELF file has been loaded");
  return elf_obj;
}

Expected<Triple> elf_loader::get_triple() const {
  if (!elf_obj)
    return createStringError(inconvertibleErrorCode(),
                             "No ELF file has been loaded");
  return elf_obj->makeTriple();
}

Error elf_loader::get_section_by_name(StringRef section_name,
                                      SectionRef &section) const {
  if (!elf_obj)
    return createStringError(inconvertibleErrorCode(),
                             "No ELF file has been loaded");

  for (const SectionRef &sec : elf_obj->sections()) {
    Expected<StringRef> name_or_err = sec.getName();
    if (!name_or_err)
      continue;
    if (name_or_err.get() == section_name) {
      section = sec;
      return Error::success();
    }
  }

  return createStringError(inconvertibleErrorCode(), "Section '%s' not found",
                           section_name.str().c_str());
}

Expected<StringRef>
elf_loader::get_section_contents(const SectionRef &section) const {
  if (!elf_obj)
    return createStringError(inconvertibleErrorCode(),
                             "No ELF file has been loaded");

  return section.getContents();
}

Expected<unsigned> elf_loader::get_file_type() const {
  if (!elf_obj)
    return createStringError(inconvertibleErrorCode(),
                             "No ELF file has been loaded");

  // Handle both 32-bit and 64-bit ELF files
  if (auto *elf_32_le = dyn_cast<ELF32LEObjectFile>(elf_obj))
    return elf_32_le->getELFFile().getHeader().e_type;
  if (auto *elf_32_be = dyn_cast<ELF32BEObjectFile>(elf_obj))
    return elf_32_be->getELFFile().getHeader().e_type;
  if (auto *elf_64_le = dyn_cast<ELF64LEObjectFile>(elf_obj))
    return elf_64_le->getELFFile().getHeader().e_type;
  if (auto *elf_64_be = dyn_cast<ELF64BEObjectFile>(elf_obj))
    return elf_64_be->getELFFile().getHeader().e_type;

  return createStringError(inconvertibleErrorCode(),
                           "Unsupported ELF file format");
}

Expected<unsigned> elf_loader::get_machine_type() const {
  if (!elf_obj)
    return createStringError(inconvertibleErrorCode(),
                             "No ELF file has been loaded");

  if (auto *elf_32_le = dyn_cast<ELF32LEObjectFile>(elf_obj))
    return elf_32_le->getELFFile().getHeader().e_machine;
  if (auto *elf_32_be = dyn_cast<ELF32BEObjectFile>(elf_obj))
    return elf_32_be->getELFFile().getHeader().e_machine;
  if (auto *elf_64_le = dyn_cast<ELF64LEObjectFile>(elf_obj))
    return elf_64_le->getELFFile().getHeader().e_machine;
  if (auto *elf_64_be = dyn_cast<ELF64BEObjectFile>(elf_obj))
    return elf_64_be->getELFFile().getHeader().e_machine;

  return createStringError(inconvertibleErrorCode(),
                           "Unsupported ELF file format");
}

Expected<SubtargetFeatures> elf_loader::get_features() const {
  if (!elf_obj)
    return createStringError(inconvertibleErrorCode(),
                             "No ELF file has been loaded");

  return elf_obj->getFeatures();
}

elf_disassembler::elf_disassembler(const elf_loader &p_loader)
    : loader(p_loader) {}

Error elf_disassembler::initialize() {
  if (initialized)
    return Error::success();

  Expected<Triple> triple_or_err = loader.get_triple();
  if (!triple_or_err)
    return triple_or_err.takeError();

  Triple the_triple = triple_or_err.get();
  std::string triple_name = the_triple.normalize();
  std::string error;

  the_target = TargetRegistry::lookupTarget(the_triple, error);
  if (!the_target)
    return createStringError(inconvertibleErrorCode(),
                             "Unable to find target for triple %s: %s",
                             triple_name.c_str(), error.c_str());

  mri.reset(the_target->createMCRegInfo(the_triple));
  if (!mri)
    return createStringError(inconvertibleErrorCode(),
                             "Failed to create MCRegisterInfo for %s",
                             triple_name.c_str());

  // TODO(Ilyagavrilin): add CPU selection or remove this feature
  std::string cpu_name = "";

  mai.reset(the_target->createMCAsmInfo(*mri, the_triple, MCTargetOptions()));
  if (!mai)
    return createStringError(inconvertibleErrorCode(),
                             "Failed to create MCAsmInfo for %s",
                             triple_name.c_str());

  Expected<SubtargetFeatures> features_or_err = get_disasm_features();
  if (features_or_err.takeError()) {
    return createStringError(inconvertibleErrorCode(),
                             "Unable to get features for triple %s",
                             triple_name.c_str());
  }
  SubtargetFeatures features = features_or_err.get();

  sti.reset(the_target->createMCSubtargetInfo(the_triple, cpu_name,
                                              features.getString()));
  if (!sti)
    return createStringError(inconvertibleErrorCode(),
                             "Failed to create MCSubtargetInfo for %s",
                             triple_name.c_str());

  mii.reset(the_target->createMCInstrInfo());
  if (!mii)
    return createStringError(inconvertibleErrorCode(),
                             "Failed to create MCInstrInfo for %s",
                             triple_name.c_str());

  ctx = std::make_unique<MCContext>(the_triple, *mai.get(), *mri.get(),
                                    *sti.get());
  dis_asm.reset(the_target->createMCDisassembler(*sti, *ctx));
  if (!dis_asm)
    return createStringError(inconvertibleErrorCode(),
                             "Failed to create MCDisassembler for %s",
                             triple_name.c_str());

  inst_printer.reset(
      the_target->createMCInstPrinter(the_triple, 0, *mai, *mii, *mri));
  if (!inst_printer)
    return createStringError(inconvertibleErrorCode(),
                             "Failed to create MCinst_printer for %s",
                             triple_name.c_str());

  inst_printer->setPrintImmHex(true);
  inst_printer->setPrintBranchImmAsAddress(true);

  initialized = true;
  return Error::success();
}

Error elf_disassembler::disassemble_section(StringRef section_name,
                                            raw_ostream &os, bool show_bytes) {
  if (!initialized) {
    if (Error err = initialize())
      return err;
  }

  SectionRef section;
  if (Error err = loader.get_section_by_name(section_name, section))
    return err;

  Expected<StringRef> contents_or_err = loader.get_section_contents(section);
  if (!contents_or_err)
    return contents_or_err.takeError();

  StringRef contents = contents_or_err.get();
  uint64_t section_addr = section.getAddress();

  ArrayRef<uint8_t> bytes(reinterpret_cast<const uint8_t *>(contents.data()),
                          contents.size());

  return disassemble_bytes(bytes, section_addr, os, show_bytes);
}

Error elf_disassembler::dump_section(StringRef section_name, raw_ostream &os,
                                     bool show_address) {
  SectionRef section;
  if (Error err = loader.get_section_by_name(section_name, section))
    return err;

  Expected<StringRef> contents_or_err = loader.get_section_contents(section);
  if (!contents_or_err)
    return contents_or_err.takeError();

  StringRef contents = contents_or_err.get();
  uint64_t section_addr = section.getAddress();

  ArrayRef<uint8_t> bytes(reinterpret_cast<const uint8_t *>(contents.data()),
                          contents.size());

  // Dump bytes in 16-byte rows
  const size_t bytes_per_row = 16;
  char ascii_bytes[bytes_per_row + 1];
  ascii_bytes[bytes_per_row] = '\0';

  os << "contents of section " << section_name << " (" << bytes.size()
     << " bytes):\n";

  for (size_t offset = 0; offset < bytes.size(); offset += bytes_per_row) {
    if (show_address)
      os << format_hex(section_addr + offset, 10) << ": ";

    // Print hex bytes
    for (size_t i = 0; i < bytes_per_row; ++i) {
      if (offset + i < bytes.size()) {
        uint8_t byte = bytes[offset + i];
        os << format_hex_no_prefix(byte, 2) << " ";
        ascii_bytes[i] = isPrint(byte) ? byte : '.';
      } else {
        os << "   "; // 3 spaces for alignment
        ascii_bytes[i] = ' ';
      }
    }

    // Print ASCII representation
    os << " " << ascii_bytes << "\n";
  }

  return Error::success();
}

Error elf_disassembler::disassemble_bytes(ArrayRef<uint8_t> bytes,
                                          uint64_t address, raw_ostream &os,
                                          bool show_bytes) {
  if (!initialized) {
    if (Error err = initialize())
      return err;
  }

  // Disassemble each instruction
  uint64_t size;
  MCInst inst;

  for (size_t index = 0; index < bytes.size();) {
    os << format_hex(address, 10) << ": ";

    if (dis_asm->getInstruction(inst, size, bytes.slice(index), address,
                                nulls())) {
      if (show_bytes) {
        for (size_t i = 0; i < size && (index + i) < bytes.size(); ++i)
          os << format_hex_no_prefix(bytes[index + i], 2) << " ";

        // Padding for alignment (assuming max 8 bytes per instruction)
        if (size < 8)
          os.indent(3 * (8 - size));
      }

      inst_printer->printInst(&inst, address, "", *sti, os);
      os << "\n";

      index += size;
      address += size;
    } else {
      // Invalid instruction - print one byte and continue
      if (show_bytes)
        os << format_hex_no_prefix(bytes[index], 2) << "                      ";

      os << "<invalid>\n";
      ++index;
      ++address;
    }
  }

  return Error::success();
}

Expected<SubtargetFeatures> elf_disassembler::get_disasm_features() const {
  Expected<Triple> triple_or_err = loader.get_triple();
  if (triple_or_err.takeError()) {
    return createStringError(inconvertibleErrorCode(),
                             "Unable to get target triple");
  }
  Triple the_triple = triple_or_err.get();
  std::string the_triple_str = the_triple.normalize();

  Expected<SubtargetFeatures> features_or_err = loader.get_features();
  if (features_or_err.takeError()) {
    return createStringError(inconvertibleErrorCode(),
                             "Unable to get features for triple %s",
                             the_triple_str.c_str());
  }
  SubtargetFeatures features = features_or_err.get();

  switch (the_triple.getArch()) {
  case llvm::Triple::riscv64:
  case llvm::Triple::riscv32:
    return features;
  // Features for not RISC-V architectures are not so important
  case llvm::Triple::aarch64:
    return SubtargetFeatures("+all");
  case llvm::Triple::x86_64:
    return SubtargetFeatures("+nopl");
  default:
    return createStringError(inconvertibleErrorCode(),
                             "llvm-allm does not have support for %s",
                             the_triple_str.c_str());
  }
}
} // namespace mctomir
