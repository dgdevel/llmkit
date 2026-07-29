#ifndef AGENT_H
#define AGENT_H

#include "llmkit.h"

#define RUN_MODE_QUIET  "quiet"
#define RUN_MODE_DEBUG  "debug"
#define RUN_MODE_STREAM "stream"

int agent_run(runtime_ctx *ctx, const char *convo_path, const char *prompt, const char *run_mode,
              bool steering);

#endif
