#include <llvm/ADT/StringExtras.h>
#include <mctomir/symbols.h>

#include <yaml-cpp/emitter.h>
#include <yaml-cpp/emittermanip.h>
#include <yaml-cpp/yaml.h>

namespace YAML {

template <> struct convert<mctomir::function_symbol> {
  static Node encode(const mctomir::function_symbol &func) {
    Node node;
    node["name"] = func.name;
    node["start"] = "0x" + llvm::utohexstr(func.start);
    node["end"] = "0x" + llvm::utohexstr(func.end);
    return node;
  }
};
static YAML::Emitter &operator<<(YAML::Emitter &out,
                                 const mctomir::function_symbol &func) {
  out << YAML::convert<mctomir::function_symbol>::encode(func);
  return out;
}
} // namespace YAML

namespace mctomir {

file_info load_file_info_from_yaml(std::string yaml) {
  auto symbols = YAML::Load(yaml);
  file_info syms;
  for (auto &&symb : symbols) {
    function_symbol syminfo;
    syminfo.name = symb["name"].as<std::string>();
    syminfo.start = symb["start"].as<uint64_t>();
    syminfo.end = symb["end"].as<uint64_t>();
    syms.add(syminfo);
  }
  return syms;
}

std::string save_to_yaml(const file_info &info) {
  YAML::Emitter out;
  out << YAML::BeginSeq;
  for (auto &symb : info)
    out << symb;
  out << YAML::EndSeq;
  return out.c_str();
}

} // namespace mctomir
