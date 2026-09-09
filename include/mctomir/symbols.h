#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ranges = std::ranges;

namespace mctomir {

struct function_symbol final {
  std::string name;
  uint64_t start;
  uint64_t end;
};

class file_info final : private std::vector<function_symbol> {
public:
  file_info() = default;

  void add(const function_symbol &s) { vector::push_back(s); }

  using vector::back;
  using vector::begin;
  using vector::empty;
  using vector::end;
  using vector::front;
  using vector::size;
  auto get_start_of(std::string_view func) const {
    auto found =
        ranges::find_if(*this, [func](auto &fs) { return fs.name == func; });
    if (found == end())
      throw std::runtime_error("Symbol info not found for: " +
                               std::string(func));
    return found->start;
  }
};

file_info load_file_info_from_yaml(std::string yaml);

std::string save_to_yaml(const file_info &);

} // namespace mctomir
