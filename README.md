# codex-cpp

`codex-cpp` is a Windows-native C++ console app that mirrors the day-to-day Codex CLI workflow in a smaller, hackable implementation.

It includes:

- interactive chat mode when you run `codex-cpp` with no subcommand
- `exec` for one-shot prompts
- `login`, `logout`, `resume`, `fork`, `features`, `completion`, and `sandbox windows`
- OpenAI account login via the official Codex browser flow by default
- optional `--device-auth` fallback for device-code login
- secure API-key storage on Windows via DPAPI
- session persistence under `%USERPROFILE%\.codex-cpp`
- direct OpenAI Responses API calls via `cpr`

It does not attempt to fully recreate the Rust TUI or internal agent runtime. This build is intentionally a practical console-first implementation.

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

The binary will be written to a configuration-specific build directory such as:

`build\Release\codex-cpp.exe`

## Build options

You can choose which artifacts to generate:

- executable: `-DCODEX_CPP_BUILD_EXE=ON`
- static library: `-DCODEX_CPP_BUILD_STATIC_LIB=ON`
- shared library: `-DCODEX_CPP_BUILD_SHARED_LIB=ON`

Examples:

```powershell
cmake -S . -B build -DCODEX_CPP_BUILD_EXE=OFF -DCODEX_CPP_BUILD_STATIC_LIB=ON
cmake -S . -B build -DCODEX_CPP_BUILD_EXE=OFF -DCODEX_CPP_BUILD_SHARED_LIB=ON
cmake -S . -B build -DCODEX_CPP_BUILD_EXE=ON -DCODEX_CPP_BUILD_STATIC_LIB=ON
```

The public library API is declared in `include\codex_cpp\codex_cpp.h`.

The project fetches `nlohmann/json` and `cpr` at configure time.

## CI

GitHub Actions builds the project on `windows-2022` and verifies:

- the executable target
- the static library target
- the shared library target
- a basic `--version` smoke test

## Quick start

Sign in with your OpenAI account:

```powershell
codex-cpp login
```

Or keep using a raw API key:

```powershell
codex-cpp login --api-key sk-...
```

Run once:

```powershell
codex-cpp exec "Explain what this repository does."
```

Start an interactive session:

```powershell
codex-cpp --model gpt-5.2
```

Resume your most recent session:

```powershell
codex-cpp resume --last
```

## Data layout

`codex-cpp` stores its state here by default:

- config: `%USERPROFILE%\.codex-cpp\config.json`
- account login: `%USERPROFILE%\.codex-cpp\auth.json`
- encrypted credentials: `%USERPROFILE%\.codex-cpp\credentials.bin`
- sessions: `%USERPROFILE%\.codex-cpp\sessions\*.json`

When you use `codex-cpp login` with no flags, it runs the official `codex login` flow and imports the resulting auth into `codex-cpp`.

If that login does not include a native Responses bearer, `codex-cpp` automatically falls back to the official installed `codex` runtime for `exec`, interactive chat, `resume`, and `fork`.

You can override the data root with `--data-dir PATH`.
