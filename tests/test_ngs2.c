/* SPDX-License-Identifier: GPL-2.0-or-later */
/* libSceNgs2 (src/runtime_ngs2.c): a sampler voice with PCM blocks is mixed into the first render
 * buffer, reports its state, and ends when its blocks run out. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/runtime_ngs2.c"

uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}
/* No ATRAC9 decoder in this test. */
void *resogun_atrac9_open(const uint8_t config[4],int *channels,int *bytes,int *samples) {
    (void)config; (void)channels; (void)bytes; (void)samples;
    return NULL;
}
void resogun_atrac9_close(void *handle) { (void)handle; }
void resogun_atrac9_reset(void *handle) { (void)handle; }
int resogun_atrac9_decode(void *handle,const uint8_t *data,int size,float *out) {
    (void)handle; (void)data; (void)size; (void)out;
    return 0;
}

enum { GRAIN=256 };
static uint64_t system_handle, rack, voice;
static float out[GRAIN*8];

static void render(void) {
    Ngs2RenderBuffer buffer={out,sizeof out,0x18,8};
    assert(ngs2_system_render(system_handle,&buffer,1)==0);
}
static uint32_t flags(void) {
    uint32_t state[12];
    assert(ngs2_voice_state(voice,state,0x30)==0);
    return state[0];
}
static void event(uint32_t id) {
    uint32_t param[3]={0xc,6,id};
    assert(ngs2_voice_control(voice,param)==0);
}

int main(void) {
    Ngs2SystemOption system_option={.size=0x40,.max_grain=GRAIN,.num_grain=GRAIN,.sample_rate=48000};
    assert(ngs2_system_create_alloc(&system_option,&system_option,&system_handle)==0);
    struct { Ngs2RackOption head; uint32_t tail[10]; } rack_option={.head={.size=0xa8,.max_grain=GRAIN,.max_voices=4}};
    assert(ngs2_rack_create_alloc(system_handle,0x1000,&rack_option.head,&rack_option,&rack)==0);
    assert(ngs2_rack_voice(rack,1,&voice)==0);
    assert(flags()==0);

    /* Setup: 16-bit mono at half the system rate, so every input sample covers two output ones. */
    uint32_t setup[10]={0x28,0x10000000,0x12,1,24000,0,0,0,0,0};
    assert(ngs2_voice_control(voice,setup)==0);
    static int16_t wave[300];
    for (int i=0;i<300;++i) wave[i]=16384;
    uint32_t block[8]={0,sizeof wave,0,0,0,0,0x1234,0};
    struct { uint32_t head[2]; const void *data; uint32_t flags, count; const void *blocks; } blocks={{0x20,0x10000001},wave,0,1,block};
    assert(ngs2_voice_control(voice,&blocks)==0);
    render();
    assert(out[0]==0.0f); /* not started yet */
    event(0);
    render();
    const float level=0.5f*MIX_LEVEL*0.7071f;
    assert(fabsf(out[0]-level)<1e-4f && fabsf(out[1]-level)<1e-4f && out[2]==0.0f);
    assert(fabsf(out[(GRAIN-1)*8]-level)<1e-4f);
    assert(flags()==3);
    uint64_t state[6];
    assert(ngs2_voice_state(voice,state,0x30)==0 && state[4]==0x1234 && state[2]>=128 && state[2]<=130);
    /* 300 samples last 600 output frames: the third grain ends the voice. */
    render();
    assert(flags()==3);
    render();
    assert(flags()==0 && out[(GRAIN-1)*8]==0.0f);

    /* A repeated block keeps playing; stop ends it. */
    block[2]=UINT32_MAX;
    assert(ngs2_voice_control(voice,&blocks)==0);
    event(0);
    for (int i=0;i<8;++i) render();
    assert(flags()==3 && fabsf(out[0]-level)<1e-4f);
    event(4);
    render();
    assert(flags()==1 && out[0]==0.0f);
    event(5);
    render();
    assert(fabsf(out[0]-level)<1e-4f);
    event(1);
    render();
    assert(flags()==0 && out[0]==0.0f);

    /* HE-VAG, mono at the system rate: predictor 0 and shift 0 make every code c a sample c*4096. */
    uint32_t vag_setup[10]={0x28,0x10000000,0x1c,1,48000,0,0,0,0,0};
    assert(ngs2_voice_control(voice,vag_setup)==0);
    static uint8_t vag[16*20];
    for (int f=0;f<20;++f) memset(vag+f*16+2,0x11,14);
    uint32_t vag_block[8]={0,sizeof vag,0,0,0,0,0,0};
    struct { uint32_t head[2]; const void *data; uint32_t flags, count; const void *blocks; } vag_blocks={{0x20,0x10000001},vag,0,1,vag_block};
    assert(ngs2_voice_control(voice,&vag_blocks)==0);
    event(0);
    render();
    const float vag_level=0.125f*MIX_LEVEL*0.7071f;
    assert(fabsf(out[0]-vag_level)<1e-4f && fabsf(out[(GRAIN-1)*8+1]-vag_level)<1e-4f && flags()==3);
    for (int i=0;i<3;++i) render(); /* 560 samples: over within three grains */
    assert(flags()==0);

    assert(ngs2_rack_destroy(rack,NULL)==0);
    assert(ngs2_system_destroy(system_handle,NULL)==0);
    puts("ngs2: ok");
    return 0;
}
