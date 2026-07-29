#ifndef AGENT_H
#define AGENT_H

#include "llmkit.h"

#define OUTPUT_MODE_QUIET  "quiet"
#define OUTPUT_MODE_DEBUG  "debug"
#define OUTPUT_MODE_STREAM "stream"

int agent_run(runtime_ctx *ctx, const char *convo_path, const char *prompt,
              const char *output_mode);

#endif
