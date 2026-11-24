/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pad mapping: the keyboard layout and the gamepad's Back button. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/runtime_pad.c"

int resogun_gpu_overlay_captures_input(void) { return 0; }
int resogun_skip_intro(void) { return 0; }
int resogun_display_settings_is_open(void) { return 0; }
void resogun_display_settings_open(void) {}
int resogun_title_back_exits(void) { return 1; }
void resogun_exit_prompt_open(void) {}
void resogun_cheats_open(void) {}
static unsigned long long fake_flips;
unsigned long long resogun_gpu_flips(void) { return fake_flips; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static bool keys[SDL_SCANCODE_COUNT];

/* The keyboard's contribution to a neutral pad with the keys in `held` down. */
static PadData press(const SDL_Scancode *held, size_t count) {
    memset(keys,0,sizeof(keys));
    for (size_t i=0;i<count;++i) keys[held[i]]=true;
    PadData d;
    memset(&d,0,sizeof(d));
    d.left_x=d.left_y=d.right_x=d.right_y=128;
    sample_keyboard(&d,keys);
    return d;
}
static PadData press1(SDL_Scancode key) { return press(&key,1); }

static size_t sources_of(const ButtonMap *map, uint32_t ps) {
    size_t n=0;
    for (size_t i=0;i<map->count;++i) n+=map->entries[i].ps==ps;
    return n;
}

static void test_default_keyboard(void) {
    assert(press1(SDL_SCANCODE_SPACE).buttons==BTN_CROSS);
    assert(press1(SDL_SCANCODE_RETURN).buttons==BTN_CROSS);
    assert(press1(SDL_SCANCODE_ESCAPE).buttons==BTN_CIRCLE);
    assert(press1(SDL_SCANCODE_F).buttons==BTN_SQUARE);
    assert(press1(SDL_SCANCODE_H).buttons==BTN_TRIANGLE);
    assert(press1(SDL_SCANCODE_Q).buttons==BTN_L1 && press1(SDL_SCANCODE_O).buttons==BTN_R1);
    assert(press1(SDL_SCANCODE_UP).buttons==BTN_UP && press1(SDL_SCANCODE_RIGHT).buttons==BTN_RIGHT);
    assert(press1(SDL_SCANCODE_TAB).buttons==BTN_OPTIONS);
    assert(press1(SDL_SCANCODE_T).buttons==CLICK); /* play_click turns it into a touch */
    PadData d=press1(SDL_SCANCODE_E);
    assert(d.buttons==BTN_L2 && d.l2==255);
    /* Sticks: no button bits, full tilt, both sticks at once. */
    const SDL_Scancode slide[]={SDL_SCANCODE_W,SDL_SCANCODE_D,SDL_SCANCODE_J};
    d=press(slide,3);
    assert(d.buttons==0);
    assert(d.left_y==0 && d.left_x==255 && d.right_x==0 && d.right_y==128);
    /* Two buttons at once, one from each hand. */
    const SDL_Scancode taps[]={SDL_SCANCODE_F,SDL_SCANCODE_H};
    assert(press(taps,2).buttons==(BTN_SQUARE|BTN_TRIANGLE));
    assert(press(NULL,0).buttons==0);
}

static void test_pad_buttons(void) {
    assert(sources_of(&pad_map,CLICK)==1); /* Back/View clicks the touchpad */
    assert(sources_of(&pad_map,BTN_CROSS)==1 && sources_of(&key_map,BTN_CROSS)==3);
}

/* Users 1 and 2 each open one pad, whose handle is the user id; user 2's reads disconnected
 * until a second gamepad connects, and the keyboard stays with user 1. */
static void test_two_users(void) {
    assert(pad_open(1,0,0,NULL)==ERR_NOT_INITIALIZED);
    assert(pad_init()==0);
    assert(pad_open(3,0,0,NULL)==ERR_INVALID_ARG);
    opened[0]=opened[1]=1; /* as pad_open does, without its host thread */
    assert(slot_of(1)==0 && slot_of(2)==1 && slot_of(3)==-1 && slot_of(0)==-1);
    PadData d;
    assert(pad_read_state(2,&d)==0 && !d.connected);
    assert(pad_read_state(1,&d)==0 && d.connected);
    ControllerInfo info;
    assert(pad_info(2,&info)==0 && !info.connected && pad_info(1,&info)==0 && info.connected);
    host_read(&d,1,NULL,1);
    assert(!d.connected);
    const uint8_t rumble2[2]={200,10};
    assert(pad_vibration(2,rumble2)==0 && rumble[1].large==200 && rumble[0].large==0);
    const uint8_t red[3]={255,0,0};
    assert(pad_light_bar(2,red)==0 && light[1].r==255 && light[0].r==0);
    assert(pad_close(2)==0 && pad_read_state(2,&d)==ERR_INVALID_HANDLE && pad_close(2)==ERR_INVALID_HANDLE);
}

/* RESOGUN_PAD_REPLAY: states keyed to flips since the first read; real input is ignored until the
 * tail after the last line passes, then comes back. */
static void test_replay(void) {
    const char *path="test_pad_replay.tmp";
    FILE *f=fopen(path,"w");
    assert(f);
    fprintf(f,"# pad record v1%c10 16384 128 128 128 128 0 0%cS 18 testsync%c20 0 255 128 128 128 0 0%c",10,10,10,10);
    fclose(f);
    #ifdef _WIN32
    _putenv_s("RESOGUN_PAD_REPLAY",path);
#else
    setenv("RESOGUN_PAD_REPLAY",path,1);
#endif
    fake_flips=1000;
#ifdef _WIN32
    _putenv_s("RESOGUN_PAD_SYNC",".");
#else
    setenv("RESOGUN_PAD_SYNC",".",1);
#endif
    PadData d;
    memset(&d,0,sizeof(d));
    d.buttons=BTN_SQUARE; d.left_x=d.left_y=d.right_x=d.right_y=128;
    replay_armed=1; replay_start=0;
    assert(replay_sample(&d) && replay_count==2 && d.buttons==0); /* before the first line: neutral */
    d.buttons=BTN_SQUARE;
    fake_flips=1010; assert(replay_sample(&d) && d.buttons==16384 && d.left_x==128);
        fake_flips=1017; assert(replay_sample(&d) && d.buttons==16384);
    d.buttons=BTN_SQUARE;
    fake_flips=1018; assert(!replay_sample(&d) && d.buttons==BTN_SQUARE); /* waits: real input is live */
    fake_flips=1090; assert(!replay_sample(&d));
    FILE *g=fopen("testsync","w"); assert(g); fclose(g);
    fake_flips=1100; assert(replay_sample(&d) && d.buttons==16384); /* synced: flip 18 again */
    fake_flips=1102; assert(replay_sample(&d) && d.buttons==0 && d.left_x==255);
    remove("testsync");
    fake_flips=1102+REPLAY_TAIL; assert(replay_sample(&d) && d.left_x==255);
    d.buttons=BTN_SQUARE;
    fake_flips=1103+REPLAY_TAIL; assert(!replay_sample(&d) && d.buttons==BTN_SQUARE && replay_armed==2);
    remove(path);
}

int main(void) {
    test_replay();
    test_default_keyboard();
    test_pad_buttons();
    test_two_users();
    puts("pad tests passed");
    return 0;
}
