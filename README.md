# codex-cpp

`codex-cpp` is a Windows-native C++23 library that exposes Codex-style prompting and credential helpers as static and shared libraries.

It includes:

- a C API for prompt, credential, and version operations
- a C++ `codex_instance` wrapper around the C API
- Windows credential storage via DPAPI
- persistent local state under `%USERPROFILE%\.codex-cpp`
- direct OpenAI Responses API calls via `cpr`

## Requirements

- Windows
- CMake 3.20+
- A C++23-capable compiler
- Visual Studio 2022 or newer is the primary tested path

## Build

From a Visual Studio developer shell:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026"
cmake --build build --config Release
```

You can also use a different generator, such as `Ninja`, if that is what you have installed.

The generated artifacts are written to the configuration-specific build directory, for example:

- `build\Release\codex-cpp-static.lib`
- `build\Release\codex-cpp.dll`
- `build\Release\codex-cpp.lib`

## Build options

You can choose which artifacts to generate:

- static library: `-DCODEX_CPP_BUILD_STATIC_LIB=ON`
- shared library: `-DCODEX_CPP_BUILD_SHARED_LIB=ON`

Examples:

```powershell
cmake -S . -B build -DCODEX_CPP_BUILD_STATIC_LIB=OFF -DCODEX_CPP_BUILD_SHARED_LIB=ON
cmake -S . -B build -DCODEX_CPP_BUILD_STATIC_LIB=ON -DCODEX_CPP_BUILD_SHARED_LIB=OFF
cmake -S . -B build -DCODEX_CPP_BUILD_STATIC_LIB=ON -DCODEX_CPP_BUILD_SHARED_LIB=ON
```

The public library API is declared in `include\codex_cpp\codex_cpp.h`.

The project fetches `nlohmann/json` and `cpr` at configure time.

## Shared library API

The DLL now exposes a prompt-oriented entrypoint instead of forcing library consumers through the CLI-style `argc` and `argv` path:

```c
int codex_cpp_prompt(const char *prompt, char **response_out);
int codex_cpp_set_api_key(const char *api_key);
int codex_cpp_has_credentials(void);
int codex_cpp_logout(void);
void codex_cpp_free_string(char *value);
const char *codex_cpp_last_error(void);
const char *codex_cpp_version(void);
```

For C++ callers, `include\codex_cpp\codex_cpp.h` also exposes a `codex_instance` wrapper class with separate methods like `prompt()`, `set_api_key()`, `has_credentials()`, `logout()`, and `version()`.

Low-level C API example:

```c
char *response = NULL;
if (codex_cpp_prompt("Explain this repository.", &response) == 0) {
    puts(response);
    codex_cpp_free_string(response);
} else {
    fprintf(stderr, "%s\n", codex_cpp_last_error());
}
```

`codex_cpp_prompt` directly calls the library's OpenAI Responses API path. It currently requires a reusable API key or Responses bearer token, such as `OPENAI_API_KEY` or a bearer saved by `codex_cpp_set_api_key`.

## CI

GitHub Actions builds the project on `windows-2022` and verifies:

- the static library target
- the shared library target
- the import library and DLL artifacts

## Data layout

`codex-cpp` stores its state here by default:

- config: `%USERPROFILE%\.codex-cpp\config.json`
- account login: `%USERPROFILE%\.codex-cpp\auth.json`
- encrypted credentials: `%USERPROFILE%\.codex-cpp\credentials.bin`
- sessions: `%USERPROFILE%\.codex-cpp\sessions\*.json`

The library currently exposes prompt and credential operations directly. Credentials can come from `OPENAI_API_KEY`, a stored bearer, or the encrypted key written by `codex_cpp_set_api_key`.
