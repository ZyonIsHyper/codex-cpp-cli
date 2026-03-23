#include "app.hpp"

#include "codex_cpp/codex_cpp.h"

#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>

namespace {

thread_local std::string last_error_message;

[[nodiscard]] char *duplicate_c_string(std::string_view value) {
    auto buffer = std::make_unique<char[]>(value.size() + 1);
    if (!value.empty()) {
        std::memcpy(buffer.get(), value.data(), value.size());
    }
    buffer[value.size()] = '\0';
    return buffer.release();
}

void set_last_error(std::string message) {
    last_error_message = std::move(message);
}

} // namespace

int codex_cpp_prompt(const char *prompt, char **response_out) {
    if (response_out == nullptr) {
        set_last_error("response_out must not be null");
        return 1;
    }
    *response_out = nullptr;

    if (prompt == nullptr) {
        set_last_error("prompt must not be null");
        return 1;
    }

    try {
        *response_out = duplicate_c_string(codex_cpp::prompt_once(prompt));
        last_error_message.clear();
        return 0;
    } catch (const std::bad_alloc &) {
        set_last_error("out of memory");
    } catch (const std::exception &error) {
        set_last_error(error.what());
    } catch (...) {
        set_last_error("unknown error");
    }

    return 1;
}

int codex_cpp_set_api_key(const char *api_key) {
    if (api_key == nullptr) {
        set_last_error("api_key must not be null");
        return 1;
    }

    try {
        codex_cpp::set_api_key(api_key);
        last_error_message.clear();
        return 0;
    } catch (const std::bad_alloc &) {
        set_last_error("out of memory");
    } catch (const std::exception &error) {
        set_last_error(error.what());
    } catch (...) {
        set_last_error("unknown error");
    }

    return 1;
}

int codex_cpp_has_credentials(void) {
    try {
        last_error_message.clear();
        return codex_cpp::has_credentials() ? 1 : 0;
    } catch (const std::bad_alloc &) {
        set_last_error("out of memory");
    } catch (const std::exception &error) {
        set_last_error(error.what());
    } catch (...) {
        set_last_error("unknown error");
    }

    return -1;
}

int codex_cpp_logout(void) {
    try {
        codex_cpp::logout();
        last_error_message.clear();
        return 0;
    } catch (const std::bad_alloc &) {
        set_last_error("out of memory");
    } catch (const std::exception &error) {
        set_last_error(error.what());
    } catch (...) {
        set_last_error("unknown error");
    }

    return 1;
}

void codex_cpp_free_string(char *value) {
    delete[] value;
}

const char *codex_cpp_last_error(void) {
    return last_error_message.c_str();
}

const char *codex_cpp_version(void) {
    return codex_cpp::version().data();
}
