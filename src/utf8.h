#ifndef UTF8_H
#define UTF8_H

#include <stdbool.h>
#include <stddef.h>

bool utf8_validate(const char *s, size_t len);
bool utf8_validate_c_string(const char *s);

#endif
