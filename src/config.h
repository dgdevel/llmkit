#ifndef CONFIG_H
#define CONFIG_H

#include "llmkit.h"

int config_load(const char *path, runtime_ctx *ctx);
void config_free(runtime_ctx *ctx);

#endif
