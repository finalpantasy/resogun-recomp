/* Invite codes (src/invite_code.h) agree with the title server's server/invite.py. */
#include <assert.h>
#include <string.h>
#include "invite_code.h"

int main(void) {
    GolfInvite v;
    /* invite.encode("192.168.1.20", 48765, 0x5a3f2c1) in server/invite.py */
    static const char *const same[] = {"R2M0-255Y-FPT7-WP1X", "r2m0 255y fpt7 wp1x", " R2M0255YFPT7WP1X\n", "R2MO-255Y-FPT7-WPIX"};
    for (size_t i = 0; i < sizeof same / sizeof *same; ++i) {
        memset(&v, 0, sizeof v);
        assert(golf_invite_decode(same[i], &v));
        assert(v.ip[0] == 192 && v.ip[1] == 168 && v.ip[2] == 1 && v.ip[3] == 20);
        assert(v.port == 48765 && v.key == 0x5a3f2c1);
    }
    char code[20], key[8];
    golf_invite_encode(&v, code);
    assert(!strcmp(code, "R2M0-255Y-FPT7-WP1X"));
    golf_invite_key_text(v.key, key);
    assert(!strcmp(key, "5a3f2c1"));
    /* Every one-symbol typo fails. */
    const char *plain = "R2M0255YFPT7WP1X";
    for (int i = 0; i < 16; ++i)
        for (const char *c = golf_invite_alphabet; *c; ++c) {
            if (*c == plain[i]) continue;
            char typo[17];
            memcpy(typo, plain, 17);
            typo[i] = *c;
            assert(!golf_invite_decode(typo, &v));
        }
    static const char *const bad[] = {"", "R2M0", "R2M0-255Y-FPT7-WP1", "R2M0-255Y-FPT7-WP1XX", "R2M0-255Y-FPT7-WPUX",
                                      "192.168.1.20:48765", "R2M0_255Y_FPT7_WP1X"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) assert(!golf_invite_decode(bad[i], &v));
    assert(golf_invite_shaped("R2M0-255Y-FPT7-WP1Y") && golf_invite_shaped("r2m0255yfpt7wp1x"));
    assert(!golf_invite_shaped("192.168.1.20") && !golf_invite_shaped("gamingpc") && !golf_invite_shaped("ABCDEFGHJKMNPQRS:1"));
    /* Round trip at the edges. */
    GolfInvite edge = {{255, 255, 255, 255}, 65535, (1u << GOLF_INVITE_KEY_BITS) - 1}, back;
    golf_invite_encode(&edge, code);
    assert(golf_invite_decode(code, &back) && !memcmp(edge.ip, back.ip, 4) && back.port == edge.port && back.key == edge.key);
    return 0;
}
