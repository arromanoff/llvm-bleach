#include <cstdint>
extern "C" {

struct bleach_section_t {
  char *data;
  uint64_t start;
  uint64_t size;
};

char *bleach_try_get_real_addr_impl(uint64_t addr,
                                    const bleach_section_t *sections,
                                    std::size_t section_count) {
  for (auto i = 0uz; i < section_count; ++i) {
    auto *s = &sections[i];
    if ((addr >= s->start) && (addr < s->start + s->size)) {
      return s->data + (addr - s->start);
    }
  }
  return nullptr;
}
} // extern "C"
