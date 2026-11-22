/* The game's version, logged first so a bug report's log tells which dump it came from: param.sfo's
 * TITLE_ID and APP_VER, and what scripts/prepare.py records in analysis.json beside the image: the
 * eboot's SHA-256 and the verified build it is (VERIFIED_BUILDS; empty for one prepared with
 * --any-build). */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/mman.h>
#endif
#include "gpu/resogun_gpu.h"

/* The string value of "key" in analysis.json's head (it is written first), at most size-1 chars of
 * [0-9a-z.]; 0 when absent. */
static int report_string(const char *head, const char *key, char *out, size_t size) {
    char pattern[64];
    snprintf(pattern,sizeof(pattern),"\"%s\": \"",key);
    const char *value=strstr(head,pattern);
    if (!value) return 0;
    value+=strlen(pattern);
    const size_t n=strspn(value,"0123456789abcdefghijklmnopqrstuvwxyz.");
    if (n>=size || value[n]!='"') return 0;
    memcpy(out,value,n); out[n]=0;
    return 1;
}

static int clock_foreign; /* Everybody's Golf: no Resogun frame clock to look for */
void runtime_game_identify(const char *title_id, const char *app_version, const char *app0, const char *image_path) {
    (void)app0;
    clock_foreign=!strncmp(title_id,"CUSA04687",9);
    char sha[65]="", build[16]="", path[4096], head[4096];
    const char *slash=strrchr(image_path,'/'), *back=strrchr(image_path,'\\');
    if (back>slash) slash=back;
    const int dir=slash ? (int)(slash-image_path)+1 : 0;
    snprintf(path,sizeof(path),"%.*sanalysis.json",dir,image_path);
    FILE *f=fopen(path,"rb");
    if (f) {
        const size_t n=fread(head,1,sizeof(head)-1,f);
        fclose(f);
        head[n]=0;
        if (!report_string(head,"source_sha256",sha,sizeof(sha)) || strlen(sha)!=64) sha[0]=0;
        report_string(head,"verified_build",build,sizeof(build));
    }
    printf("Game: %s version %s, eboot.bin sha256 %s (%s%s)\n",title_id[0] ? title_id : "?",
           app_version[0] ? app_version : "unknown",sha[0] ? sha : "unknown",
           build[0] ? "verified build " : "not a verified build",build);
}

/* settings.ini frame_rate onto the engine's frame clock (eboot 01.10). The clock (a global at
 * 0x152cd28, vtable 0xc6c2c8) holds the target frame rate at +8; its set_fps (vtable slot 3, the
 * engine_set_fps script command's) also sets the step 1/fps. The game sets 59.94 at boot, runs one
 * update per frame without frame skip, and waits for one vblank a frame; the vblank follows
 * frame_rate (emulator_settings.h), so the target becomes frame_rate*0.999 like the game's 59.94.
 * resogun_frame_rate() is the rate the window's display can show (ResogunSettings::
 * EffectiveFrameRate). Called on the game's thread at each flip, it also restores the rate after
 * the game resets it. */
static unsigned char *game_image;
static int clock_unknown;
static void cheats_sync(void);
void runtime_game_set_image(unsigned char *image) { game_image=image; }
void runtime_game_frame_rate(void) {
    cheats_sync();
    /* The Cheats page holds the game here, before the frame is handed over: nothing moves until
     * it closes, and the presenter shows the last frame under it. Each frame is a fixed step of
     * the game's clock, so the game resumes where it was. */
    while (resogun_game_held()) {
        const struct timespec wait={0,5000000};
        nanosleep(&wait,NULL);
        cheats_sync();
    }
    if (!game_image || clock_unknown) return;
    if (clock_foreign) { /* that address holds other data in another game: do not follow it */
        clock_unknown=1;
        resogun_set_frame_rate(60);
        puts("Frame rate: not Resogun's eboot; the vblank stays 60");
        return;
    }
    float *clock=*(float **)(game_image+0x152cd28);
    if (!clock) return;
    void **vtable=*(void ***)clock;
    if ((unsigned char *)vtable!=game_image+0xc6c2c8) {
        clock_unknown=1;
        resogun_set_frame_rate(60); /* the vblank back to the game's own rate */
        puts("Frame rate: the engine's frame clock is not where eboot 01.10 has it; frame_rate stays 60");
        return;
    }
    const int rate=resogun_frame_rate();
    const float want=(float)rate*0.999f;
    if (fabsf(clock[2]-want)<0.001f) return;
    typedef void (ABI *SetFps)(void *clock, float fps);
    ((SetFps)vtable[3])(clock,want);
    printf("Frame rate: %d FPS (settings.ini frame_rate; engine target %.2f)\n",rate,(double)want);
}

/* A 4:3, 16:10, 21:9 or 32:9 output (eboot 01.10). The engine sizes its display buffers from a
 * table at 0xa5d120 (1920x1080 on PS4, 3840x2160 on PS4 Pro) and takes its aspect ratio from the
 * video out's size (emulator_settings.h, resogun_width_4k).
 * Its render scripts create the scene targets with fixed sizes (mrt_albedo 1920x1080 or 1920x2160
 * checkerboard, SSAO at half, menu_overlay for the pause menu...) through 0x476aa0 (render targets:
 * this, name, format, width, height, options) and 0x478840 and 0x478910 (depth buffers: this,
 * name, width, height, ...); each starts with a jmp to a stub that widens the size before the
 * function's own prologue runs (narrows it, for 4:3 and 16:10). The main menu's panels
 * (menu_slide_*, dummy_depth*) are drawn on 3D cards and keep their size.
 * The engine's perspective takes a vertical half-angle tan(fov/2)/sqrt(1+aspect) (measured: the
 * main menu's and the gameplay camera's matrices at 16:9, 21:9 and 32:9), so a wider picture would
 * also see less above and below: 10% at 21:9, 28% at 32:9, with the HUD and the menus, drawn
 * in 3D before the camera, pushed past the top and bottom. Every camera goes through 0x420bd0
 * (this, view, projection, fov, near, far, aspect) with its projection matrix; for the screen's
 * cameras (the aspect of the picture) a copy with the smaller of 16:9's vertical and horizontal
 * scales is passed instead, and a fov that gives it, so the picture keeps the PS4's view on one
 * axis and shows more on the other. */
uint32_t runtime_game_wide_width(const char *name, uint32_t width, uint32_t height, uint32_t width_4k) {
    if (width_4k==3840 || !width_4k || !height) return width;
    if (name && (!strncmp(name,"menu_slide",10) || !strncmp(name,"dummy_depth",11))) return width;
    if ((uint64_t)width*9!=(uint64_t)height*16 && (uint64_t)width*9!=(uint64_t)height*8) return width;
    uint32_t wide=(uint32_t)(((uint64_t)width*width_4k+3839)/3840);
    return wide+(wide&1);
}

static uint32_t wide_4k;
static int wide_verbose;
enum { WIDE_TARGET=0, WIDE_DEPTH=1 };
/* Called by the stubs with the guest's argument registers as pushed: r11 r10 r9 r8 rdi rsi rdx
 * rcx rax. Render targets have their width in ecx and height in r8d, depth buffers in edx, ecx. */
static ABI void wide_create(uint64_t *regs, uint64_t kind) {
    enum { R8=3, RSI=5, RDX=6, RCX=7 };
    const char *name=(const char *)regs[RSI];
    uint64_t *width=&regs[kind==WIDE_TARGET ? RCX : RDX];
    const uint32_t height=(uint32_t)regs[kind==WIDE_TARGET ? R8 : RCX], before=(uint32_t)*width;
    const uint32_t after=runtime_game_wide_width(name,before,height,wide_4k);
    if (after==before) return;
    *width=after;
    if (wide_verbose) printf("Wide: %s %ux%u -> %ux%u\n",name ? name : "?",before,height,after,height);
}

/* Each hooked function starts with this prologue (push rbp; mov rbp,rsp; push r15..rbx; sub
 * rsp,frame): 17 bytes, none rip-relative, so the stub runs them in place and jumps back. */
static int wide_prologue(const unsigned char *at, unsigned char frame) {
    static const unsigned char prologue[16]={0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec};
    return !memcmp(at,prologue,sizeof(prologue)) && at[16]==frame;
}
static unsigned char *wide_hook(unsigned char *image, unsigned char *stub, uint32_t offset, unsigned kind) {
    static const unsigned char save[]={0x50,0x51,0x52,0x56,0x57,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53, /* push rax..r11 */
                                       0x48,0x89,0xe7};                                                   /* mov rdi, rsp */
    static const unsigned char restore[]={0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5f,0x5e,0x5a,0x59,0x58};
    unsigned char *site=image+offset, *p=stub;
    const uint64_t call=(uint64_t)(uintptr_t)wide_create, back=(uint64_t)(uintptr_t)(site+17), to=(uint64_t)(uintptr_t)stub;
    /* At entry rsp is 8 mod 16; nine pushes make it 16-aligned for the call. */
    memcpy(p,save,sizeof(save)); p+=sizeof(save);
    *p++=0xbe; memcpy(p,&kind,4); p+=4;                     /* mov esi, kind */
    *p++=0x48; *p++=0xb8; memcpy(p,&call,8); p+=8;          /* mov rax, wide_create */
    *p++=0xff; *p++=0xd0;                                   /* call rax */
    memcpy(p,restore,sizeof(restore)); p+=sizeof(restore);
    memcpy(p,site,17); p+=17;                               /* the prologue */
    *p++=0xff; *p++=0x25; memset(p,0,4); p+=4;              /* jmp [rip]: back */
    memcpy(p,&back,8); p+=8;
    site[0]=0xff; site[1]=0x25; memset(site+2,0,4);         /* jmp [rip]: the stub */
    memcpy(site+6,&to,8);
    memset(site+14,0x90,3);
    return p;
}
/* 0x420bd0's projection on the guest's thread: the caller's matrix stays as it is. */
static _Thread_local float wide_projection[16];
/* The stub passes xmm0..xmm3 (fov, near, far, aspect; 16 bytes each) and the argument registers
 * as pushed (r11 r10 r9 r8 rdi rsi rdx rcx rax); changes to either reach the function. */
static ABI void wide_camera(float *xmm, uint64_t *regs) {
    enum { RDX=6 };
    const float aspect=xmm[12], wide=(float)wide_4k/2160.0f;
    const float *m=(const float *)regs[RDX];
    if (!m || fabsf(aspect-wide)>wide*0.001f) return;
    /* The screen's orthographic cameras (w = 1; x within +-aspect, y +-1: menus, backdrops) show
     * more at the sides when wider; narrower, they zoom out to 16:9's width, as the pause menu's
     * layout needs (the menus then keep 16:9 with dark bands above and below). */
    if (m[11]==0.0f && m[15]==1.0f) {
        const float fit=aspect*9.0f/16.0f;
        if (fit>=1.0f) return;
        memcpy(wide_projection,m,sizeof(wide_projection));
        wide_projection[0]*=fit;
        wide_projection[5]*=fit;
        regs[RDX]=(uint64_t)(uintptr_t)wide_projection;
        return;
    }
    /* A perspective (w = z) for the picture's shape: shadow, cube and card cameras differ. */
    if (m[11]!=1.0f || m[15]!=0.0f) return;
    /* Wider than 16:9 keeps its height (more at the sides), narrower its width (more above and
     * below): the smaller zoom keeps both of 16:9's extents in view, the HUD included. */
    const float tall=sqrtf((1.0f+16.0f/9.0f)/(1.0f+aspect)), across=tall*aspect*9.0f/16.0f;
    const float scale=fminf(tall,across);
    memcpy(wide_projection,m,sizeof(wide_projection));
    wide_projection[0]*=scale;
    wide_projection[5]*=scale;
    regs[RDX]=(uint64_t)(uintptr_t)wide_projection;
    xmm[0]=2.0f*atanf(tanf(xmm[0]*0.5f)/scale);
}
/* 0x420bd0 starts with 14 bytes that are not rip-relative: vmovups ymm4,[rdx]; vmovups
 * ymm5,[rdx+0x20]; vxorps xmm10,xmm10,xmm10. */
static const unsigned char camera_head[14]={0xc5,0xfc,0x10,0x22,0xc5,0xfc,0x10,0x6a,0x20,0xc4,0x41,0x28,0x57,0xd2};
static unsigned char *wide_camera_hook(unsigned char *image, unsigned char *stub) {
    static const unsigned char save[]={0x50,0x51,0x52,0x56,0x57,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53, /* push rax..r11 */
        0x48,0x83,0xec,0x40,                                                    /* sub rsp, 0x40 */
        0x0f,0x11,0x04,0x24, 0x0f,0x11,0x4c,0x24,0x10,                         /* movups [rsp..], xmm0..xmm3 */
        0x0f,0x11,0x54,0x24,0x20, 0x0f,0x11,0x5c,0x24,0x30,
        0x48,0x89,0xe7, 0x48,0x8d,0x74,0x24,0x40};                              /* mov rdi, rsp; lea rsi, [rsp+0x40] */
    static const unsigned char restore[]={0x0f,0x10,0x04,0x24, 0x0f,0x10,0x4c,0x24,0x10,
        0x0f,0x10,0x54,0x24,0x20, 0x0f,0x10,0x5c,0x24,0x30, 0x48,0x83,0xc4,0x40,
        0x41,0x5b,0x41,0x5a,0x41,0x59,0x41,0x58,0x5f,0x5e,0x5a,0x59,0x58};
    unsigned char *site=image+0x420bd0, *p=stub;
    const uint64_t call=(uint64_t)(uintptr_t)wide_camera, back=(uint64_t)(uintptr_t)(site+sizeof(camera_head)),
                   to=(uint64_t)(uintptr_t)stub;
    /* Nine pushes and 0x40 bytes keep the call 16-aligned, as in wide_hook. */
    memcpy(p,save,sizeof(save)); p+=sizeof(save);
    *p++=0x48; *p++=0xb8; memcpy(p,&call,8); p+=8;          /* mov rax, wide_camera */
    *p++=0xff; *p++=0xd0;                                   /* call rax */
    memcpy(p,restore,sizeof(restore)); p+=sizeof(restore);
    memcpy(p,camera_head,sizeof(camera_head)); p+=sizeof(camera_head);
    *p++=0xff; *p++=0x25; memset(p,0,4); p+=4;              /* jmp [rip]: back */
    memcpy(p,&back,8); p+=8;
    site[0]=0xff; site[1]=0x25; memset(site+2,0,4);         /* jmp [rip]: the stub */
    memcpy(site+6,&to,8);
    return p;
}

int runtime_game_widen(unsigned char *image, uint32_t width_4k) {
    static const uint32_t table[4]={1920,1080,3840,2160};
    uint32_t *sizes=(uint32_t *)(image+0xa5d120);
    if (memcmp(sizes,table,sizeof(table)) || !wide_prologue(image+0x476aa0,0x28) ||
        !wide_prologue(image+0x478840,0x18) || !wide_prologue(image+0x478910,0x18) ||
        memcmp(image+0x420bd0,camera_head,sizeof(camera_head))) {
        puts("Wide: the engine's sizes are not where eboot 01.10 has them; the picture stays 16:9");
        return 0;
    }
    unsigned char *stub=runtime_low_map(4096,PROT_READ|PROT_WRITE|PROT_EXEC);
    if (!stub) return 0;
    wide_4k=width_4k;
    wide_verbose=runtime_verbose();
    sizes[0]=width_4k/2; sizes[2]=width_4k;
    unsigned char *p=wide_hook(image,stub,0x476aa0,WIDE_TARGET);
    p=wide_hook(image,p,0x478840,WIDE_DEPTH);
    p=wide_hook(image,p,0x478910,WIDE_DEPTH);
    wide_camera_hook(image,p);
    printf("Wide: the game's picture is %ux2160 (%ux1080 in 1080p mode), its scene resized to match\n",width_4k,width_4k/2);
    return 1;
}

/* The Cheats page (L3+R3: runtime_pad.c; settings.ini cheat_*), on eboot 01.10. Most are the
 * engine's own debug switches and tunables, bytes the game only reads, set at each flip:
 *   - invulnerability: the -god_mode switch's flag (0x151c654), which the ship's collision
 *     checks (0x25c83d, 0x25d19e) skip a hit for. The engine also lets god mode bomb for free;
 *     that is taken out (god_off below), infinite bombs is its own cheat. A stub in the Commando
 *     house's hit (0xd91ba) also lets its hits go, as its shield does.
 *   - infinite lives: the tunable rg__game__player__DEBUG_unlimited_lives (0x15221d0), checked by
 *     the game state's tick (0x276305) before a death takes a life (0x275a50).
 *   - no multiplier decay: the tunable rg__game__combos__DEBUG_allow_combo_expiration (0xed4758),
 *     checked by the combo update (0x36a12) before the combo's timer counts down: off, the
 *     multiplier no longer runs out between kills.
 * Stubs jumped to from the engine's update code, on the game's own thread, do the rest:
 *   - infinite bombs: the game state adds each player's bomb change once a tick (0x276067: bombs
 *     at +0xc0, the change at +0x128 + player*0x4c); a negative change (a bomb used) is dropped,
 *     pickups still count, and none left becomes one.
 *   - boost recharge: the boost update (0x244d71) fills its meter (float +0x60, 1 is full) while
 *     no boost runs (+0x59): a boost still lasts one meter. It takes the place of the engine's
 *     rg__game__player__boost__DEBUG_unlimited_boost check (0x1516328), kept as it was.
 *   - overdrive recharge: the overdrive meter's update (0x253b4d) fills it (float +0x5c) while no
 *     overdrive drains it (+0x74). It takes the place of a one-shot fill flag's check (0x151a560),
 *     kept as it was.
 *   - maximum weapon level: the tunable rg__game__weapons__minimum_level (0x1522760) is raised
 *     past every weapon's maximum, and each level the game sets is clamped between them (0x278fa0
 *     and the like); a stub in the weapons' update (0x279efe) sets the level once when the cheat is
 *     switched on, so it applies at once and not at the next pickup. */
enum { CHEAT_INVULNERABLE, CHEAT_LIVES, CHEAT_BOMBS, CHEAT_BOOST, CHEAT_OVERDRIVE, CHEAT_WEAPON, CHEAT_MULTIPLIER,
       CHEAT_COUNT };
static unsigned char cheat_on[CHEAT_COUNT]; /* read by the stubs */
static unsigned char *cheat_image;
static int weapon_floor_saved;       /* rg__game__weapons__minimum_level as the game had it */
static _Atomic unsigned weapon_turn; /* counts the weapon cheat being switched on */
/* At each flip: the Cheats page's choices into the engine's flags and the stubs'. */
static void cheats_sync(void) {
    if (!cheat_image) return;
    const int mask=resogun_cheats();
    for (int i=0;i<CHEAT_COUNT;++i) cheat_on[i]=(unsigned char)((mask>>i)&1);
    cheat_image[0x151c654]=cheat_on[CHEAT_INVULNERABLE];
    cheat_image[0x15221d0]=cheat_on[CHEAT_LIVES];
    /* The weapons' minimum level: every level the game sets is raised to the weapon's maximum. */
    static int weapon_was;
    int *floor=(int *)(cheat_image+0x1522760);
    if (cheat_on[CHEAT_WEAPON] && !weapon_was) { weapon_floor_saved=*floor; ++weapon_turn; }
    if (!cheat_on[CHEAT_WEAPON] && weapon_was) *floor=weapon_floor_saved;
    if (cheat_on[CHEAT_WEAPON]) *floor=0x7fffffff;
    weapon_was=cheat_on[CHEAT_WEAPON];
    /* rg__game__combos__DEBUG_allow_combo_expiration: off, the combo's timer never runs out. */
    static int combo_was;
    static unsigned char combo_saved;
    unsigned char *expire=cheat_image+0xed4758;
    if (cheat_on[CHEAT_MULTIPLIER] && !combo_was) combo_saved=*expire;
    if (!cheat_on[CHEAT_MULTIPLIER] && combo_was) *expire=combo_saved;
    if (cheat_on[CHEAT_MULTIPLIER]) *expire=0;
    combo_was=cheat_on[CHEAT_MULTIPLIER];
}

/* Called from the weapons' update (0x279efe, on the game's thread) with the player's weapon set:
 * the first update after the cheat is switched on sets the current weapon's level through the
 * class's own setter (vtable slot 0x48: attribute 6, the current weapon, as the
 * Set_Player_Weapon_Level script command does); the raised minimum makes that the maximum. */
static ABI void cheat_weapons(unsigned char *weapons) {
    static unsigned char *seen[4];
    static unsigned seen_turn[4];
    if (!cheat_on[CHEAT_WEAPON] || !weapons) return;
    const unsigned turn=weapon_turn;
    for (int i=0;i<4;++i) if (seen[i]==weapons && seen_turn[i]==turn) return;
    static int next;
    seen[next]=weapons; seen_turn[next]=turn; next=(next+1)%4;
    typedef void (ABI *SetAttribute)(void *self, int attribute, int value);
    ((SetAttribute)(*(void ***)weapons)[0x48/8])(weapons,6,0);
}

/* Stub writing: bytes, a jmp [rip] to an absolute target (14 bytes), mov rax, imm64. */
static unsigned char *emit(unsigned char *p, const void *bytes, size_t n) { memcpy(p,bytes,n); return p+n; }
static unsigned char *emit_jmp(unsigned char *p, const void *to) {
    const uint64_t target=(uint64_t)(uintptr_t)to;
    *p++=0xff; *p++=0x25; memset(p,0,4); p+=4;
    memcpy(p,&target,8);
    return p+8;
}
static unsigned char *emit_rax(unsigned char *p, const void *address) {
    const uint64_t value=(uint64_t)(uintptr_t)address;
    *p++=0x48; *p++=0xb8; memcpy(p,&value,8);
    return p+8;
}
/* A short jump's offset byte (at `offset`) to `to`, ahead of it. */
static void land(unsigned char *offset, const unsigned char *to) { *offset=(unsigned char)(to-(offset+1)); }
/* The `size` bytes at `site` become a jmp to `stub` and nops. */
static void cheat_site(unsigned char *site, size_t size, const unsigned char *stub) {
    emit_jmp(site,stub);
    memset(site+14,0x90,size-14);
}

static const unsigned char bombs_site[38]={0x43,0x8b,0x84,0x37,0x28,0x01,0x00,0x00,0x85,0xc0,0x0f,0x84,0x7f,0x00,0x00,0x00,
    0x41,0x03,0x87,0xc0,0x00,0x00,0x00,0xb9,0x00,0x00,0x00,0x00,0x0f,0x48,0xc1,0x41,0x89,0x87,0xc0,0x00,0x00,0x00};
static const unsigned char boost_site[17]={0x80,0x3d,0xb0,0x15,0x2d,0x01,0x00,0x74,0x08,0x41,0xc7,0x46,0x60,0x00,0x00,0x80,0x3f};
static const unsigned char overdrive_site[30]={0x80,0x3d,0x0c,0x6a,0x2c,0x01,0x00,0xc5,0xfa,0x10,0x61,0x04,0xc6,0x05,0x00,
    0x6a,0x2c,0x01,0x00,0x74,0x09,0x41,0xc7,0x44,0x24,0x5c,0x00,0x00,0x80,0x3f};

/* 0x276067, in the game state's tick (this r15, player*0x4c r14): mov eax,[r15+r14+0x128];
 * test eax,eax; jz 0x2760f6; add eax,[r15+0xc0]; clamp at 0; mov [r15+0xc0],eax; on at 0x27608d.
 * With the cheat a bomb used is not taken, and none left becomes one, since a bomb needs one in
 * hand ("has bombs", 0x276e80). rcx is free (the code after both exits loads it). */
static unsigned char *bombs_stub(unsigned char *image, unsigned char *p) {
    unsigned char *none, *kept, *pickup;
    const uint64_t flag=(uint64_t)(uintptr_t)&cheat_on[CHEAT_BOMBS];
    p=emit(p,bombs_site,10);                         /* mov eax,[r15+r14+0x128]; test eax,eax */
    *p++=0x74; none=p++;                             /* jz: no change */
    *p++=0x79; pickup=p++;                           /* jns: bombs added */
    *p++=0x48; *p++=0xb9; memcpy(p,&flag,8); p+=8;   /* mov rcx, &cheat_on[CHEAT_BOMBS] */
    *p++=0x80; *p++=0x39; *p++=0x00;                 /* cmp byte [rcx],0 */
    *p++=0x75; kept=p++;                             /* jnz: the bomb used is not taken */
    land(pickup,p);
    p=emit(p,bombs_site+16,22);                      /* add, clamp, store */
    p=emit_jmp(p,image+0x27608d);
    land(none,p); land(kept,p);
    static const unsigned char empty[]={0x41,0x83,0xbf,0xc0,0x00,0x00,0x00,0x00};               /* cmp dword [r15+0xc0],0 */
    static const unsigned char one[]={0x41,0xc7,0x87,0xc0,0x00,0x00,0x00,0x01,0x00,0x00,0x00}; /* mov dword [r15+0xc0],1 */
    unsigned char *off, *some;
    *p++=0x48; *p++=0xb9; memcpy(p,&flag,8); p+=8;
    *p++=0x80; *p++=0x39; *p++=0x00;
    *p++=0x74; off=p++;
    p=emit(p,empty,sizeof(empty));
    *p++=0x7f; some=p++;                             /* jg */
    p=emit(p,one,sizeof(one));
    land(off,p); land(some,p);
    return emit_jmp(p,image+0x2760f6);
}

/* 0x244d71, in the boost update (this r14): with the engine's unlimited-boost flag the meter is
 * full; with the cheat, also while no boost runs. On at 0x244d82, which sets the flags anew. */
static unsigned char *boost_stub(unsigned char *image, unsigned char *p) {
    static const unsigned char full[]={0x41,0xc7,0x46,0x60,0x00,0x00,0x80,0x3f}; /* mov dword [r14+0x60],1.0 */
    static const unsigned char idle[]={0x41,0x80,0x7e,0x59,0x00};               /* cmp byte [r14+0x59],0 */
    unsigned char *no_debug, *no_cheat, *running;
    *p++=0x50;                                       /* push rax */
    p=emit_rax(p,image+0x1516328);
    *p++=0x80; *p++=0x38; *p++=0x00;                 /* cmp byte [rax],0 */
    *p++=0x74; no_debug=p++;
    p=emit(p,full,sizeof(full));
    land(no_debug,p);
    p=emit_rax(p,&cheat_on[CHEAT_BOOST]);
    *p++=0x80; *p++=0x38; *p++=0x00;
    *p++=0x74; no_cheat=p++;
    p=emit(p,idle,sizeof(idle));
    *p++=0x75; running=p++;
    p=emit(p,full,sizeof(full));
    land(no_cheat,p); land(running,p);
    *p++=0x58;                                       /* pop rax */
    return emit_jmp(p,image+0x244d82);
}

/* 0x253b4d, in the overdrive meter's update (this r12, rcx its owner): vmovss xmm4,[rcx+4]; the
 * one-shot fill flag (0x151a560) fills the meter and is cleared; with the cheat the meter is also
 * filled while no overdrive drains it (float [r12+0x74] not above 0). On at 0x253b6b. */
static unsigned char *overdrive_stub(unsigned char *image, unsigned char *p) {
    static const unsigned char full[]={0x41,0xc7,0x44,0x24,0x5c,0x00,0x00,0x80,0x3f}; /* mov dword [r12+0x5c],1.0 */
    static const unsigned char drain[]={0x41,0x83,0x7c,0x24,0x74,0x00};                /* cmp dword [r12+0x74],0 */
    unsigned char *no_fill, *no_cheat, *draining;
    p=emit(p,overdrive_site+7,5);                    /* vmovss xmm4,[rcx+4] */
    *p++=0x50;                                       /* push rax */
    p=emit_rax(p,image+0x151a560);
    *p++=0x80; *p++=0x38; *p++=0x00;                 /* cmp byte [rax],0 */
    *p++=0xc6; *p++=0x00; *p++=0x00;                 /* mov byte [rax],0 */
    *p++=0x74; no_fill=p++;
    p=emit(p,full,sizeof(full));
    land(no_fill,p);
    p=emit_rax(p,&cheat_on[CHEAT_OVERDRIVE]);
    *p++=0x80; *p++=0x38; *p++=0x00;
    *p++=0x74; no_cheat=p++;
    p=emit(p,drain,sizeof(drain));                   /* a float above 0 is above 0 as an int too */
    *p++=0x7f; draining=p++;                         /* jg */
    p=emit(p,full,sizeof(full));
    land(no_cheat,p); land(draining,p);
    *p++=0x58;                                       /* pop rax */
    return emit_jmp(p,image+0x253b6b);
}

static const unsigned char weapon_site[18]={0x41,0x8b,0x9f,0x3c,0x01,0x00,0x00,0x85,0xdb,0x7e,0x75,0x49,0x8b,0xbf,0xa0,0x00,0x00,0x00};

/* 0x279efe, in the weapons' update (this r15): cheat_weapons(this), then the original mov ebx,
 * [r15+0x13c]; test ebx,ebx; jle 0x279f7e; mov rdi,[r15+0xa0]; on at 0x279f10. The update holds
 * nothing in caller-saved registers here (a call may come just before) and rsp is aligned. */
static unsigned char *weapon_stub(unsigned char *image, unsigned char *p) {
    unsigned char *pending;
    *p++=0x4c; *p++=0x89; *p++=0xff;                 /* mov rdi, r15 */
    p=emit_rax(p,(const void *)(uintptr_t)cheat_weapons);
    *p++=0xff; *p++=0xd0;                            /* call rax */
    p=emit(p,weapon_site,9);                         /* mov ebx,[r15+0x13c]; test ebx,ebx */
    *p++=0x7f; pending=p++;                          /* jg: a level change is pending */
    p=emit_jmp(p,image+0x279f7e);
    land(pending,p);
    p=emit(p,weapon_site+11,7);                      /* mov rdi,[r15+0xa0] */
    return emit_jmp(p,image+0x279f10);
}

static const unsigned char house_site[22]={0x44,0x89,0xf8,0x48,0x8d,0x0d,0xa8,0x22,0x83,0x00,0x41,0x8b,0x94,0x24,0x1c,0x01,
    0x00,0x00,0x44,0x8b,0x3c,0x81};

/* 0xd91ba, in the Commando house's hit (this r12, the hit's kind r15d), where a hit the shield
 * (+0x10c) did not stop takes its damage from the health (int +0x11c): mov eax,r15d; lea rcx,
 * [damage by kind]; mov edx,[r12+0x11c]; mov r15d,[rcx+rax*4]; on at 0xd91d0. With invulnerability
 * the hit is let go as the shield does (0xd919d). rax and rcx are loaded here. */
static unsigned char *house_stub(unsigned char *image, unsigned char *p) {
    unsigned char *off;
    p=emit_rax(p,&cheat_on[CHEAT_INVULNERABLE]);
    *p++=0x80; *p++=0x38; *p++=0x00;                 /* cmp byte [rax],0 */
    *p++=0x74; off=p++;
    p=emit_jmp(p,image+0xd919d);
    land(off,p);
    p=emit(p,house_site,3);                          /* mov eax,r15d */
    const uint64_t damage=(uint64_t)(uintptr_t)(image+0x90b46c);
    *p++=0x48; *p++=0xb9; memcpy(p,&damage,8); p+=8; /* mov rcx, the damage table */
    p=emit(p,house_site+10,12);                      /* mov edx,[r12+0x11c]; mov r15d,[rcx+rax*4] */
    return emit_jmp(p,image+0xd91d0);
}

/* The combo update's expiry check (0x36a12): cmp byte [rip -> 0xed4758],0. */
static const unsigned char combo_check[7]={0x80,0x3d,0x3f,0xdd,0xe9,0x00,0x00};

/* The god_mode getter (0x25a760) as the bomb code calls it: the bomb (0x24e59d) takes one unless it
 * is set, and "has bombs" (0x276e8f) answers yes with none left. Both read it as off (xor eax,eax),
 * so invulnerability leaves the bombs alone. */
static const unsigned char god_bomb_use[5]={0xe8,0xbe,0xc1,0x00,0x00}, god_bomb_has[5]={0xe8,0xcc,0x38,0xfe,0xff};
static const unsigned char god_off[5]={0x31,0xc0,0x0f,0x1f,0x00};

int runtime_game_cheats(unsigned char *image) {
    if (memcmp(image+0x276067,bombs_site,sizeof(bombs_site)) || memcmp(image+0x244d71,boost_site,sizeof(boost_site)) ||
        memcmp(image+0x253b4d,overdrive_site,sizeof(overdrive_site)) || memcmp(image+0x24e59d,god_bomb_use,5) ||
        memcmp(image+0x276e8f,god_bomb_has,5) || memcmp(image+0x279efe,weapon_site,sizeof(weapon_site)) ||
        memcmp(image+0x36a12,combo_check,sizeof(combo_check)) || memcmp(image+0xd91ba,house_site,sizeof(house_site))) {
        puts("Cheats: the engine's code is not where eboot 01.10 has it; the Cheats page does nothing");
        return 0;
    }
    unsigned char *stub=runtime_low_map(4096,PROT_READ|PROT_WRITE|PROT_EXEC);
    if (!stub) return 0;
    unsigned char *p=stub;
    cheat_site(image+0x276067,sizeof(bombs_site),p);
    p=bombs_stub(image,p);
    cheat_site(image+0x244d71,sizeof(boost_site),p);
    p=boost_stub(image,p);
    cheat_site(image+0x253b4d,sizeof(overdrive_site),p);
    p=overdrive_stub(image,p);
    cheat_site(image+0x279efe,sizeof(weapon_site),p);
    p=weapon_stub(image,p);
    cheat_site(image+0xd91ba,sizeof(house_site),p);
    house_stub(image,p);
    memcpy(image+0x24e59d,god_off,5);
    memcpy(image+0x276e8f,god_off,5);
    cheat_image=image;
    cheats_sync();
    const int mask=resogun_cheats();
    if (mask) printf("Cheats: on:%s%s%s%s%s%s%s\n",mask&1 ? " invulnerability" : "",mask&2 ? " infinite-lives" : "",
                     mask&4 ? " infinite-bombs" : "",mask&8 ? " boost-recharge" : "",mask&16 ? " overdrive-recharge" : "",
                     mask&32 ? " max-weapon" : "",mask&64 ? " no-multiplier-decay" : "");
    return 1;
}

/* Everybody's Golf 1.17: frame rate priority, the PS4 Pro display option for 60 FPS (byte
 * 0x2cea1d9, copied from the save's option by the game object's update), in every scene.
 * The game picks its flip rate in nine places (0x0b7cb0 twice, 0x25e8e0, 0xe543b0, 0xe5d150,
 * 0xe5e140, 0xe5f770, 0xfb2960 and 0xfd2320), each an inlined copy of one rule: 0 (every vblank)
 * for the 60 Hz mode (game object +0x24ec == 60), else 1 (every second vblank); with the option
 * on, 0 as well unless the tee, the shot or the flyover has an object up (+0x13150, +0x181b0,
 * +0x181a8, +0x12bda, +0x12c18). The rate goes to +0x24f0 (the frame timing at 0xfb3070 reads it
 * as its budget) and to sceVideoOutSetFlipRate, which all nine reach through the PLT stub at
 * 0x10fed70, `jmp [0x1340660]`. One hook there: the GOT slot points at golf_flip_rate, which
 * turns any slower rate into 0 while the option is on and stores the same 0 in +0x24f0. The
 * game's frame loop already paces on vblanks counted since the last frame (0x3c / +0x24ec - 1,
 * one vblank in the 30 Hz mode), so with flips at 60 Hz those scenes run at 60.
 * Installed only with settings.ini frame_rate_priority on; off, the game runs untouched. */
#define GOLF_OPTION_FLAG 0x2cea1d9u   /* current frame rate priority option */
#define GOLF_GAME_OBJECT 0x2cea1c0u   /* pointer to the game object */
#define GOLF_FLIP_RATE_RECORD 0x24f0u /* game object: the flip rate it last set */
#define GOLF_FLIP_RATE_STUB 0x10fed70u
#define GOLF_FLIP_RATE_SLOT 0x1340660u
static unsigned char *golf_image;
static int32_t (ABI *golf_flip_rate_next)(int32_t handle,int32_t rate);
static int golf_flip_rate_lifted;

/* sceVideoOutSetFlipRate(handle, rate): rate 0 flips every vblank, 1 every second, 2 every
 * third. Called by the game's frame mode code only. */
static int32_t ABI golf_flip_rate(int32_t handle,int32_t rate) {
    if (rate>0 && __atomic_load_n(golf_image+GOLF_OPTION_FLAG,__ATOMIC_RELAXED)) {
        unsigned char *game;
        memcpy(&game,golf_image+GOLF_GAME_OBJECT,sizeof(game));
        if (game) memset(game+GOLF_FLIP_RATE_RECORD,0,4);
        if (!golf_flip_rate_lifted++) printf("60 FPS: the game asked for flip rate %d in a frame rate priority scene; flips stay at 60 Hz\n",rate);
        rate=0;
    }
    return golf_flip_rate_next(handle,rate);
}

/* settings.ini frame_rate_priority: the two places that load the save's display option (0xbced47,
 * the options block +0x2c; 0x14a2d7, the save +0x37) load 2, frame rate priority; the game then
 * sets the option flag itself when it runs in PS4 Pro mode (render_resolution=4k). */
static const struct { uint32_t at; unsigned char load[5]; } golf_option_loads[]={
    {0xbced47,{0x41,0x8b,0x44,0x24,0x2c}}, /* mov eax,[r12+0x2c] */
    {0x14a2d7,{0x41,0x0f,0xbe,0x46,0x37}}, /* movsx eax,byte [r14+0x37] */
};

int runtime_game_golf_sixty(unsigned char *image) {
    if (!clock_foreign || !resogun_frame_rate_priority()) return 0; /* Resogun, or the option off */
    for (size_t i=0;i<sizeof(golf_option_loads)/sizeof(golf_option_loads[0]);++i)
        if (memcmp(image+golf_option_loads[i].at,golf_option_loads[i].load,5)) {
            puts("60 FPS: the display option is not loaded where eboot 1.17 loads it; not changed");
            return 0;
        }
    /* The stub's jmp [rip+disp32] must reach the slot, and the slot must hold the import. */
    const unsigned char *stub=image+GOLF_FLIP_RATE_STUB;
    int32_t disp;
    memcpy(&disp,stub+2,4);
    void *next;
    memcpy(&next,image+GOLF_FLIP_RATE_SLOT,sizeof(next));
    if (stub[0]!=0xff || stub[1]!=0x25 || GOLF_FLIP_RATE_STUB+6+(int64_t)disp!=GOLF_FLIP_RATE_SLOT || !next) {
        puts("60 FPS: sceVideoOutSetFlipRate is not imported where eboot 1.17 imports it; not changed");
        return 0;
    }
    static const unsigned char load_two[5]={0xb8,0x02,0x00,0x00,0x00}; /* mov eax,2 */
    for (size_t i=0;i<sizeof(golf_option_loads)/sizeof(golf_option_loads[0]);++i)
        memcpy(image+golf_option_loads[i].at,load_two,5);
    golf_image=image;
    golf_flip_rate_next=(int32_t (ABI *)(int32_t,int32_t))next;
    void *hook=(void *)golf_flip_rate;
    memcpy(image+GOLF_FLIP_RATE_SLOT,&hook,sizeof(hook));
    puts("60 FPS: settings.ini frame_rate_priority=1: the display option reads as frame rate priority, and its flip rate holds in every scene");
    return 1;
}

/* Everybody's Golf 1.17: the scene's render size (settings.ini render_height). At 0xfb7887 the
 * game object's update picks it from the PS4 Pro flag (0x2cea525) and the frame rate priority
 * flag (wanted 0x2cea1d8, current 0x2cea1d9): frame rate priority clears the Pro flag and renders
 * 1920x1080 (scale 0x1000, 4.12 fixed point); otherwise Pro renders 3200x1800 (scale 0x1aaa).
 * Both paths (0xfb78ce, and 0xfb78b6 through its jump at 0xfb78cc) end at 0xfb7919, which stores
 * the size in the game object (+0x244c, +0x2450, scale +0x12c48); a new size (r14b) rebuilds the
 * scene targets and display buffers (0xfb7997). Patched: 0xfb78cc jumps on to 0xfb78ce, where a
 * stub stores the mode, counts a mode change as a change, so the targets are rebuilt for the new
 * mode at the same size, and gives render_height's 16:9 size in either mode. The rebuild makes
 * the frame rate priority target (+0x15168, drawn by 0xfb5a27 and 0xfb5d4b) only on a Pro with
 * a height up to 1080 (0xfb7e95); above that frame rate priority faulted at 0xfee27a (a clear
 * with no colour target bound), so its jg goes. Needs PS4 Pro mode (render_resolution=4k). */
/* The size the stub gives, packed so one aligned 8-byte store changes it at once: height in bits
 * 0..15, width in 16..31, scale in 32..47; 0 leaves it to the game (1920x1080 in frame rate
 * priority, 3200x1800 otherwise). golf_size[1] is the size last given, golf_size[2] the mode. */
static volatile uint64_t *golf_size;
static int golf_size_limit;

static uint64_t golf_pack_size(int height) {
    if (height<=0) return 0;
    const uint64_t h=(uint64_t)height, w=h*16/9, scale=(h*0x1000+1079)/1080;
    return h|w<<16|scale<<32;
}

/* The Display Settings panel (gpu/shim/resogun_overlay.cpp): a new render height while the game
 * runs. The stub sees it at the game object's next update and the game rebuilds its scene
 * targets, as it does when its own display option changes. Heights the direct memory pool set up
 * at start cannot hold wait for a restart: 0 is returned and nothing changes. */
int runtime_game_golf_set_render_height(int height) {
    if (!golf_size || height>golf_size_limit) return 0;
    __atomic_store_n(&golf_size[0],golf_pack_size(height),__ATOMIC_RELEASE);
    printf("Render size: %d lines from the next frame (Display Settings)\n",height);
    return 1;
}

int runtime_game_golf_render_height_limit(void) { return golf_size ? golf_size_limit : 0; }

/* RESOGUN_TEST_RENDER_HEIGHT=<seconds>:<height>[,<seconds>:<height>...] (diagnostics): the
 * panel's live change, made by a timer, for scripted runs (their pad input cannot open the panel). */
static void *golf_size_test(void *arg) {
    const char *spec=arg;
    unsigned waited=0;
    for (const char *at=spec; at && *at;) {
        unsigned seconds=0; int height=0;
        if (sscanf(at,"%u:%d",&seconds,&height)!=2) break;
        if (seconds>waited) { sleep(seconds-waited); waited=seconds; }
        if (!runtime_game_golf_set_render_height(height)) printf("Render size: %d is past this start's limit\n",height);
        at=strchr(at,',');
        if (at) ++at;
    }
    return NULL;
}

int runtime_game_golf_resolution(unsigned char *image) {
    const int height=resogun_render_height();
    if (!clock_foreign || !resogun_render_4k()) return 0;
    static const unsigned char at_cc[2]={0xeb,0x3b}, at_ce[2]={0x88,0x15}, at_db[6]={0x41,0xbd,0x08,0x07,0x00,0x00},
                               at_19[6]={0x89,0x97,0x48,0x2c,0x01,0x00}, at_e95[8]={0x81,0xfa,0x38,0x04,0x00,0x00,0x7f,0x55};
    if (memcmp(image+0xfb78cc,at_cc,2) || memcmp(image+0xfb78ce,at_ce,2) || memcmp(image+0xfb78db,at_db,6) ||
        memcmp(image+0xfb7919,at_19,6) || memcmp(image+0xfb7e95,at_e95,8)) {
        puts("Render size: the size choice is not where eboot 1.17 has it; not patched");
        return 0;
    }
    unsigned char *stub=runtime_low_map(4096,PROT_READ|PROT_WRITE|PROT_EXEC), *p=stub;
    if (!stub) return 0;
    golf_size=(volatile uint64_t *)(stub+4072); /* want, given, mode; 8-byte aligned */
    golf_size[0]=golf_pack_size(height);
    const uint64_t data=(uint64_t)(uintptr_t)golf_size, flag=(uint64_t)(uintptr_t)(image+0x2cea1d9),
                   back=(uint64_t)(uintptr_t)(image+0xfb7919);
    /* Live here: dl the mode, rdi the game object, r14b changed; rsi free. Out: edx scale, ebx
     * width, r13d height. A new mode or a new size counts as a change (r14b), which rebuilds the
     * scene targets and display buffers. */
    static const unsigned char code[]={
        0x38,0x56,0x10,                   /* cmp [rsi+16], dl: the mode last seen */
        0x74,0x03,                        /* je same_mode */
        0x41,0xb6,0x01,                   /* mov r14b, 1 */
        0x88,0x56,0x10,                   /* same_mode: mov [rsi+16], dl */
        0x48,0x8b,0x1e,                   /* mov rbx, [rsi]: the size wanted */
        0x48,0x3b,0x5e,0x08,              /* cmp rbx, [rsi+8]: the size given */
        0x74,0x07,                        /* je same_size */
        0x48,0x89,0x5e,0x08,              /* mov [rsi+8], rbx */
        0x41,0xb6,0x01,                   /* mov r14b, 1 */
    };
    *p++=0x48; *p++=0xbe; memcpy(p,&data,8); p+=8;          /* mov rsi, golf_size */
    memcpy(p,code,sizeof code); p+=sizeof code;
    *p++=0x48; *p++=0xbe; memcpy(p,&flag,8); p+=8;          /* same_size: mov rsi, flag */
    *p++=0x88; *p++=0x16;                                   /* mov [rsi], dl */
    static const unsigned char pick[]={
        0x48,0x85,0xdb,                   /* test rbx, rbx */
        0x75,0x28,                        /* jnz chosen */
        0x84,0xd2,                        /* test dl, dl */
        0x74,0x12,                        /* jz pro */
        0xba,0x00,0x10,0x00,0x00,         /* mov edx, 0x1000 */
        0xbb,0x80,0x07,0x00,0x00,         /* mov ebx, 1920 */
        0x41,0xbd,0x38,0x04,0x00,0x00,    /* mov r13d, 1080 */
        0xeb,0x26,                        /* jmp out */
        0xba,0xaa,0x1a,0x00,0x00,         /* pro: mov edx, 0x1aaa */
        0xbb,0x80,0x0c,0x00,0x00,         /* mov ebx, 3200 */
        0x41,0xbd,0x08,0x07,0x00,0x00,    /* mov r13d, 1800 */
        0xeb,0x14,                        /* jmp out */
        0x41,0x89,0xdd,                   /* chosen: mov r13d, ebx */
        0x41,0x81,0xe5,0xff,0xff,0x00,0x00, /* and r13d, 0xffff */
        0x48,0x89,0xda,                   /* mov rdx, rbx */
        0x48,0xc1,0xea,0x20,              /* shr rdx, 32 */
        0xc1,0xeb,0x10,                   /* shr ebx, 16 */
    };
    memcpy(p,pick,sizeof pick); p+=sizeof pick;
    *p++=0xff; *p++=0x25; memset(p,0,4); p+=4;              /* out: jmp [rip]: back */
    memcpy(p,&back,8);
    image[0xfb7e9b]=0x90; image[0xfb7e9c]=0x90;             /* 0xfb7e9b: jg 0xfb7ef2 */
    image[0xfb78cd]=0x00;                                   /* 0xfb78cc: jmp 0xfb78ce */
    const uint64_t to=(uint64_t)(uintptr_t)stub;
    unsigned char *site=image+0xfb78ce;
    site[0]=0xff; site[1]=0x25; memset(site+2,0,4);         /* jmp [rip]: the stub */
    memcpy(site+6,&to,8);
    /* The Pro's 5568 MiB of direct memory holds 3200x1800; at 2160 the first course load needed
     * 5716 MiB, and with none left the loader (0xf8d090) asks for 0 bytes and retries forever.
     * The pool is set once, so the panel can go up to what it holds. */
    const int h=height ? height : 1080;
    if (h>1800) runtime_memory_want_pool(h>2160 ? 8704 : 6592);
    golf_size_limit=h>2160 ? 2880 : h>1800 ? 2160 : 1800;
    const char *test=getenv("RESOGUN_TEST_RENDER_HEIGHT");
    pthread_t timer;
    if (test && *test && !pthread_create(&timer,NULL,golf_size_test,(void *)test)) pthread_detach(timer);
    if (height) printf("Render size: %dx%d in either display mode (settings.ini render_height)\n",height*16/9,height);
    else puts("Render size: the game's own (settings.ini render_height=0)");
    return 1;
}
