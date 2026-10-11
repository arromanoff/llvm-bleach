#include "bleach/version.inc"
#include "mctomir/mctomir-transform.h"
#include "mctomir/symbols.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"
#include <llvm/ADT/StringExtras.h>
#include <llvm/CodeGen/Passes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/MCDisassembler/MCDisassembler.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/SymbolicFile.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/WithColor.h>

#include <fstream>
using namespace llvm;
using namespace llvm::object;
using namespace mctomir;
static cl::OptionCategory options("mctomir options");
static cl::opt<std::string>
    input_filename(cl::Positional, cl::desc("<input ELF file>"), cl::Required);
static cl::opt<std::string> output_filename("o", cl::desc("Output MIR file"),
                                            cl::value_desc("filename"),
                                            cl::init("-"), cl::cat(options));
static cl::opt<bool> verbose_output("verbose",
                                    cl::desc("Enable verbose output"),
                                    cl::init(false), cl::cat(options));
static cl::alias verbose_output_alias("v", cl::desc("Alias for --verbose"),
                                      cl::aliasopt(verbose_output),
                                      cl::cat(options));

static cl::opt<std::string>
    symbol_table_file("symtab-file", cl::desc("File to save symtab to."),
                      cl::value_desc("filename"), cl::cat(options));

int main(int argc, char **argv) try {
  cl::AddExtraVersionPrinter([](raw_ostream &os) {
    os << "Bleach version: " LLVM_BLEACH_VERSION_STRING "\n";
  });
  llvm::setBugReportMsg("PLEASE submit a bug report to "
                        "https://github.com/ajlekcahdp4/llvm-bleach/issues and "
                        "include the crash backtrace.\n");
  InitLLVM x(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "LLVM-based ELF-to-MIR converter\n");

  // Initialize all LLVM targets
  InitializeAllTargetInfos();
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmParsers();
  InitializeAllAsmPrinters();
  InitializeAllDisassemblers();

  LLVMContext ctx;

  auto res = lift_elf_file(ctx, input_filename);
  res.mcctx.reset();
  res.mi_analysis.reset();

  if (symbol_table_file.getNumOccurrences()) {
    auto &funcs = res.funcs;
    file_info finfo;
    for (auto &func : funcs)
      finfo.add({func.name, func.start, func.end});
    auto yaml = save_to_yaml(finfo);
    if (symbol_table_file == "-")
      outs() << yaml << '\n';
    else {
      std::fstream fs(symbol_table_file.getValue(), std::ios::out);
      if (!fs.good())
        throw std::runtime_error(
            formatv("Cannot open file \"{}\"", symbol_table_file.getValue()));
      fs << yaml << '\n';
    }
  }
  if (verbose_output)
    outs() << "Writing MIR to: " << output_filename << '\n';
  std::error_code errc;
  llvm::raw_fd_ostream os(output_filename, errc);
  if (errc)
    throw std::runtime_error(errc.message());
  print_mir(os, res);
  return EXIT_SUCCESS;
} catch (std::exception &e) {
  errs() << "ERROR: " << e.what() << '\n';
  return EXIT_FAILURE;
}
