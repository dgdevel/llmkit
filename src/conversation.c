#include "conversation.h"

int conversation_open(const char *path, FILE **out_fp) {
    (void)path;
    (void)out_fp;
    return EXIT_SUCCESS;
}

int conversation_write_meta(FILE *fp, const char *config_hash, const char *run_id) {
    (void)fp;
    (void)config_hash;
    (void)run_id;
    return EXIT_SUCCESS;
}

int conversation_write_entry(FILE *fp, entry_type type, ...) {
    (void)fp;
    (void)type;
    return EXIT_SUCCESS;
}

int conversation_reconstruct(const char *path, void **out_msgs, int *out_count) {
    (void)path;
    (void)out_msgs;
    (void)out_count;
    return EXIT_SUCCESS;
}

void conversation_free_messages(void *msgs, int count) {
    (void)msgs;
    (void)count;
}
