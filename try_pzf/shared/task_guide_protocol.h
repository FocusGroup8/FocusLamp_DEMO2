#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* UTF-8 byte capacity includes a terminator; the limit is Unicode scalar
 * values, not bytes. These checks are shared by the MCP receiver and head. */
#define TG_STEP_COUNT 3
#define TG_STEP_MAX_CHARS 18
#define TG_STEP_BYTES (TG_STEP_MAX_CHARS * 4 + 1)

static inline bool tg_utf8_next(const char **cursor, uint32_t *codepoint)
{
    if (!cursor || !*cursor || !codepoint) return false;
    const unsigned char *p = (const unsigned char *)*cursor;
    uint32_t cp = *p++;
    unsigned tail = 0;
    uint32_t minimum = 0;
    if (cp < 0x80) {
        if (!cp) return false;
    } else if (cp >= 0xc2 && cp <= 0xdf) {
        cp &= 0x1f; tail = 1; minimum = 0x80;
    } else if (cp >= 0xe0 && cp <= 0xef) {
        cp &= 0x0f; tail = 2; minimum = 0x800;
    } else if (cp >= 0xf0 && cp <= 0xf4) {
        cp &= 7; tail = 3; minimum = 0x10000;
    } else return false;
    while (tail--) {
        if ((*p & 0xc0) != 0x80) return false;
        cp = (cp << 6) | (*p++ & 0x3f);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    *cursor = (const char *)p;
    *codepoint = cp;
    return true;
}

static inline bool tg_space(uint32_t cp)
{
    return cp == 0x20 || cp == 0xa0 || cp == 0x1680 ||
        (cp >= 0x2000 && cp <= 0x200a) || cp == 0x202f || cp == 0x205f || cp == 0x3000;
}

static inline bool tg_number(uint32_t cp)
{
    return (cp >= '0' && cp <= '9') || (cp >= 0xff10 && cp <= 0xff19) ||
        cp == 0x4e00 || cp == 0x4e8c || cp == 0x4e09 || cp == 0x56db ||
        cp == 0x4e94 || cp == 0x516d || cp == 0x4e03 || cp == 0x516b ||
        cp == 0x4e5d || cp == 0x5341;
}

static inline const char *tg_step_error(const char *text)
{
    if (!text || !*text) return "Each step must contain 1 to 18 characters";
    size_t bytes = 0;
    while (bytes < TG_STEP_BYTES && text[bytes]) ++bytes;
    if (bytes >= TG_STEP_BYTES) return "Each step must contain 1 to 18 characters";
    const char *cursor = text;
    uint32_t cp = 0, first = 0, second = 0, last = 0;
    unsigned count = 0;
    while (*cursor) {
        if (!tg_utf8_next(&cursor, &cp)) return "Step text must be valid UTF-8";
        if (++count > TG_STEP_MAX_CHARS) return "Each step must contain 1 to 18 characters";
        if (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || cp == 0x2028 || cp == 0x2029 ||
            (cp >= 0x200b && cp <= 0x200f) || cp == 0xfeff)
            return "Steps must not contain line breaks or control characters";
        if (cp == '`' || cp == '*') return "Use plain text without Markdown";
        if (count == 1) first = cp;
        if (count == 2) second = cp;
        last = cp;
    }
    if (tg_space(first) || tg_space(last)) return "Remove leading and trailing whitespace";
    if (first == '#' || first == '>' || first == '-' || first == '+' || first == '_' ||
        first == 0x2022 || first == 0x25cf || first == 0x25a0 ||
        (first >= 0x2460 && first <= 0x2473) || (first >= 0x2776 && first <= 0x277f))
        return "Do not add numbering or bullet prefixes";
    if (tg_number(first) && (second == '.' || second == ')' || second == ':' ||
        second == 0x3001 || second == 0xff09 || second == 0xff0e || second == 0xff1a || tg_space(second)))
        return "Do not add numbering or bullet prefixes";
    return NULL;
}

static inline const char *tg_steps_error(const char *const steps[TG_STEP_COUNT])
{
    if (!steps) return "Exactly three steps are required";
    for (unsigned i = 0; i < TG_STEP_COUNT; ++i) {
        const char *error = tg_step_error(steps[i]);
        if (error) return error;
        for (unsigned j = 0; j < i; ++j)
            if (strcmp(steps[i], steps[j]) == 0) return "The three steps must be different";
    }
    return NULL;
}

static inline bool tg_validate_steps(const char *const steps[TG_STEP_COUNT])
{
    return tg_steps_error(steps) == NULL;
}
