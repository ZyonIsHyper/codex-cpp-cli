#include "codex_cpp/codex_cpp.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::optional<std::string> current_env_value(const char *name) {
#ifdef _WIN32
    char *value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return std::nullopt;
    }

    std::string result(value);
    std::free(value);
    return result;
#else
    if (const char *value = std::getenv(name); value != nullptr) {
        return std::string(value);
    }
    return std::nullopt;
#endif
}

class scoped_env_var final {
  public:
    scoped_env_var(const char *name, std::string value) : name_(name) {
        if (const auto existing = current_env_value(name_); existing.has_value()) {
            has_original_value_ = true;
            original_value_ = *existing;
        }

        set(value);
    }

    ~scoped_env_var() {
        restore();
    }

    scoped_env_var(const scoped_env_var &) = delete;
    scoped_env_var &operator=(const scoped_env_var &) = delete;

  private:
    void set(const std::string &value) const {
#ifdef _WIN32
        _putenv_s(name_, value.c_str());
#else
        setenv(name_, value.c_str(), 1);
#endif
    }

    void restore() const {
        if (has_original_value_) {
            set(original_value_);
            return;
        }

#ifdef _WIN32
        _putenv_s(name_, "");
#else
        unsetenv(name_);
#endif
    }

    const char *name_;
    bool has_original_value_ = false;
    std::string original_value_;
};

class scoped_test_data_dir final {
  public:
    scoped_test_data_dir()
        : path_(
              std::filesystem::temp_directory_path() /
              ("codex-cpp-tests-" +
               std::to_string(std::filesystem::file_time_type::clock::now().time_since_epoch().count()))
          ) {
        std::filesystem::create_directories(path_);
    }

    ~scoped_test_data_dir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept {
        return path_;
    }

  private:
    std::filesystem::path path_;
};

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void test_c_api_error_path() {
    char *response = nullptr;
    const int exit_code = codex_cpp_prompt("", &response);
    expect(exit_code == 1, "codex_cpp_prompt should fail on an empty prompt");
    expect(response == nullptr, "codex_cpp_prompt should not allocate a response on failure");
    expect(codex_cpp_last_error() != nullptr, "codex_cpp_last_error should return a message pointer");
    expect(std::string(codex_cpp_last_error()) == "prompt must not be empty", "unexpected prompt error text");
}

void test_codex_instance_version() {
    const codex_instance instance;
    expect(instance.version() == "0.2.0", "codex_instance::version should match the library version");
}

void test_codex_instance_credential_lifecycle() {
    const codex_instance instance;

    expect(!instance.has_credentials(), "credentials should start empty inside the isolated test data dir");

    instance.set_api_key("sk-test-key");
    expect(instance.has_credentials(), "set_api_key should make credentials visible");

    instance.logout();
    expect(!instance.has_credentials(), "logout should remove locally stored credentials");
}

void test_codex_instance_prompt_exception() {
    const codex_instance instance;

    bool threw = false;
    try {
        static_cast<void>(instance.prompt(""));
    } catch (const std::runtime_error &error) {
        threw = true;
        expect(std::string(error.what()) == "prompt must not be empty", "unexpected prompt exception text");
    }

    expect(threw, "codex_instance::prompt should throw on an empty prompt");
}

} // namespace

int main() {
    try {
        const scoped_test_data_dir test_data_dir;
        const scoped_env_var data_dir_override("CODEX_CPP_DATA_DIR", test_data_dir.path().string());
        const scoped_env_var disable_external_credentials("CODEX_CPP_DISABLE_EXTERNAL_CREDENTIALS", "1");
        const scoped_env_var openai_api_key_override("OPENAI_API_KEY", "");

        test_c_api_error_path();
        test_codex_instance_version();
        test_codex_instance_credential_lifecycle();
        test_codex_instance_prompt_exception();
        std::cout << "All codex-cpp tests passed.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Test failure: " << error.what() << "\n";
        return 1;
    }
}
