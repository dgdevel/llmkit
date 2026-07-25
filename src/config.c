#include "config.h"

int config_load(const char *path, runtime_ctx *ctx) {
    (void)path; (void)ctx;
    return EXIT_SUCCESS;
}

void config_free(runtime_ctx *ctx) {
    (void)ctx;
}
