#include "app.hpp"

#include "json.hpp"
#include "version.hpp"

#include <cpr/cpr.h>
#include <windows.h>
#include <shellapi.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace codex_cpp {
namespace {

constexpr const char *k_default_model = "gpt-5.2";
constexpr const char *k_default_api_base = "https://api.openai.com/v1";
constexpr const char *k_default_reasoning_effort = "medium";
constexpr const char *k_default_auth_issuer = "https://auth.openai.com";
constexpr const char *k_openai_oauth_client_id = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr const char *k_default_system_prompt =
    "You are Codex CPP, a helpful coding assistant working in a terminal. "
    "Be concise, practical, and explicit about risks.";

struct global_options {
    std::string model = k_default_model;
    std::string api_base = k_default_api_base;
    std::string system_prompt = k_default_system_prompt;
    std::string reasoning_effort = k_default_reasoning_effort;
    std::filesystem::path data_dir;
    bool verbose = false;
};

enum class command_type {
    interactive,
    exec,
    login,
    logout,
    resume,
    fork,
    features,
    completion,
    sandbox,
    help,
    version,
};

struct parsed_command {
    command_type type = command_type::interactive;
    global_options global;
    std::string prompt;
    bool exec_json = false;
    bool login_status = false;
    bool login_use_device_auth = false;
    bool login_read_stdin = false;
    std::string login_api_key;
    std::string session_id;
    bool use_last_session = false;
    std::string features_action;
    std::string feature_name;
    std::string completion_shell = "powershell";
    bool sandbox_full_auto = false;
    std::vector<std::string> sandbox_command;
};

struct features {
    bool persist_sessions = true;
    bool color_output = true;
    bool verbose_http = false;
    bool web_search = false;
};

struct config {
    std::string model = k_default_model;
    std::string api_base = k_default_api_base;
    std::string system_prompt = k_default_system_prompt;
    std::string reasoning_effort = k_default_reasoning_effort;
    features feature_flags;
};

struct message {
    std::string role;
    std::string text;
};

struct session_record {
    std::string session_id;
    std::string title;
    std::string created_at;
    std::string forked_from;
    std::string model;
    std::string system_prompt;
    std::string last_response_id;
    std::vector<message> transcript;
};

struct response_result {
    std::string response_id;
    std::string output_text;
    json raw;
};

struct http_response {
    unsigned long status = 0;
    std::string body;
};

struct stored_auth {
    std::string auth_mode;
    std::string openai_api_key;
    std::string id_token;
    std::string access_token;
    std::string refresh_token;
    std::string account_id;
};

struct device_code_info {
    std::string verification_url;
    std::string user_code;
    std::string device_auth_id;
    unsigned int interval_seconds = 5;
};

struct device_token_poll_result {
    std::string authorization_code;
    std::string code_verifier;
};

struct oauth_tokens {
    std::string id_token;
    std::string access_token;
    std::string refresh_token;
};

[[nodiscard]] int run_foreground_process(const std::wstring &command_line);
[[nodiscard]] int
run_foreground_process_with_stdin(const std::wstring &command_line, const std::string &stdin_text);
[[nodiscard]] int
run_foreground_process(const std::filesystem::path &application, const std::vector<std::string> &arguments);
[[nodiscard]] int run_foreground_process_with_stdin(
    const std::filesystem::path &application, const std::vector<std::string> &arguments,
    const std::string &stdin_text
);

[[nodiscard]] std::wstring utf8_to_wide(const std::string &value) {
    if (value.empty()) {
        return L"";
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("failed to convert UTF-8 to UTF-16");
    }
    std::wstring result(static_cast<std::size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    return result;
}

[[nodiscard]] std::string wide_to_utf8(const std::wstring &value) {
    if (value.empty()) {
        return "";
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("failed to convert UTF-16 to UTF-8");
    }
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}

[[noreturn]] void fail(const std::string &message) {
    throw std::runtime_error(message);
}

[[nodiscard]] std::optional<std::string> get_env_var(const char *name) {
    char *value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    if (result.empty()) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] bool env_flag_enabled(const char *name) {
    const auto value = get_env_var(name);
    if (!value.has_value()) {
        return false;
    }

    std::string normalized = *value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

[[nodiscard]] std::filesystem::path default_data_dir() {
    if (const auto override_dir = get_env_var("CODEX_CPP_DATA_DIR"); override_dir.has_value()) {
        return std::filesystem::path(*override_dir);
    }

    const auto user_profile = get_env_var("USERPROFILE");
    if (!user_profile.has_value()) {
        fail("USERPROFILE is not set");
    }
    return std::filesystem::path(*user_profile) / ".codex-cpp";
}

[[nodiscard]] std::filesystem::path config_path(const std::filesystem::path &data_dir) {
    return data_dir / "config.json";
}

[[nodiscard]] std::filesystem::path credentials_path(const std::filesystem::path &data_dir) {
    return data_dir / "credentials.bin";
}

[[nodiscard]] std::filesystem::path auth_path(const std::filesystem::path &data_dir) {
    return data_dir / "auth.json";
}

[[nodiscard]] std::filesystem::path official_codex_auth_path() {
    const auto user_profile = get_env_var("USERPROFILE");
    if (!user_profile.has_value()) {
        fail("USERPROFILE is not set");
    }
    return std::filesystem::path(*user_profile) / ".codex" / "auth.json";
}

[[nodiscard]] bool has_account_login(const stored_auth &auth) {
    return !auth.access_token.empty() || !auth.refresh_token.empty() || !auth.id_token.empty();
}

[[nodiscard]] std::filesystem::path sessions_dir(const std::filesystem::path &data_dir) {
    return data_dir / "sessions";
}

void ensure_data_layout(const std::filesystem::path &data_dir) {
    std::filesystem::create_directories(data_dir);
    std::filesystem::create_directories(sessions_dir(data_dir));
}

[[nodiscard]] std::string
join_strings(const std::vector<std::string> &parts, const std::string &separator = " ") {
    std::ostringstream output;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index != 0) {
            output << separator;
        }
        output << parts[index];
    }
    return output.str();
}

[[nodiscard]] std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

[[nodiscard]] std::string narrow_title(const std::string &prompt) {
    const std::string cleaned = prompt.substr(0, std::min<std::size_t>(prompt.size(), 72));
    return cleaned.empty() ? "New session" : cleaned;
}

[[nodiscard]] std::string timestamp_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time_value = std::chrono::system_clock::to_time_t(now);
    std::tm local_time{};
    localtime_s(&local_time, &time_value);
    std::ostringstream output;
    output << std::put_time(&local_time, "%Y-%m-%dT%H:%M:%S");
    return output.str();
}

[[nodiscard]] std::string timestamp_now_utc() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time_value = std::chrono::system_clock::to_time_t(now);
    std::tm utc_time{};
    gmtime_s(&utc_time, &time_value);
    std::ostringstream output;
    output << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

[[nodiscard]] std::string session_id_now() {
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    std::mt19937_64 rng(static_cast<std::mt19937_64::result_type>(now));
    std::uniform_int_distribution<unsigned int> dist(0, 15);
    std::ostringstream output;
    output << "session-"
           << std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()
              )
                  .count()
           << "-";
    for (int i = 0; i < 8; ++i) {
        output << std::hex << dist(rng);
    }
    return output.str();
}

[[nodiscard]] std::string read_file_utf8(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail("failed to open " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void write_file_utf8(const std::filesystem::path &path, const std::string &content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        fail("failed to write " + path.string());
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
}

[[nodiscard]] std::string base64_url_decode(std::string value) {
    std::replace(value.begin(), value.end(), '-', '+');
    std::replace(value.begin(), value.end(), '_', '/');
    while (value.size() % 4 != 0) {
        value.push_back('=');
    }

    std::string decoded;
    decoded.reserve((value.size() * 3) / 4);

    auto decode_char = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') {
            return ch - 'A';
        }
        if (ch >= 'a' && ch <= 'z') {
            return ch - 'a' + 26;
        }
        if (ch >= '0' && ch <= '9') {
            return ch - '0' + 52;
        }
        if (ch == '+') {
            return 62;
        }
        if (ch == '/') {
            return 63;
        }
        if (ch == '=') {
            return -2;
        }
        return -1;
    };

    for (std::size_t index = 0; index < value.size(); index += 4) {
        const int a = decode_char(value[index]);
        const int b = decode_char(value[index + 1]);
        const int c = decode_char(value[index + 2]);
        const int d = decode_char(value[index + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1) {
            fail("invalid base64url payload");
        }

        const unsigned int block =
            (static_cast<unsigned int>(a) << 18) | (static_cast<unsigned int>(b) << 12) |
            (static_cast<unsigned int>(c < 0 ? 0 : c) << 6) | static_cast<unsigned int>(d < 0 ? 0 : d);

        decoded.push_back(static_cast<char>((block >> 16) & 0xFF));
        if (c != -2) {
            decoded.push_back(static_cast<char>((block >> 8) & 0xFF));
        }
        if (d != -2) {
            decoded.push_back(static_cast<char>(block & 0xFF));
        }
    }

    return decoded;
}

[[nodiscard]] std::optional<std::string> account_id_from_id_token(const std::string &id_token) {
    const std::size_t first_dot = id_token.find('.');
    if (first_dot == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t second_dot = id_token.find('.', first_dot + 1);
    if (second_dot == std::string::npos) {
        return std::nullopt;
    }

    const std::string payload = base64_url_decode(id_token.substr(first_dot + 1, second_dot - first_dot - 1));
    const json payload_json = json::parse(payload);
    if (!payload_json.contains("https://api.openai.com/auth")) {
        return std::nullopt;
    }
    const json &auth_claims = payload_json.at("https://api.openai.com/auth");
    if (!auth_claims.is_object() || !auth_claims.contains("chatgpt_account_id") ||
        !auth_claims.at("chatgpt_account_id").is_string()) {
        return std::nullopt;
    }
    return auth_claims.at("chatgpt_account_id").get_ref<const std::string &>();
}

[[nodiscard]] json features_to_json(const features &features) {
    json::object_t object;
    object["persist_sessions"] = json(features.persist_sessions);
    object["color_output"] = json(features.color_output);
    object["verbose_http"] = json(features.verbose_http);
    object["web_search"] = json(features.web_search);
    return json(std::move(object));
}

[[nodiscard]] features features_from_json(const json &json) {
    features parsed_features;
    if (!json.is_object()) {
        return parsed_features;
    }
    if (json.contains("persist_sessions") && json.at("persist_sessions").is_boolean()) {
        parsed_features.persist_sessions = json.at("persist_sessions").get<bool>();
    }
    if (json.contains("color_output") && json.at("color_output").is_boolean()) {
        parsed_features.color_output = json.at("color_output").get<bool>();
    }
    if (json.contains("verbose_http") && json.at("verbose_http").is_boolean()) {
        parsed_features.verbose_http = json.at("verbose_http").get<bool>();
    }
    if (json.contains("web_search") && json.at("web_search").is_boolean()) {
        parsed_features.web_search = json.at("web_search").get<bool>();
    }
    return parsed_features;
}

[[nodiscard]] config load_config(const std::filesystem::path &data_dir) {
    ensure_data_layout(data_dir);
    config loaded_config;
    const auto config_file_path = config_path(data_dir);
    if (!std::filesystem::exists(config_file_path)) {
        return loaded_config;
    }

    const json root = json::parse(read_file_utf8(config_file_path));
    if (root.contains("model") && root.at("model").is_string()) {
        loaded_config.model = root.at("model").get_ref<const std::string &>();
    }
    if (root.contains("api_base") && root.at("api_base").is_string()) {
        loaded_config.api_base = root.at("api_base").get_ref<const std::string &>();
    }
    if (root.contains("system_prompt") && root.at("system_prompt").is_string()) {
        loaded_config.system_prompt = root.at("system_prompt").get_ref<const std::string &>();
    }
    if (root.contains("reasoning_effort") && root.at("reasoning_effort").is_string()) {
        loaded_config.reasoning_effort = root.at("reasoning_effort").get_ref<const std::string &>();
    }
    if (root.contains("features")) {
        loaded_config.feature_flags = features_from_json(root.at("features"));
    }
    return loaded_config;
}

void save_config(const std::filesystem::path &data_dir, const config &config) {
    ensure_data_layout(data_dir);
    json::object_t root;
    root["model"] = json(config.model);
    root["api_base"] = json(config.api_base);
    root["system_prompt"] = json(config.system_prompt);
    root["reasoning_effort"] = json(config.reasoning_effort);
    root["features"] = features_to_json(config.feature_flags);
    write_file_utf8(config_path(data_dir), json(std::move(root)).dump(2));
}

[[nodiscard]] std::vector<unsigned char> read_binary_file(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail("failed to open " + path.string());
    }
    return std::vector<unsigned char>(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()
    );
}

void write_binary_file(const std::filesystem::path &path, const std::vector<unsigned char> &bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        fail("failed to write " + path.string());
    }
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void save_auth(const std::filesystem::path &data_dir, const stored_auth &auth) {
    ensure_data_layout(data_dir);
    json::object_t root;
    root["auth_mode"] = json(auth.auth_mode);
    root["OPENAI_API_KEY"] = json(auth.openai_api_key);

    json::object_t tokens;
    tokens["id_token"] = json(auth.id_token);
    tokens["access_token"] = json(auth.access_token);
    tokens["refresh_token"] = json(auth.refresh_token);
    if (!auth.account_id.empty()) {
        tokens["account_id"] = json(auth.account_id);
    }
    root["tokens"] = json(std::move(tokens));
    root["last_refresh"] = json(timestamp_now_utc());
    write_file_utf8(auth_path(data_dir), json(std::move(root)).dump(2));
}

[[nodiscard]] std::optional<stored_auth> load_stored_auth_file(const std::filesystem::path &path) {
    if (!std::filesystem::exists(path)) {
        return std::nullopt;
    }

    const json root = json::parse(read_file_utf8(path));
    stored_auth auth;
    if (root.contains("auth_mode") && root.at("auth_mode").is_string()) {
        auth.auth_mode = root.at("auth_mode").get_ref<const std::string &>();
    }
    if (root.contains("OPENAI_API_KEY") && root.at("OPENAI_API_KEY").is_string()) {
        auth.openai_api_key = root.at("OPENAI_API_KEY").get_ref<const std::string &>();
    }
    if (root.contains("tokens") && root.at("tokens").is_object()) {
        const json &tokens = root.at("tokens");
        if (tokens.contains("id_token") && tokens.at("id_token").is_string()) {
            auth.id_token = tokens.at("id_token").get_ref<const std::string &>();
        }
        if (tokens.contains("access_token") && tokens.at("access_token").is_string()) {
            auth.access_token = tokens.at("access_token").get_ref<const std::string &>();
        }
        if (tokens.contains("refresh_token") && tokens.at("refresh_token").is_string()) {
            auth.refresh_token = tokens.at("refresh_token").get_ref<const std::string &>();
        }
        if (tokens.contains("account_id") && tokens.at("account_id").is_string()) {
            auth.account_id = tokens.at("account_id").get_ref<const std::string &>();
        }
    }
    if (auth.account_id.empty() && !auth.id_token.empty()) {
        auth.account_id = account_id_from_id_token(auth.id_token).value_or("");
    }
    return auth;
}

[[nodiscard]] std::optional<stored_auth> load_stored_auth(const std::filesystem::path &data_dir) {
    return load_stored_auth_file(auth_path(data_dir));
}

[[nodiscard]] std::optional<stored_auth> load_available_auth(const std::filesystem::path &data_dir) {
    if (const auto local_auth = load_stored_auth(data_dir); local_auth.has_value()) {
        return local_auth;
    }
    return load_stored_auth_file(official_codex_auth_path());
}

void save_api_key(const std::filesystem::path &data_dir, const std::string &api_key) {
    ensure_data_layout(data_dir);
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(api_key.data()));
    input.cbData = static_cast<DWORD>(api_key.size());

    DATA_BLOB output{};
    if (CryptProtectData(&input, L"codex-cpp-api-key", nullptr, nullptr, nullptr, 0, &output) == FALSE) {
        fail("failed to encrypt API key");
    }

    std::vector<unsigned char> bytes(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    write_binary_file(credentials_path(data_dir), bytes);
}

[[nodiscard]] std::optional<std::string> load_api_key(const std::filesystem::path &data_dir) {
    const bool disable_external_credentials = env_flag_enabled("CODEX_CPP_DISABLE_EXTERNAL_CREDENTIALS");

    if (!disable_external_credentials) {
        if (const auto env_key = get_env_var("OPENAI_API_KEY"); env_key.has_value()) {
            return env_key;
        }
    }

    if (const auto auth = load_stored_auth(data_dir); auth.has_value() && !auth->openai_api_key.empty()) {
        return auth->openai_api_key;
    }

    if (!disable_external_credentials) {
        if (const auto auth = load_stored_auth_file(official_codex_auth_path());
            auth.has_value() && !auth->openai_api_key.empty()) {
            return auth->openai_api_key;
        }
    }

    const auto path = credentials_path(data_dir);
    if (!std::filesystem::exists(path)) {
        return std::nullopt;
    }

    auto bytes = read_binary_file(path);
    DATA_BLOB input{};
    input.pbData = bytes.data();
    input.cbData = static_cast<DWORD>(bytes.size());

    DATA_BLOB output{};
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output) == FALSE) {
        fail("failed to decrypt stored API key");
    }

    const std::string api_key(
        reinterpret_cast<const char *>(output.pbData),
        reinterpret_cast<const char *>(output.pbData) + output.cbData
    );
    LocalFree(output.pbData);
    return api_key;
}

void delete_api_key(const std::filesystem::path &data_dir) {
    std::error_code error;
    std::filesystem::remove(credentials_path(data_dir), error);
    std::filesystem::remove(auth_path(data_dir), error);
}

[[nodiscard]] std::optional<std::filesystem::path>
find_executable_on_path(const std::wstring &executable_name) {
    const DWORD required = SearchPathW(nullptr, executable_name.c_str(), nullptr, 0, nullptr, nullptr);
    if (required == 0) {
        return std::nullopt;
    }

    std::wstring buffer(static_cast<std::size_t>(required), L'\0');
    const DWORD written =
        SearchPathW(nullptr, executable_name.c_str(), nullptr, required, buffer.data(), nullptr);
    if (written == 0 || written >= required) {
        return std::nullopt;
    }

    buffer.resize(written);
    return std::filesystem::path(buffer);
}

[[nodiscard]] std::optional<std::filesystem::path> find_official_codex_executable() {
    return find_executable_on_path(L"codex.exe");
}

[[nodiscard]] std::optional<std::filesystem::path> find_official_codex_command() {
    if (const auto command_script = find_executable_on_path(L"codex.cmd"); command_script.has_value()) {
        return command_script;
    }
    return find_official_codex_executable();
}

[[nodiscard]] bool can_use_official_codex_runtime(const std::filesystem::path &data_dir) {
    const auto auth = load_available_auth(data_dir);
    return auth.has_value() && has_account_login(*auth) && find_official_codex_command().has_value();
}

[[nodiscard]] std::wstring quote_windows_argument(const std::wstring &argument) {
    if (argument.empty()) {
        return L"\"\"";
    }

    const bool requires_quotes = argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!requires_quotes) {
        return argument;
    }

    std::wstring quoted = L"\"";
    std::size_t backslash_count = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslash_count;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslash_count * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslash_count = 0;
            continue;
        }
        quoted.append(backslash_count, L'\\');
        backslash_count = 0;
        quoted.push_back(ch);
    }
    quoted.append(backslash_count * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

[[nodiscard]] std::wstring
build_command_line(const std::filesystem::path &application, const std::vector<std::string> &arguments) {
    std::wstring command_line = quote_windows_argument(application.wstring());
    for (const auto &argument : arguments) {
        command_line += L" ";
        command_line += quote_windows_argument(utf8_to_wide(argument));
    }
    return command_line;
}

[[nodiscard]] int run_command_through_cmd(
    const std::filesystem::path &command_path, const std::vector<std::string> &arguments,
    const std::optional<std::string> &stdin_text = std::nullopt
) {
    const auto cmd_executable = find_executable_on_path(L"cmd.exe");
    if (!cmd_executable.has_value()) {
        fail("cmd.exe is not available on PATH");
    }

    std::wstring inner_command = quote_windows_argument(command_path.wstring());
    for (const auto &argument : arguments) {
        inner_command += L" ";
        inner_command += quote_windows_argument(utf8_to_wide(argument));
    }

    const std::wstring cmd_command_line =
        quote_windows_argument(cmd_executable->wstring()) + L" /S /C \"" + inner_command + L"\"";
    if (stdin_text.has_value()) {
        return run_foreground_process_with_stdin(cmd_command_line, *stdin_text);
    }
    return run_foreground_process(cmd_command_line);
}

[[nodiscard]] json message_to_json(const message &message) {
    json::object_t object;
    object["role"] = json(message.role);
    object["text"] = json(message.text);
    return json(std::move(object));
}

[[nodiscard]] message message_from_json(const json &json) {
    message output_message;
    if (json.contains("role") && json.at("role").is_string()) {
        output_message.role = json.at("role").get_ref<const std::string &>();
    }
    if (json.contains("text") && json.at("text").is_string()) {
        output_message.text = json.at("text").get_ref<const std::string &>();
    }
    return output_message;
}

[[nodiscard]] json session_to_json(const session_record &session) {
    json::object_t root;
    root["session_id"] = json(session.session_id);
    root["title"] = json(session.title);
    root["created_at"] = json(session.created_at);
    root["forked_from"] = json(session.forked_from);
    root["model"] = json(session.model);
    root["system_prompt"] = json(session.system_prompt);
    root["last_response_id"] = json(session.last_response_id);

    json::array_t transcript;
    for (const auto &message : session.transcript) {
        transcript.push_back(message_to_json(message));
    }
    root["transcript"] = json(std::move(transcript));
    return json(std::move(root));
}

[[nodiscard]] session_record session_from_json(const json &json) {
    session_record session;
    if (json.contains("session_id") && json.at("session_id").is_string()) {
        session.session_id = json.at("session_id").get_ref<const std::string &>();
    }
    if (json.contains("title") && json.at("title").is_string()) {
        session.title = json.at("title").get_ref<const std::string &>();
    }
    if (json.contains("created_at") && json.at("created_at").is_string()) {
        session.created_at = json.at("created_at").get_ref<const std::string &>();
    }
    if (json.contains("forked_from") && json.at("forked_from").is_string()) {
        session.forked_from = json.at("forked_from").get_ref<const std::string &>();
    }
    if (json.contains("model") && json.at("model").is_string()) {
        session.model = json.at("model").get_ref<const std::string &>();
    }
    if (json.contains("system_prompt") && json.at("system_prompt").is_string()) {
        session.system_prompt = json.at("system_prompt").get_ref<const std::string &>();
    }
    if (json.contains("last_response_id") && json.at("last_response_id").is_string()) {
        session.last_response_id = json.at("last_response_id").get_ref<const std::string &>();
    }
    if (json.contains("transcript") && json.at("transcript").is_array()) {
        for (const auto &item : json.at("transcript").get_ref<const json::array_t &>()) {
            session.transcript.push_back(message_from_json(item));
        }
    }
    return session;
}

[[nodiscard]] std::filesystem::path
session_path(const std::filesystem::path &data_dir, const std::string &session_id) {
    return sessions_dir(data_dir) / (session_id + ".json");
}

void save_session(const std::filesystem::path &data_dir, const session_record &session) {
    ensure_data_layout(data_dir);
    write_file_utf8(session_path(data_dir, session.session_id), session_to_json(session).dump(2));
}

[[nodiscard]] session_record
load_session_by_id(const std::filesystem::path &data_dir, const std::string &session_id) {
    const auto path = session_path(data_dir, session_id);
    if (!std::filesystem::exists(path)) {
        fail("session not found: " + session_id);
    }
    return session_from_json(json::parse(read_file_utf8(path)));
}

[[nodiscard]] std::optional<session_record> load_last_session(const std::filesystem::path &data_dir) {
    const auto sessions_directory = sessions_dir(data_dir);
    if (!std::filesystem::exists(sessions_directory)) {
        return std::nullopt;
    }

    std::optional<std::filesystem::directory_entry> latest_entry;
    for (const auto &entry : std::filesystem::directory_iterator(sessions_directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        if (!latest_entry.has_value() || entry.last_write_time() > latest_entry->last_write_time()) {
            latest_entry = entry;
        }
    }

    if (!latest_entry.has_value()) {
        return std::nullopt;
    }

    return session_from_json(json::parse(read_file_utf8(latest_entry->path())));
}

void collect_output_text(const json &json, std::vector<std::string> &output) {
    if (json.is_object()) {
        const auto &object = json.get_ref<const json::object_t &>();
        const auto type_it = object.find("type");
        const auto text_it = object.find("text");
        if (type_it != object.end() && text_it != object.end() && type_it->second.is_string() &&
            text_it->second.is_string() && type_it->second.get_ref<const std::string &>() == "output_text") {
            output.push_back(text_it->second.get_ref<const std::string &>());
        }
        for (const auto &[_, child] : object) {
            collect_output_text(child, output);
        }
        return;
    }
    if (json.is_array()) {
        for (const auto &child : json.get_ref<const json::array_t &>()) {
            collect_output_text(child, output);
        }
    }
}

[[nodiscard]] std::string error_message_from_body(const std::string &body) {
    try {
        const json body_json = json::parse(body);
        if (body_json.contains("error") && body_json.at("error").is_object()) {
            const auto &error = body_json.at("error");
            if (error.contains("message") && error.at("message").is_string()) {
                return error.at("message").get_ref<const std::string &>();
            }
        }
    } catch (...) {
    }
    return body;
}

[[nodiscard]] http_response http_post_body(
    const std::string &url, const std::string &bearer_token, const std::string &content_type,
    const std::string &body, bool verbose
) {
    cpr::Header headers{{"Content-Type", content_type}};
    if (!bearer_token.empty()) {
        headers.emplace("Authorization", "Bearer " + bearer_token);
    }

    if (verbose) {
        std::cerr << "POST " << url << "\n";
    }

    const cpr::Response response =
        cpr::Post(cpr::Url{url}, headers, cpr::Body{body}, cpr::ConnectTimeout{10000}, cpr::Timeout{300000});
    if (response.error.code != cpr::ErrorCode::OK) {
        fail("HTTP request failed: " + response.error.message);
    }
    return http_response{static_cast<unsigned long>(response.status_code), response.text};
}

[[nodiscard]] http_response http_post_json(
    const std::string &url, const std::string &bearer_token, const std::string &body, bool verbose
) {
    return http_post_body(url, bearer_token, "application/json", body, verbose);
}

[[nodiscard]] response_result create_response(
    const config &config, const std::string &api_key, const std::string &prompt,
    const std::string &previous_response_id, bool verbose
) {
    json::object_t request;
    request["model"] = json(config.model);
    request["input"] = json(prompt);
    request["store"] = json(true);
    request["instructions"] = json(config.system_prompt);

    json::object_t reasoning;
    reasoning["effort"] = json(config.reasoning_effort);
    request["reasoning"] = json(std::move(reasoning));

    if (!previous_response_id.empty()) {
        request["previous_response_id"] = json(previous_response_id);
    }

    const std::string body = json(std::move(request)).dump(-1);
    const http_response response = http_post_json(
        config.api_base + "/responses", api_key, body, verbose || config.feature_flags.verbose_http
    );

    if (response.status < 200 || response.status >= 300) {
        fail(
            "OpenAI API error (" + std::to_string(response.status) +
            "): " + error_message_from_body(response.body)
        );
    }

    const json response_json = json::parse(response.body);
    std::vector<std::string> parts;
    collect_output_text(response_json, parts);

    response_result result;
    result.raw = response_json;
    if (response_json.contains("id") && response_json.at("id").is_string()) {
        result.response_id = response_json.at("id").get_ref<const std::string &>();
    }
    result.output_text = join_strings(parts, "\n");
    return result;
}

[[nodiscard]] config apply_overrides(config config, const global_options &global) {
    config.model = global.model;
    config.api_base = global.api_base;
    config.system_prompt = global.system_prompt;
    config.reasoning_effort = global.reasoning_effort;
    return config;
}

[[nodiscard]] std::string url_encode(const std::string &value) {
    std::ostringstream output;
    output.fill('0');
    output << std::hex << std::uppercase;
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            output << static_cast<char>(ch);
        } else {
            output << '%' << std::setw(2) << static_cast<int>(ch);
        }
    }
    return output.str();
}

[[nodiscard]] int run_foreground_process(const std::wstring &command_line) {
    std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};

    if (CreateProcessW(
            nullptr, mutable_command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup_info,
            &process_info
        ) == FALSE) {
        fail("CreateProcessW failed");
    }

    WaitForSingleObject(process_info.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process_info.hProcess, &exit_code);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return static_cast<int>(exit_code);
}

[[nodiscard]] int
run_foreground_process_with_stdin(const std::wstring &command_line, const std::string &stdin_text) {
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE child_stdin_read = nullptr;
    HANDLE child_stdin_write = nullptr;
    if (CreatePipe(&child_stdin_read, &child_stdin_write, &security_attributes, 0) == FALSE) {
        fail("CreatePipe failed");
    }
    if (SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0) == FALSE) {
        CloseHandle(child_stdin_read);
        CloseHandle(child_stdin_write);
        fail("SetHandleInformation failed");
    }

    std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = child_stdin_read;
    startup_info.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup_info.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION process_info{};
    if (CreateProcessW(
            nullptr, mutable_command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup_info,
            &process_info
        ) == FALSE) {
        CloseHandle(child_stdin_read);
        CloseHandle(child_stdin_write);
        fail("CreateProcessW failed");
    }

    CloseHandle(child_stdin_read);

    DWORD written = 0;
    if (!stdin_text.empty() &&
        WriteFile(
            child_stdin_write, stdin_text.data(), static_cast<DWORD>(stdin_text.size()), &written, nullptr
        ) == FALSE) {
        CloseHandle(child_stdin_write);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        fail("WriteFile failed");
    }
    CloseHandle(child_stdin_write);

    WaitForSingleObject(process_info.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process_info.hProcess, &exit_code);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return static_cast<int>(exit_code);
}

[[nodiscard]] int
run_foreground_process(const std::filesystem::path &application, const std::vector<std::string> &arguments) {
    const std::wstring command_line = build_command_line(application, arguments);
    std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');

    const std::wstring wide_application = application.wstring();

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};

    if (CreateProcessW(
            wide_application.c_str(), mutable_command_line.data(), nullptr, nullptr, TRUE, 0, nullptr,
            nullptr, &startup_info, &process_info
        ) == FALSE) {
        fail("CreateProcessW failed");
    }

    WaitForSingleObject(process_info.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process_info.hProcess, &exit_code);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return static_cast<int>(exit_code);
}

[[nodiscard]] int run_foreground_process_with_stdin(
    const std::filesystem::path &application, const std::vector<std::string> &arguments,
    const std::string &stdin_text
) {
    SECURITY_ATTRIBUTES security_attributes{};
    security_attributes.nLength = sizeof(security_attributes);
    security_attributes.bInheritHandle = TRUE;

    HANDLE child_stdin_read = nullptr;
    HANDLE child_stdin_write = nullptr;
    if (CreatePipe(&child_stdin_read, &child_stdin_write, &security_attributes, 0) == FALSE) {
        fail("CreatePipe failed");
    }
    if (SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0) == FALSE) {
        CloseHandle(child_stdin_read);
        CloseHandle(child_stdin_write);
        fail("SetHandleInformation failed");
    }

    const std::wstring command_line = build_command_line(application, arguments);
    std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');

    const std::wstring wide_application = application.wstring();

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = child_stdin_read;
    startup_info.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup_info.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION process_info{};
    if (CreateProcessW(
            wide_application.c_str(), mutable_command_line.data(), nullptr, nullptr, TRUE, 0, nullptr,
            nullptr, &startup_info, &process_info
        ) == FALSE) {
        CloseHandle(child_stdin_read);
        CloseHandle(child_stdin_write);
        fail("CreateProcessW failed");
    }

    CloseHandle(child_stdin_read);

    DWORD written = 0;
    if (!stdin_text.empty() &&
        WriteFile(
            child_stdin_write, stdin_text.data(), static_cast<DWORD>(stdin_text.size()), &written, nullptr
        ) == FALSE) {
        CloseHandle(child_stdin_write);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        fail("WriteFile failed");
    }
    CloseHandle(child_stdin_write);

    WaitForSingleObject(process_info.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process_info.hProcess, &exit_code);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return static_cast<int>(exit_code);
}

[[nodiscard]] bool import_official_codex_auth(const std::filesystem::path &data_dir) {
    const auto auth = load_stored_auth_file(official_codex_auth_path());
    if (!auth.has_value()) {
        return false;
    }
    save_auth(data_dir, *auth);
    return true;
}

void append_official_codex_common_args(std::vector<std::string> &arguments, const config &config) {
    if (!config.model.empty()) {
        arguments.push_back("-m");
        arguments.push_back(config.model);
    }
    if (config.feature_flags.web_search) {
        arguments.push_back("--search");
    }
}

[[nodiscard]] int run_official_codex_exec(const parsed_command &command, const config &config) {
    const auto runtime_command = find_official_codex_command();
    if (!runtime_command.has_value()) {
        fail("OpenAI account login was found, but no official codex command is available on PATH.");
    }

    std::vector<std::string> arguments;
    arguments.push_back("exec");
    append_official_codex_common_args(arguments, config);
    arguments.push_back("--skip-git-repo-check");
    if (command.exec_json) {
        arguments.push_back("--json");
    }
    arguments.push_back("-");

    if (command.global.verbose) {
        std::cerr << "Using the official codex runtime for OpenAI account auth.\n";
    }
    return run_command_through_cmd(*runtime_command, arguments, command.prompt);
}

[[nodiscard]] int run_official_codex_interactive(
    const parsed_command &command, const config &config, const std::optional<std::string> &subcommand
) {
    const auto runtime_command = find_official_codex_command();
    if (!runtime_command.has_value()) {
        fail("OpenAI account login was found, but no official codex command is available on PATH.");
    }

    std::vector<std::string> arguments;
    if (subcommand.has_value()) {
        arguments.push_back(*subcommand);
    }
    append_official_codex_common_args(arguments, config);
    if (command.use_last_session) {
        arguments.push_back("--last");
    } else if (!command.session_id.empty()) {
        arguments.push_back(command.session_id);
    }
    if (!command.prompt.empty()) {
        arguments.push_back(command.prompt);
    }

    std::cout << "Using the official codex runtime for your OpenAI account login.\n";
    return run_command_through_cmd(*runtime_command, arguments);
}

void open_browser(const std::string &url) {
    const std::wstring wide_url = utf8_to_wide(url);
    const auto result = reinterpret_cast<std::intptr_t>(
        ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL)
    );
    if (result <= 32) {
        std::cerr << "Could not open a browser automatically. Visit this URL manually:\n" << url << "\n";
    }
}

[[nodiscard]] device_code_info request_device_code(bool verbose) {
    json::object_t body;
    body["client_id"] = json(k_openai_oauth_client_id);
    const http_response response = http_post_json(
        std::string(k_default_auth_issuer) + "/api/accounts/deviceauth/usercode", "",
        json(std::move(body)).dump(-1), verbose
    );

    if (response.status < 200 || response.status >= 300) {
        fail(
            "device-code login request failed (" + std::to_string(response.status) +
            "): " + error_message_from_body(response.body)
        );
    }

    const json response_json = json::parse(response.body);
    device_code_info info;
    info.verification_url = std::string(k_default_auth_issuer) + "/codex/device";
    if (response_json.contains("user_code") && response_json.at("user_code").is_string()) {
        info.user_code = response_json.at("user_code").get_ref<const std::string &>();
    } else if (response_json.contains("usercode") && response_json.at("usercode").is_string()) {
        info.user_code = response_json.at("usercode").get_ref<const std::string &>();
    } else {
        fail("device-code login response did not include user_code");
    }
    if (!response_json.contains("device_auth_id") || !response_json.at("device_auth_id").is_string()) {
        fail("device-code login response did not include device_auth_id");
    }
    info.device_auth_id = response_json.at("device_auth_id").get_ref<const std::string &>();
    if (response_json.contains("interval")) {
        if (response_json.at("interval").is_number()) {
            info.interval_seconds = static_cast<unsigned int>(response_json.at("interval").get<double>());
        } else if (response_json.at("interval").is_string()) {
            info.interval_seconds = static_cast<unsigned int>(
                std::stoul(response_json.at("interval").get_ref<const std::string &>())
            );
        }
    }
    if (info.interval_seconds == 0) {
        info.interval_seconds = 5;
    }
    return info;
}

[[nodiscard]] device_token_poll_result
poll_for_device_authorization(const device_code_info &device_code, bool verbose) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(15);

    while (std::chrono::steady_clock::now() < deadline) {
        json::object_t body;
        body["device_auth_id"] = json(device_code.device_auth_id);
        body["user_code"] = json(device_code.user_code);
        const http_response response = http_post_json(
            std::string(k_default_auth_issuer) + "/api/accounts/deviceauth/token", "",
            json(std::move(body)).dump(-1), verbose
        );

        if (response.status >= 200 && response.status < 300) {
            const json response_json = json::parse(response.body);
            if (!response_json.contains("authorization_code") ||
                !response_json.at("authorization_code").is_string()) {
                fail("device-auth completion response missing authorization_code");
            }
            if (!response_json.contains("code_verifier") || !response_json.at("code_verifier").is_string()) {
                fail("device-auth completion response missing code_verifier");
            }
            device_token_poll_result result;
            result.authorization_code = response_json.at("authorization_code").get_ref<const std::string &>();
            result.code_verifier = response_json.at("code_verifier").get_ref<const std::string &>();
            return result;
        }

        if (response.status == 403 || response.status == 404) {
            std::this_thread::sleep_for(std::chrono::seconds(device_code.interval_seconds));
            continue;
        }

        fail(
            "device-auth polling failed (" + std::to_string(response.status) +
            "): " + error_message_from_body(response.body)
        );
    }

    fail("device auth timed out after 15 minutes");
}

[[nodiscard]] oauth_tokens exchange_authorization_code(
    const std::string &authorization_code, const std::string &code_verifier, bool verbose
) {
    const std::string body =
        "grant_type=authorization_code"
        "&code=" +
        url_encode(authorization_code) +
        "&redirect_uri=" + url_encode(std::string(k_default_auth_issuer) + "/deviceauth/callback") +
        "&client_id=" + url_encode(k_openai_oauth_client_id) + "&code_verifier=" + url_encode(code_verifier);

    const http_response response = http_post_body(
        std::string(k_default_auth_issuer) + "/oauth/token", "", "application/x-www-form-urlencoded", body,
        verbose
    );

    if (response.status < 200 || response.status >= 300) {
        fail(
            "OAuth token exchange failed (" + std::to_string(response.status) +
            "): " + error_message_from_body(response.body)
        );
    }

    const json response_json = json::parse(response.body);
    oauth_tokens tokens;
    if (response_json.contains("id_token") && response_json.at("id_token").is_string()) {
        tokens.id_token = response_json.at("id_token").get_ref<const std::string &>();
    }
    if (response_json.contains("access_token") && response_json.at("access_token").is_string()) {
        tokens.access_token = response_json.at("access_token").get_ref<const std::string &>();
    }
    if (response_json.contains("refresh_token") && response_json.at("refresh_token").is_string()) {
        tokens.refresh_token = response_json.at("refresh_token").get_ref<const std::string &>();
    }
    if (tokens.id_token.empty() || tokens.access_token.empty() || tokens.refresh_token.empty()) {
        fail("OAuth token exchange returned an incomplete token set");
    }
    return tokens;
}

[[nodiscard]] std::string exchange_id_token_for_api_key(const std::string &id_token, bool verbose) {
    const std::string body = "grant_type=" + url_encode("urn:ietf:params:oauth:grant-type:token-exchange") +
                             "&client_id=" + url_encode(k_openai_oauth_client_id) +
                             "&requested_token=" + url_encode("openai-api-key") +
                             "&subject_token=" + url_encode(id_token) +
                             "&subject_token_type=" + url_encode("urn:ietf:params:oauth:token-type:id_token");

    const http_response response = http_post_body(
        std::string(k_default_auth_issuer) + "/oauth/token", "", "application/x-www-form-urlencoded", body,
        verbose
    );

    if (response.status < 200 || response.status >= 300) {
        fail(
            "OpenAI API token exchange failed (" + std::to_string(response.status) +
            "): " + error_message_from_body(response.body)
        );
    }

    const json response_json = json::parse(response.body);
    if (!response_json.contains("access_token") || !response_json.at("access_token").is_string()) {
        fail("OpenAI API token exchange did not return access_token");
    }
    return response_json.at("access_token").get_ref<const std::string &>();
}

[[nodiscard]] std::string
consume_value(const std::vector<std::string> &args, std::size_t &index, const std::string &flag_name) {
    if (index + 1 >= args.size()) {
        fail("missing value for " + flag_name);
    }
    ++index;
    return args[index];
}

[[nodiscard]] parsed_command parse_command_line(int argc, char **argv) {
    parsed_command command;
    command.global.data_dir = default_data_dir();

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    std::size_t index = 0;
    while (index < args.size()) {
        const std::string &arg = args[index];
        if (arg == "-h" || arg == "--help") {
            command.type = command_type::help;
            return command;
        }
        if (arg == "--version") {
            command.type = command_type::version;
            return command;
        }
        if (arg == "--model") {
            command.global.model = consume_value(args, index, "--model");
            ++index;
            continue;
        }
        if (arg == "--system") {
            command.global.system_prompt = consume_value(args, index, "--system");
            ++index;
            continue;
        }
        if (arg == "--api-base") {
            command.global.api_base = consume_value(args, index, "--api-base");
            ++index;
            continue;
        }
        if (arg == "--reasoning-effort") {
            command.global.reasoning_effort = consume_value(args, index, "--reasoning-effort");
            ++index;
            continue;
        }
        if (arg == "--data-dir") {
            command.global.data_dir = consume_value(args, index, "--data-dir");
            ++index;
            continue;
        }
        if (arg == "--verbose") {
            command.global.verbose = true;
            ++index;
            continue;
        }
        break;
    }

    if (index >= args.size()) {
        command.type = command_type::interactive;
        return command;
    }

    const std::string subcommand = args[index++];
    if (subcommand == "exec") {
        command.type = command_type::exec;
        while (index < args.size()) {
            if (args[index] == "--json") {
                command.exec_json = true;
            } else {
                command.prompt = join_strings(
                    std::vector<std::string>(args.begin() + static_cast<long long>(index), args.end())
                );
                break;
            }
            ++index;
        }
        if (command.prompt.empty()) {
            fail("exec requires a prompt");
        }
        return command;
    }

    if (subcommand == "login") {
        command.type = command_type::login;
        if (index < args.size() && args[index] == "status") {
            command.login_status = true;
            return command;
        }
        while (index < args.size()) {
            if (args[index] == "--api-key") {
                command.login_api_key = consume_value(args, index, "--api-key");
            } else if (args[index] == "--with-api-key") {
                command.login_read_stdin = true;
            } else if (args[index] == "--device-auth") {
                command.login_use_device_auth = true;
            } else {
                fail("unknown login argument: " + args[index]);
            }
            ++index;
        }
        return command;
    }

    if (subcommand == "logout") {
        command.type = command_type::logout;
        return command;
    }

    if (subcommand == "resume") {
        command.type = command_type::resume;
        while (index < args.size()) {
            if (args[index] == "--last") {
                command.use_last_session = true;
            } else if (command.session_id.empty()) {
                command.session_id = args[index];
            } else {
                fail("unexpected resume argument: " + args[index]);
            }
            ++index;
        }
        if (command.session_id.empty() && !command.use_last_session) {
            command.use_last_session = true;
        }
        return command;
    }

    if (subcommand == "fork") {
        command.type = command_type::fork;
        while (index < args.size()) {
            if (args[index] == "--last") {
                command.use_last_session = true;
            } else if (command.session_id.empty()) {
                command.session_id = args[index];
            } else {
                fail("unexpected fork argument: " + args[index]);
            }
            ++index;
        }
        if (command.session_id.empty() && !command.use_last_session) {
            command.use_last_session = true;
        }
        return command;
    }

    if (subcommand == "features") {
        command.type = command_type::features;
        if (index >= args.size()) {
            fail("features requires list, enable, or disable");
        }
        command.features_action = args[index++];
        if ((command.features_action == "enable" || command.features_action == "disable") &&
            index < args.size()) {
            command.feature_name = args[index++];
        }
        if ((command.features_action == "enable" || command.features_action == "disable") &&
            command.feature_name.empty()) {
            fail("features " + command.features_action + " requires a feature name");
        }
        return command;
    }

    if (subcommand == "completion") {
        command.type = command_type::completion;
        if (index < args.size()) {
            command.completion_shell = to_lower(args[index]);
        }
        return command;
    }

    if (subcommand == "sandbox") {
        command.type = command_type::sandbox;
        if (index >= args.size() || args[index] != "windows") {
            fail("sandbox currently supports only: sandbox windows ...");
        }
        ++index;
        while (index < args.size()) {
            if (args[index] == "--full-auto") {
                command.sandbox_full_auto = true;
            } else {
                command.sandbox_command.assign(args.begin() + static_cast<long long>(index), args.end());
                break;
            }
            ++index;
        }
        if (command.sandbox_command.empty()) {
            fail("sandbox windows requires a command to run");
        }
        return command;
    }

    command.type = command_type::interactive;
    command.prompt =
        join_strings(std::vector<std::string>(args.begin() + static_cast<long long>(index - 1), args.end()));
    return command;
}

void print_help() {
    std::cout << "codex-cpp " << version() << "\n\n"
              << "Usage:\n"
              << "  codex-cpp [GLOBAL_OPTIONS] [PROMPT]\n"
              << "  codex-cpp [GLOBAL_OPTIONS] <COMMAND> [ARGS]\n\n"
              << "Global options:\n"
              << "  --model MODEL               Override the default model\n"
              << "  --system TEXT               Override the system prompt\n"
              << "  --api-base URL              Override the OpenAI API base URL\n"
              << "  --reasoning-effort LEVEL    Set reasoning effort (low|medium|high)\n"
              << "  --data-dir PATH             Override the local state directory\n"
              << "  --verbose                   Print HTTP request progress\n"
              << "  -h, --help                  Show this help\n"
              << "  --version                   Show the version\n\n"
              << "Commands:\n"
              << "  exec <PROMPT>               Run one non-interactive turn\n"
              << "  login [status]              Sign in with your OpenAI account or inspect credentials\n"
              << "  logout                      Delete stored credentials\n"
              << "  resume [SESSION_ID|--last]  Continue a saved interactive session\n"
              << "  fork [SESSION_ID|--last]    fork from a saved interactive session\n"
              << "  features list               Show feature toggles\n"
              << "  features enable NAME        Enable a feature\n"
              << "  features disable NAME       Disable a feature\n"
              << "  completion [shell]          Print a shell completion script\n"
              << "  sandbox windows CMD...      Run a command via the Windows runner\n\n"
              << "login examples:\n"
              << "  codex-cpp login\n"
              << "  codex-cpp login --device-auth\n"
              << "  printenv OPENAI_API_KEY | codex-cpp login --with-api-key\n"
              << "  codex-cpp login --api-key sk-...\n\n"
              << "interactive slash commands:\n"
              << "  /help  /history  /exit\n";
}

void print_completion(const std::string &shell) {
    if (shell == "powershell" || shell == "pwsh") {
        std::cout << "@'\n"
                  << "Register-ArgumentCompleter -Native -CommandName codex-cpp -ScriptBlock {\n"
                  << "    param($wordToComplete, $commandAst, $cursorPosition)\n"
                  << "    'exec','login','logout','resume','fork','features','completion','sandbox' |\n"
                  << "        Where-object { $_ -like \"$wordToComplete*\" } |\n"
                  << "        ForEach-object {\n"
                  << "            [System.Management.Automation.CompletionResult]::new($_, $_, "
                     "'ParameterValue', $_)\n"
                  << "        }\n"
                  << "}\n"
                  << "'@\n";
        return;
    }
    if (shell == "bash") {
        std::cout << "_codex_cpp_complete() {\n"
                  << "  local cur=\"${COMP_WORDS[COMP_CWORD]}\"\n"
                  << "  COMPREPLY=( $(compgen -W \"exec login logout resume fork features completion "
                     "sandbox\" -- \"$cur\") )\n"
                  << "}\n"
                  << "complete -F _codex_cpp_complete codex-cpp\n";
        return;
    }

    fail("unsupported completion shell: " + shell);
}

[[nodiscard]] std::string read_api_key_from_stdin() {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();
    std::string value = buffer.str();
    std::erase(value, '\r');
    while (!value.empty() && (value.back() == '\n' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

void handle_login(const parsed_command &command) {
    if (command.login_status) {
        const bool has_env = get_env_var("OPENAI_API_KEY").has_value();
        auto auth = load_available_auth(command.global.data_dir);
        const bool has_account_session = auth.has_value() && has_account_login(*auth);
        const bool has_responses_bearer = auth.has_value() && !auth->openai_api_key.empty();
        const bool has_stored =
            has_responses_bearer || std::filesystem::exists(credentials_path(command.global.data_dir));
        const bool has_official_runtime = has_account_session && find_official_codex_command().has_value();
        std::cout << "Environment key: " << (has_env ? "yes" : "no") << "\n";
        std::cout << "OpenAI login: " << (has_account_session ? "yes" : "no") << "\n";
        if (has_account_session && !auth->account_id.empty()) {
            std::cout << "Workspace: " << auth->account_id << "\n";
        }
        std::cout << "Responses bearer: " << (has_responses_bearer ? "yes" : "no") << "\n";
        std::cout << "Official runtime: " << (has_official_runtime ? "yes" : "no") << "\n";
        std::cout << "Stored key: " << (has_stored ? "yes" : "no") << "\n";
        return;
    }

    std::string api_key = command.login_api_key;
    if (command.login_read_stdin) {
        api_key = read_api_key_from_stdin();
    }

    if (!api_key.empty()) {
        save_api_key(command.global.data_dir, api_key);
        std::cout << "API key saved to " << credentials_path(command.global.data_dir).string() << "\n";
        return;
    }

    if (!command.login_use_device_auth) {
        std::cout << "Launching official Codex browser login...\n";
        const int exit_code = run_foreground_process(L"cmd.exe /C codex login");
        if (exit_code != 0) {
            fail("official `codex login` failed with exit code " + std::to_string(exit_code));
        }
        if (!import_official_codex_auth(command.global.data_dir)) {
            fail("`codex login` completed, but no auth was available to import from ~/.codex/auth.json");
        }
        const auto imported = load_stored_auth(command.global.data_dir);
        if (!imported.has_value()) {
            fail("`codex login` completed, but the imported auth could not be read back.");
        }
        if (!imported->openai_api_key.empty()) {
            std::cout << "Imported OpenAI login from ~/.codex/auth.json into "
                      << auth_path(command.global.data_dir).string() << "\n";
            return;
        }
        if (!has_account_login(*imported)) {
            fail("OpenAI login completed, but no reusable account session or Responses bearer was imported.");
        }
        std::cout << "Imported OpenAI login from ~/.codex/auth.json into "
                  << auth_path(command.global.data_dir).string() << "\n";
        std::cout
            << "No native Responses bearer was present, so codex-cpp will use the official codex runtime"
            << " when you run commands with this account login.\n";
        return;
    }

    const device_code_info device_code = request_device_code(command.global.verbose);
    std::cout << "Follow these steps to sign in with your OpenAI account:\n\n";
    std::cout << "1. Open this URL in your browser:\n   " << device_code.verification_url << "\n\n";
    std::cout << "2. Enter this one-time code:\n   " << device_code.user_code << "\n\n";
    std::cout << "The code expires in about 15 minutes.\n";
    open_browser(device_code.verification_url);

    const device_token_poll_result poll_result =
        poll_for_device_authorization(device_code, command.global.verbose);
    const oauth_tokens tokens = exchange_authorization_code(
        poll_result.authorization_code, poll_result.code_verifier, command.global.verbose
    );
    const std::string exchanged_api_key =
        exchange_id_token_for_api_key(tokens.id_token, command.global.verbose);

    stored_auth auth;
    auth.auth_mode = "chatgpt";
    auth.openai_api_key = exchanged_api_key;
    auth.id_token = tokens.id_token;
    auth.access_token = tokens.access_token;
    auth.refresh_token = tokens.refresh_token;
    auth.account_id = account_id_from_id_token(tokens.id_token).value_or("");
    save_auth(command.global.data_dir, auth);
    {
        std::error_code cleanup_error;
        std::filesystem::remove(credentials_path(command.global.data_dir), cleanup_error);
    }

    if (auth.openai_api_key.empty()) {
        fail(
            "Device authentication succeeded, but the token exchange did not yield a Responses API bearer. "
            "Run `codex-cpp login` without `--device-auth` to use the official browser login flow instead."
        );
    }

    std::cout << "Successfully logged in using your OpenAI account.\n";
    std::cout << "Saved auth to " << auth_path(command.global.data_dir).string() << "\n";
}

void handle_logout(const parsed_command &command) {
    delete_api_key(command.global.data_dir);
    std::cout << "Stored credentials removed.\n";
}

[[nodiscard]] int run_sandbox_command(const parsed_command &command) {
    std::cout << "Running via custom Windows command launcher";
    if (command.sandbox_full_auto) {
        std::cout << " (--full-auto accepted)";
    }
    std::cout << ".\n";
    std::cout
        << "Note: this build does not yet apply the full restricted-token sandbox from the Rust CLI.\n\n";

    const std::string command_line_utf8 = join_strings(command.sandbox_command, " ");
    std::wstring command_line = utf8_to_wide(command_line_utf8);
    std::vector<wchar_t> mutable_command_line(command_line.begin(), command_line.end());
    mutable_command_line.push_back(L'\0');

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};

    if (CreateProcessW(
            nullptr, mutable_command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup_info,
            &process_info
        ) == FALSE) {
        fail("CreateProcessW failed");
    }

    WaitForSingleObject(process_info.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process_info.hProcess, &exit_code);
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return static_cast<int>(exit_code);
}

void print_assistant_message(const std::string &text) {
    std::cout << "\nassistant> " << text << "\n\n";
}

[[nodiscard]] session_record new_session(const config &config, const std::string &title) {
    session_record session;
    session.session_id = session_id_now();
    session.title = narrow_title(title);
    session.created_at = timestamp_now();
    session.model = config.model;
    session.system_prompt = config.system_prompt;
    return session;
}

void print_history(const session_record &session) {
    std::cout << "\nSession: " << session.title << " (" << session.session_id << ")\n";
    for (const auto &message : session.transcript) {
        std::cout << message.role << "> " << message.text << "\n";
    }
    std::cout << "\n";
}

[[nodiscard]] int run_exec(const parsed_command &command) {
    config config = apply_overrides(load_config(command.global.data_dir), command.global);
    auto api_key = load_api_key(command.global.data_dir);
    if (!api_key.has_value()) {
        if (can_use_official_codex_runtime(command.global.data_dir)) {
            return run_official_codex_exec(command, config);
        }
        fail("no credentials found. Run `codex-cpp login` or set OPENAI_API_KEY.");
    }

    const response_result result =
        create_response(config, *api_key, command.prompt, "", command.global.verbose);

    if (command.exec_json) {
        std::cout << result.raw.dump(2) << "\n";
    } else {
        std::cout << result.output_text << "\n";
    }
    return 0;
}

[[nodiscard]] session_record
resolve_session(const parsed_command &command, const config &config, bool fork_mode) {
    std::optional<session_record> loaded;
    if (command.use_last_session) {
        loaded = load_last_session(command.global.data_dir);
    } else if (!command.session_id.empty()) {
        loaded = load_session_by_id(command.global.data_dir, command.session_id);
    }
    if (!loaded.has_value()) {
        fail("no saved session found");
    }

    if (!fork_mode) {
        return *loaded;
    }

    session_record forked = *loaded;
    forked.forked_from = loaded->session_id;
    forked.session_id = session_id_now();
    forked.created_at = timestamp_now();
    forked.model = config.model;
    forked.system_prompt = config.system_prompt;
    return forked;
}

[[nodiscard]] int
run_interactive(const parsed_command &command, std::optional<session_record> session_override) {
    config config = apply_overrides(load_config(command.global.data_dir), command.global);
    auto api_key = load_api_key(command.global.data_dir);
    if (!api_key.has_value()) {
        const std::optional<std::string> subcommand =
            command.type == command_type::resume ? std::optional<std::string>("resume")
            : command.type == command_type::fork ? std::optional<std::string>("fork")
                                                 : std::nullopt;
        if (can_use_official_codex_runtime(command.global.data_dir)) {
            return run_official_codex_interactive(command, config, subcommand);
        }
        fail("no credentials found. Run `codex-cpp login` or set OPENAI_API_KEY.");
    }

    session_record session =
        session_override.has_value()
            ? *session_override
            : new_session(config, command.prompt.empty() ? "interactive session" : command.prompt);

    if (session.model.empty()) {
        session.model = config.model;
    }
    if (session.system_prompt.empty()) {
        session.system_prompt = config.system_prompt;
    }

    std::cout << "codex-cpp interactive mode\n";
    std::cout << "model: " << config.model << "\n";
    std::cout << "session: " << session.session_id << "\n";
    std::cout << "type /help for commands\n";

    auto send_prompt = [&](const std::string &prompt) {
        session.transcript.push_back(message{"user", prompt});
        const response_result result =
            create_response(config, *api_key, prompt, session.last_response_id, command.global.verbose);
        session.last_response_id = result.response_id;
        session.transcript.push_back(message{"assistant", result.output_text});
        print_assistant_message(result.output_text);
        if (config.feature_flags.persist_sessions) {
            save_session(command.global.data_dir, session);
        }
    };

    if (!command.prompt.empty()) {
        send_prompt(command.prompt);
    }

    while (true) {
        std::cout << "you> ";
        std::string line;
        if (!std::getline(std::cin, line)) {
            std::cout << "\n";
            break;
        }
        if (line.empty()) {
            continue;
        }
        if (line == "/exit" || line == "/quit") {
            break;
        }
        if (line == "/help") {
            std::cout
                << "/help  Show commands\n/history  Print the current transcript\n/exit  Leave the session\n";
            continue;
        }
        if (line == "/history") {
            print_history(session);
            continue;
        }
        send_prompt(line);
    }

    if (config.feature_flags.persist_sessions) {
        save_session(command.global.data_dir, session);
        std::cout << "Saved session: " << session.session_id << "\n";
    }
    return 0;
}

[[nodiscard]] int run_features(const parsed_command &command) {
    config config = load_config(command.global.data_dir);
    constexpr auto valid_features = std::to_array<std::string_view>({
        "persist_sessions",
        "color_output",
        "verbose_http",
        "web_search",
    });

    if (command.features_action == "list") {
        std::cout << "persist_sessions: " << (config.feature_flags.persist_sessions ? "enabled" : "disabled")
                  << "\n";
        std::cout << "color_output: " << (config.feature_flags.color_output ? "enabled" : "disabled") << "\n";
        std::cout << "verbose_http: " << (config.feature_flags.verbose_http ? "enabled" : "disabled") << "\n";
        std::cout << "web_search: " << (config.feature_flags.web_search ? "enabled" : "disabled") << "\n";
        return 0;
    }

    if (!std::ranges::contains(valid_features, std::string_view{command.feature_name})) {
        fail("unknown feature: " + command.feature_name);
    }

    const bool enable = command.features_action == "enable";
    if (command.feature_name == "persist_sessions") {
        config.feature_flags.persist_sessions = enable;
    } else if (command.feature_name == "color_output") {
        config.feature_flags.color_output = enable;
    } else if (command.feature_name == "verbose_http") {
        config.feature_flags.verbose_http = enable;
    } else if (command.feature_name == "web_search") {
        config.feature_flags.web_search = enable;
    } else {
        fail("unsupported features action: " + command.features_action);
    }

    save_config(command.global.data_dir, config);
    std::cout << command.feature_name << ": " << (enable ? "enabled" : "disabled") << "\n";
    return 0;
}

} // namespace

std::string prompt_once(std::string_view prompt) {
    if (prompt.empty()) {
        fail("prompt must not be empty");
    }

    global_options global;
    global.data_dir = default_data_dir();
    ensure_data_layout(global.data_dir);

    config effective_config = apply_overrides(load_config(global.data_dir), global);
    auto api_key = load_api_key(global.data_dir);
    if (!api_key.has_value()) {
        if (can_use_official_codex_runtime(global.data_dir)) {
            fail(
                "codex_cpp_prompt requires a reusable API key or Responses bearer. "
                "Set OPENAI_API_KEY or run `codex-cpp login --device-auth`."
            );
        }
        fail("no credentials found. Run `codex-cpp login` or set OPENAI_API_KEY.");
    }

    const response_result result =
        create_response(effective_config, *api_key, std::string(prompt), "", global.verbose);
    return result.output_text;
}

void set_api_key(std::string_view api_key) {
    if (api_key.empty()) {
        fail("api_key must not be empty");
    }

    const auto data_dir = default_data_dir();
    ensure_data_layout(data_dir);
    save_api_key(data_dir, std::string(api_key));
}

bool has_credentials() {
    const auto data_dir = default_data_dir();
    return load_api_key(data_dir).has_value();
}

void logout() {
    const auto data_dir = default_data_dir();
    ensure_data_layout(data_dir);
    delete_api_key(data_dir);
}

int application::run(int argc, char **argv) const {
    try {
        parsed_command command = parse_command_line(argc, argv);
        ensure_data_layout(command.global.data_dir);

        switch (command.type) {
        case command_type::help:
            print_help();
            return 0;
        case command_type::version:
            std::cout << version() << "\n";
            return 0;
        case command_type::login:
            handle_login(command);
            return 0;
        case command_type::logout:
            handle_logout(command);
            return 0;
        case command_type::exec:
            return run_exec(command);
        case command_type::resume: {
            config config = apply_overrides(load_config(command.global.data_dir), command.global);
            if (!load_api_key(command.global.data_dir).has_value() &&
                can_use_official_codex_runtime(command.global.data_dir)) {
                return run_official_codex_interactive(command, config, std::string("resume"));
            }
            return run_interactive(command, resolve_session(command, config, false));
        }
        case command_type::fork: {
            config config = apply_overrides(load_config(command.global.data_dir), command.global);
            if (!load_api_key(command.global.data_dir).has_value() &&
                can_use_official_codex_runtime(command.global.data_dir)) {
                return run_official_codex_interactive(command, config, std::string("fork"));
            }
            return run_interactive(command, resolve_session(command, config, true));
        }
        case command_type::features:
            return run_features(command);
        case command_type::completion:
            print_completion(command.completion_shell);
            return 0;
        case command_type::sandbox:
            return run_sandbox_command(command);
        case command_type::interactive:
            return run_interactive(command, std::nullopt);
        }
    } catch (const std::exception &error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }

    std::cerr << "error: unhandled command\n";
    return 1;
}

std::string_view version() noexcept {
    return k_version;
}

} // namespace codex_cpp
