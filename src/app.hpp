#pragma once

#include <string_view>

namespace codex_cpp {

class application final {
  public:
    int run(int argc, char **argv) const;
};

[[nodiscard]] std::string_view version() noexcept;

} // namespace codex_cpp
