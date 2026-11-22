/* Invite codes for online play: a host's IPv4 address, TCP port and room key in 16 Crockford
 * base32 symbols, shown as "7K3M-Q0ZD-1XAB-9T5C". The title server writes them (server/invite.py,
 * which documents the layout: 32-bit address, 16-bit port, 27-bit key, 5-bit check symbol); the
 * launchers read them. Decoding ignores case, spaces and dashes and reads I and L as 1, O as 0.
 * Plain C, header-only, so the runtime launcher (C) and the window launcher (C++) share it. */
#ifndef GOLF_INVITE_CODE_H
#define GOLF_INVITE_CODE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define GOLF_INVITE_SYMBOLS 16
#define GOLF_INVITE_KEY_BITS 27

typedef struct { uint8_t ip[4]; uint16_t port; uint32_t key; } GolfInvite;

__extension__ typedef unsigned __int128 golf_invite_u128;

static const char golf_invite_alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static inline int golf_invite_symbol(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c == 'I' || c == 'L') c = '1';
    if (c == 'O') c = '0';
    const char *at = c ? strchr(golf_invite_alphabet, c) : NULL;
    return at ? (int)(at - golf_invite_alphabet) : -1;
}

static inline unsigned golf_invite_check(const int *symbols) {
    unsigned sum = 0;
    for (int i = 0; i < 15; ++i) sum += (unsigned)(2 * i + 1) * (unsigned)symbols[i];
    return sum % 32;
}

/* 1 and the fields for a valid code (surrounding spaces, dashes and inner spaces allowed), else 0. */
static inline int golf_invite_decode(const char *text, GolfInvite *out) {
    int symbols[GOLF_INVITE_SYMBOLS], count = 0;
    for (; *text; ++text) {
        if (*text == '-' || *text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') continue;
        const int s = golf_invite_symbol(*text);
        if (s < 0 || count == GOLF_INVITE_SYMBOLS) return 0;
        symbols[count++] = s;
    }
    if (count != GOLF_INVITE_SYMBOLS || golf_invite_check(symbols) != (unsigned)symbols[15]) return 0;
    golf_invite_u128 hi = 0; /* the first 75 bits: address (32), port (16), key (27) */
    for (int i = 0; i < 15; ++i) hi = hi << 5 | (unsigned)symbols[i];
    const uint32_t key = (uint32_t)(hi & ((1u << GOLF_INVITE_KEY_BITS) - 1));
    const uint16_t port = (uint16_t)(hi >> 27);
    const uint32_t ip = (uint32_t)(hi >> 43);
    if (!port || !key) return 0;
    out->ip[0] = (uint8_t)(ip >> 24); out->ip[1] = (uint8_t)(ip >> 16);
    out->ip[2] = (uint8_t)(ip >> 8); out->ip[3] = (uint8_t)ip;
    out->port = port;
    out->key = key;
    return 1;
}

/* 1 when the text has the shape of a code (16 symbols once dashes and spaces go, no dot), so a
 * mistyped code is reported as one instead of being tried as a host name. */
static inline int golf_invite_shaped(const char *text) {
    int count = 0;
    for (; *text; ++text) {
        if (*text == '-' || *text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') continue;
        if (*text == '.' || *text == ':') return 0;
        ++count;
    }
    return count == GOLF_INVITE_SYMBOLS;
}

/* The code for these fields, "XXXX-XXXX-XXXX-XXXX" (20 bytes with the terminator). */
static inline void golf_invite_encode(const GolfInvite *in, char out[20]) {
    const uint64_t ip = (uint64_t)in->ip[0] << 24 | (uint64_t)in->ip[1] << 16 | (uint64_t)in->ip[2] << 8 | in->ip[3];
    const golf_invite_u128 value = (golf_invite_u128)ip << 43 | (uint64_t)in->port << 27 |
                                    (in->key & ((1u << GOLF_INVITE_KEY_BITS) - 1));
    int symbols[GOLF_INVITE_SYMBOLS];
    for (int i = 0; i < 15; ++i) symbols[i] = (int)((value >> (5 * (14 - i))) & 31);
    symbols[15] = (int)golf_invite_check(symbols);
    int n = 0;
    for (int i = 0; i < GOLF_INVITE_SYMBOLS; ++i) {
        if (i && !(i % 4)) out[n++] = '-';
        out[n++] = golf_invite_alphabet[symbols[i]];
    }
    out[n] = 0;
}

/* The room key as --server-key and the X-Golf-Key header carry it: 7 lowercase hex digits. */
static inline void golf_invite_key_text(uint32_t key, char out[8]) { snprintf(out, 8, "%07x", (unsigned)key); }

#endif
