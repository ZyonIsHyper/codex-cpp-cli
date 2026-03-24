#pragma once

#ifdef __cplusplus
#include <stdexcept>
#include <string>
#include <string_view>
#endif

#if defined(_WIN32) && defined(CODEX_CPP_SHARED)
#if defined(CODEX_CPP_BUILDING_SHARED)
#define CODEX_CPP_API __declspec(dllexport)
#else
#define CODEX_CPP_API __declspec(dllimport)
#endif
#else
#define CODEX_CPP_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

CODEX_CPP_API int codex_cpp_prompt(const char *prompt, char **response_out);
CODEX_CPP_API int codex_cpp_set_api_key(const char *api_key);
CODEX_CPP_API int codex_cpp_has_credentials(void);
CODEX_CPP_API int codex_cpp_logout(void);
CODEX_CPP_API void codex_cpp_free_string(char *value);
CODEX_CPP_API const char *codex_cpp_last_error(void);
CODEX_CPP_API const char *codex_cpp_version(void);

#ifdef __cplusplus
}

// codex_instance is the ergonomic C++ wrapper for the exported C ABI.
// Methods throw std::runtime_error when the underlying C call fails.
class codex_instance final {
  public:
    // Sends one prompt and returns the model text output.
    [[nodiscard]] std::string prompt(std::string_view prompt_text) const {
        std::string prompt_copy(prompt_text);
        char *response = nullptr;
        if (codex_cpp_prompt(prompt_copy.c_str(), &response) != 0) {
            throw std::runtime_error(last_error());
        }

        std::string result = response == nullptr ? std::string{} : std::string(response);
        codex_cpp_free_string(response);
        return result;
    }

    // Persists an API key in the local codex-cpp data directory.
    void set_api_key(std::string_view api_key) const {
        std::string api_key_copy(api_key);
        if (codex_cpp_set_api_key(api_key_copy.c_str()) != 0) {
            throw std::runtime_error(last_error());
        }
    }

    // Returns true when prompt-capable credentials are currently available.
    [[nodiscard]] bool has_credentials() const {
        const int result = codex_cpp_has_credentials();
        if (result < 0) {
            throw std::runtime_error(last_error());
        }
        return result != 0;
    }

    // Removes locally stored credentials from the codex-cpp data directory.
    void logout() const {
        if (codex_cpp_logout() != 0) {
            throw std::runtime_error(last_error());
        }
    }

    // Returns the last C API error message that was reported on this thread.
    [[nodiscard]] std::string last_error() const {
        const char *message = codex_cpp_last_error();
        return message == nullptr ? std::string{} : std::string(message);
    }

    // Returns the library version string.
    [[nodiscard]] std::string version() const {
        const char *value = codex_cpp_version();
        return value == nullptr ? std::string{} : std::string(value);
    }
};
#endif
