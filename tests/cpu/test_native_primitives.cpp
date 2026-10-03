#include "me_primitives.h"
#include <cassert>

int main() {
  std::vector<uint32_t> output;
  const auto check = [&](std::vector<uint32_t> input, std::vector<uint32_t> expected) {
    me::native::ConvertFan(input, output);
    assert(output == expected);
  };
  check({}, {});
  check({1, 2}, {});
  check({5, 6, 7, 8}, {5, 6, 7, 5, 7, 8});
  check({1, 2, UINT32_MAX, 3, 4, 5, 6, UINT32_MAX, UINT32_MAX, 7},
        {3, 4, 5, 3, 5, 6});
  check({UINT32_MAX, 9, 8, 7}, {9, 8, 7});
}
