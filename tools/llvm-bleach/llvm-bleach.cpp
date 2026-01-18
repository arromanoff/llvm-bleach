#include "bleach/lifter/instr-impl.hpp"
#include "bleach/passes/block-ir-builder.hpp"
#include "bleach/passes/redundant-branch-eraser.hpp"

#include "bleach/version.inc"
#include "mctomir/mctomir-transform.h"
#include "mctomir/symbols.h"

#include <llvm/CodeGen/CommandFlags.h>
#include <llvm/CodeGen/MIRParser/MIRParser.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/CodeGen/MachineModuleInfo.h>
#include <llvm/CodeGen/MachinePassManager.h>
#include <llvm/CodeGen/TargetPassConfig.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IRPrintingPasses.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/StandardInstrumentations.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileOutputBuffer.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/CGPassBuilderOption.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/SROA.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>

using namespace llvm;
static cl::OptionCategory options("llvm-bleach options");
namespace bleach {

static cl::opt<std::string> input_file_name(cl::Positional,
                                            cl::desc("<mir/ELF file>"),
                                            cl::cat(options), cl::init(""),
                                            cl::Required);
static cl::opt<std::string> instructions_file(
    "instructions",
    cl::desc("File with (YAML) descriptions of instructions in LLVM IR"),
    cl::cat(options), cl::init(""));

static cl::opt<bool>
    dump_input_mir("dump-input-mir",
                   cl::desc("Dump input MIR (or MIR of input ELF file)"),
                   cl::cat(options));

static cl::opt<std::string>
    dump_input_instructions("dump-input-instructions",
                            cl::desc("Dump input instructions"),
                            cl::value_desc("filename"), cl::cat(options));

static cl::opt<bool>
    no_inline_opt("noinline-instr",
                  cl::desc("Do not inline call to instruction implementations"),
                  cl::cat(options));
static cl::opt<bool>
    assume_function_nop("assume-function-nop",
                        cl::desc("Assume called functions don't change state"),
                        cl::cat(options));
static cl::opt<std::string>
    dump_struct_def_option("state-struct-file",
                           cl::desc("File to dump state struct definition to"),
                           cl::cat(options));

static cl::opt<unsigned>
    stack_size_option("stack-size", cl::desc("Stack size for array (bytes)"),
                      cl::init(8000), cl::cat(options));

static cl::opt<std::string> symtab_file(
    "symtab-file",
    cl::desc("Optional symtab file. Needed for lifting indirecy branches."),
    cl::cat(options));

static cl::opt<std::string>
    lifted_prefix("lifted-prefix", cl::init("bleached_"),
                  cl::desc("Prefix to append to the name of a lifted function "
                           "(e.g. 'main' -> 'bleached_main')"),
                  cl::cat(options));

static cl::opt<std::string> output_filename("o",
                                            cl::desc("Output LLVM IR file"),
                                            cl::value_desc("filename"),
                                            cl::init("-"), cl::cat(options));

namespace {

class print_pass : public PassInfoMixin<print_pass> {
public:
  PreservedAnalyses run(Module &m, ModuleAnalysisManager &mam) {
    auto &mmi = mam.getResult<MachineModuleAnalysis>(m).getMMI();
    for (auto &f : m) {
      auto &mf = mmi.getOrCreateMachineFunction(f);
      mf.print(outs());
    }
    return PreservedAnalyses::all();
  }
};

bool is_mir_file(const std::filesystem::path &filename) {
  // TODO: implement a better way to determine file type
  return filename.extension().native() == ".mir";
}

struct input_mir final {
  std::unique_ptr<Module> mod;
  std::unique_ptr<TargetMachine> tmachine;
  std::unique_ptr<MachineModuleInfo> mmi;
};

input_mir read_or_translate_mir(const std::filesystem::path &input_file,
                                LLVMContext &ctx) {
  if (!is_mir_file(input_file)) {
    // Treat all non-MIR files as ELFs
    auto translated = mctomir::lift_elf_file(ctx, input_file.native());
    return {.mod = std::move(translated.mod),
            .tmachine = std::move(translated.tmachine),
            .mmi = std::move(translated.mmi)};
  }
  SMDiagnostic err_mir_parser;
  auto mir_parser =
      createMIRParserFromFile(input_file.native(), err_mir_parser, ctx);
  if (!mir_parser)
    throw std::runtime_error(err_mir_parser.getMessage().str());
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
    auto tm = codegen::createTargetMachineForTriple(triple.str());
    if (auto err = tm.takeError())
      throw std::runtime_error(toString(std::move(err)));
    target_machine = std::unique_ptr<TargetMachine>(
        static_cast<TargetMachine *>(tm->release()));
    return target_machine->createDataLayout().getStringRepresentation();
  };
  auto m = mir_parser->parseIRModule(set_data_layout);
  assert(m);
  auto machine_module_info =
      std::make_unique<MachineModuleInfo>(target_machine.get());
  assert(machine_module_info);
  if (mir_parser->parseMachineFunctions(*m, *machine_module_info))
    throw std::runtime_error("Failed to parse machine functions");
  return {.mod = std::move(m),
          .tmachine = std::move(target_machine),
          .mmi = std::move(machine_module_info)};
}

// remove to get read of unitialized global
codegen::RegisterCodeGenFlags cfg;

} // namespace
} // namespace bleach
using namespace bleach;
auto main(int argc, char **argv) -> int try {
  cl::AddExtraVersionPrinter([](raw_ostream &os) {
    os << "Bleach version: " LLVM_BLEACH_VERSION_STRING "\n";
  });
  llvm::setBugReportMsg("PLEASE submit a bug report to "
                        "https://github.com/ajlekcahdp4/llvm-bleach/issues and "
                        "include the crash backtrace.\n");
  cl::ParseCommandLineOptions(argc, argv, "llvm-bleach");
  InitLLVM llvm_stacktrace(argc, argv);
  InitializeAllTargetInfos();
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllDisassemblers();
  LLVMContext ctx;
  auto input_mir = read_or_translate_mir(input_file_name.getValue(), ctx);
  auto &[m, target_machine, machine_module_info] = input_mir;
  assert(machine_module_info);
  if (!instructions_file.getNumOccurrences()) {
    std::cerr << "Skipping instructions parsing since no file were provided\n";
    return EXIT_SUCCESS;
  };
  std::ifstream file(instructions_file.getValue());
  std::string yaml(std::istreambuf_iterator<char>{file},
                   std::istreambuf_iterator<char>{});
  if (!file.good())
    throw std::runtime_error("Failed to open file \"" +
                             instructions_file.getValue() + "\"");
  auto instrs = lifter::load_from_yaml(std::move(yaml), ctx);
  if (dump_input_instructions.getNumOccurrences()) {
    auto yaml_str = lifter::save_to_yaml(instrs);
    auto out_buffer =
        FileOutputBuffer::create(dump_input_instructions, yaml_str.size());
    if (auto err = out_buffer.takeError())
      throw std::runtime_error(toString(std::move(err)));
    auto out = std::move(*out_buffer);
    std::copy(yaml_str.begin(), yaml_str.end(), out->getBufferStart());
    if (auto err = out->commit())
      throw std::runtime_error(toString(std::move(err)));
  }
  std::unique_ptr<mctomir::file_info> finfo;
  if (symtab_file.getNumOccurrences()) {
    std::fstream fs(symtab_file.getValue(), std::ios::in);
    std::istreambuf_iterator<char> begin(fs), end;
    std::string symtab_contents(begin, end);
    finfo = std::make_unique<mctomir::file_info>(
        mctomir::load_file_info_from_yaml(symtab_contents));
  }
  ModuleAnalysisManager mam;
  FunctionAnalysisManager fam;
  LoopAnalysisManager lam;
  CGSCCAnalysisManager cgam;

  ModulePassManager mpm;
  PassBuilder pass_builder;
  // remove to get nullptr dereference
  pass_builder.registerModuleAnalyses(mam);
  pass_builder.registerFunctionAnalyses(fam);
  pass_builder.registerLoopAnalyses(lam);
  pass_builder.registerCGSCCAnalyses(cgam);
  pass_builder.crossRegisterProxies(lam, fam, cgam, mam);
  mam.registerPass([&] { return PassInstrumentationAnalysis(); });
  mam.registerPass([&] { return MachineModuleAnalysis(*machine_module_info); });
  if (dump_input_mir)
    mpm.addPass(print_pass());
  mpm.addPass(bleach::lifter::block_ir_builder_pass(
      instrs, assume_function_nop.getValue(), dump_struct_def_option.getValue(),
      finfo.get(), stack_size_option.getValue(), lifted_prefix.getValue()));
  mpm.addPass(createModuleToFunctionPassAdaptor(
      bleach::lifter::redundant_branch_eraser()));
  if (!no_inline_opt)
    mpm.addPass(createModuleToPostOrderCGSCCPassAdaptor(InlinerPass()));
  mpm.addPass(
      createModuleToFunctionPassAdaptor(SROAPass(SROAOptions::PreserveCFG)));
  mpm.addPass(VerifierPass(false));
  mpm.run(*m, mam);
  if (output_filename == "-")
    m->print(outs(), nullptr);
  else {
    std::error_code errc = {};
    llvm::raw_fd_ostream fs(output_filename, errc);
    if (errc)
      throw std::runtime_error(errc.message());
    m->print(fs, nullptr);
  }
  mam.clear();
  fam.clear();
  cgam.clear();
  lam.clear();
} catch (const std::exception &e) {
  std::cerr << "ERROR: " << e.what() << std::endl;
  return EXIT_FAILURE;
}
