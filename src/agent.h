#ifndef AGENT_H
#define AGENT_H

#include "llmkit.h"

int agent_run(runtime_ctx *ctx, const char *convo_path, const char *prompt, bool stream);

#endif
