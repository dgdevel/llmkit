#ifndef CONVERSATION_H
#define CONVERSATION_H

#include "llmkit.h"

int conversation_open(const char *path, FILE **out_fp);
int conversation_write_meta(FILE *fp, const char *config_hash, const char *run_id);
int conversation_write_entry(FILE *fp, entry_type type, ...);
int conversation_reconstruct(const char *path, void **out_msgs, int *out_count);
void conversation_free_messages(void *msgs, int count);

#endif
