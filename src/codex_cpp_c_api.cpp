#include "app.hpp"

#include "codex_cpp/codex_cpp.h"

int codex_cpp_run(int argc, char **argv) {
    return codex_cpp::application{}.run(argc, argv);
}

const char *codex_cpp_version(void) {
    return codex_cpp::version().data();
}
