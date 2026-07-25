#include "utf8.h"
#include <string.h>

bool utf8_validate(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 0x7F) {
            i += 1;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (i + 1 >= len) return false;
            if (((unsigned char)s[i + 1] & 0xC0) != 0x80) return false;
            i += 2;
        } else if (c == 0xE0) {
            if (i + 2 >= len) return false;
            unsigned char b1 = (unsigned char)s[i + 1];
            unsigned char b2 = (unsigned char)s[i + 2];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
            if (b1 < 0xA0) return false;
            i += 3;
        } else if (c >= 0xE1 && c <= 0xEC) {
            if (i + 2 >= len) return false;
            if (((unsigned char)s[i + 1] & 0xC0) != 0x80) return false;
            if (((unsigned char)s[i + 2] & 0xC0) != 0x80) return false;
            i += 3;
        } else if (c == 0xED) {
            if (i + 2 >= len) return false;
            unsigned char b1 = (unsigned char)s[i + 1];
            unsigned char b2 = (unsigned char)s[i + 2];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
            if (b1 > 0x9F) return false;
            i += 3;
        } else if (c >= 0xEE && c <= 0xEF) {
            if (i + 2 >= len) return false;
            if (((unsigned char)s[i + 1] & 0xC0) != 0x80) return false;
            if (((unsigned char)s[i + 2] & 0xC0) != 0x80) return false;
            i += 3;
        } else if (c == 0xF0) {
            if (i + 3 >= len) return false;
            unsigned char b1 = (unsigned char)s[i + 1];
            unsigned char b2 = (unsigned char)s[i + 2];
            unsigned char b3 = (unsigned char)s[i + 3];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return false;
            if (b1 < 0x90) return false;
            unsigned long cp = ((unsigned long)(c & 0x07) << 18) |
                               ((unsigned long)(b1 & 0x3F) << 12) |
                               ((unsigned long)(b2 & 0x3F) << 6) | (unsigned long)(b3 & 0x3F);
            if (cp > 0x10FFFF) return false;
            i += 4;
        } else if (c >= 0xF1 && c <= 0xF3) {
            if (i + 3 >= len) return false;
            if (((unsigned char)s[i + 1] & 0xC0) != 0x80) return false;
            if (((unsigned char)s[i + 2] & 0xC0) != 0x80) return false;
            if (((unsigned char)s[i + 3] & 0xC0) != 0x80) return false;
            i += 4;
        } else if (c == 0xF4) {
            if (i + 3 >= len) return false;
            unsigned char b1 = (unsigned char)s[i + 1];
            unsigned char b2 = (unsigned char)s[i + 2];
            unsigned char b3 = (unsigned char)s[i + 3];
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return false;
            if (b1 > 0x8F) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

bool utf8_validate_c_string(const char *s) {
    if (s == NULL) return false;
    return utf8_validate(s, strlen(s));
}
