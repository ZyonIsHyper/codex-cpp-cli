#pragma once

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

CODEX_CPP_API int codex_cpp_run(int argc, char **argv);
CODEX_CPP_API const char *codex_cpp_version(void);

#ifdef __cplusplus
}
#endif
