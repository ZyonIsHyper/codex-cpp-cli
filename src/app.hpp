#pragma once

#include <string>
#include <string_view>

namespace codex_cpp {

class application final {
  public:
    int run(int argc, char **argv) const;
};

[[nodiscard]] std::string prompt_once(std::string_view prompt);
void set_api_key(std::string_view api_key);
[[nodiscard]] bool has_credentials();
void logout();
[[nodiscard]] std::string_view version() noexcept;

} // namespace codex_cpp
