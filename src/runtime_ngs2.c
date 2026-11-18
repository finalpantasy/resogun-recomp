/* libSceNgs2, Sony's audio synthesis library: systems, racks and voices with real handles and real
 * out-fields, and a render that outputs silence. No mixing graph yet.
 *
 * Contracts follow the call sites of Everybody's Golf (CUSA04687 1.17) and shadPS4's
 * core/libraries/ngs2 (ngs2.h, ngs2_impl.cpp); the evidence is in the port's
 * analysis/ngs2-contracts.md. Where this differs from shadPS4: the custom racks 0x4001-0x4003 are
 * accepted (the game creates 0x4002 submixers), and the guest buffer allocator is not called (the
 * game never reads the buffer Ngs2 would have allocated; rack and system state lives on the host).
 * Buffer sizes reported by RackQueryBufferSize are this runtime's own: Sony's are not known. */
#include "runtime.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NGS2_INVALID_NUM_CHANNELS    ((int32_t)0x804A0052)
#define NGS2_INVALID_OUT_ADDRESS     ((int32_t)0x804A0053)
#define NGS2_INVALID_OPTION_ADDRESS  ((int32_t)0x804A0080)
#define NGS2_INVALID_OPTION_SIZE     ((int32_t)0x804A0081)
#define NGS2_INVALID_MAX_GRAIN       ((int32_t)0x804A0050)
#define NGS2_INVALID_NUM_GRAIN       ((int32_t)0x804A0051)
#define NGS2_INVALID_MAX_VOICES      ((int32_t)0x804A0103)
#define NGS2_INVALID_SAMPLE_RATE     ((int32_t)0x804A0201)
#define NGS2_INVALID_BUFFER_INFO     ((int32_t)0x804A0206)
#define NGS2_INVALID_BUFFER_ADDRESS  ((int32_t)0x804A0207)
#define NGS2_INVALID_BUFFER_SIZE     ((int32_t)0x804A0209)
#define NGS2_INVALID_BUFFER_ALLOCATOR ((int32_t)0x804A020A)
#define NGS2_INVALID_SYSTEM_HANDLE   ((int32_t)0x804A0230)
#define NGS2_INVALID_RACK_ID         ((int32_t)0x804A0260)
#define NGS2_INVALID_RACK_HANDLE     ((int32_t)0x804A0261)
#define NGS2_INVALID_VOICE_HANDLE    ((int32_t)0x804A0300)
#define NGS2_INVALID_VOICE_INDEX     ((int32_t)0x804A0302)
#define NGS2_INVALID_WAVEFORM_TYPE   ((int32_t)0x804A0402)
#define NGS2_FAIL                    ((int32_t)0x804A0001)

/* SceNgs2SystemOption (0x40 bytes) and the common head of every SceNgs2RackOption (0x80 bytes,
 * followed by a tail per rack type). */
typedef struct {
    uint64_t size; char name[16]; uint32_t flags, max_grain, num_grain, sample_rate; uint32_t reserved[6];
} Ngs2SystemOption;
typedef struct {
    uint64_t size; char name[16]; uint32_t flags, max_grain, max_voices, max_input_delay, max_matrices, max_ports;
    uint32_t reserved[20];
} Ngs2RackOption;
typedef struct { void *host_buffer; uint64_t host_buffer_size; uint64_t reserved[5]; } Ngs2BufferInfo;
typedef struct { void *buffer; uint64_t size; uint32_t waveform_type, channels; } Ngs2RenderBuffer;
_Static_assert(sizeof(Ngs2SystemOption)==0x40,"SceNgs2SystemOption");
_Static_assert(sizeof(Ngs2RackOption)==0x80,"SceNgs2RackOption");
_Static_assert(sizeof(Ngs2BufferInfo)==0x38,"SceNgs2ContextBufferInfo");
_Static_assert(sizeof(Ngs2RenderBuffer)==0x18,"SceNgs2RenderBufferInfo");

/* Handles are unique nonzero 64-bit values across the three kinds (the game passes a rack handle
 * as a voice's patch destination): tag, kind, slot generation, slot, voice index. */
enum { KIND_SYSTEM=1, KIND_RACK=2, KIND_VOICE=3, MAX_SYSTEMS=8, MAX_RACKS=256 };
#define HANDLE_TAG 0x4E32ull
typedef struct { int used; uint16_t generation; uint32_t num_grain, sample_rate; } Ngs2System;
struct Voice;
typedef struct { int used; uint16_t generation; int system; uint32_t id, max_voices; struct Voice *voices; } Ngs2Rack;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Ngs2System systems[MAX_SYSTEMS];
static Ngs2Rack racks[MAX_RACKS];

static uint64_t make_handle(int kind,uint16_t generation,int slot,uint32_t voice) {
    return HANDLE_TAG<<48 | (uint64_t)kind<<44 | (uint64_t)(generation&0xfff)<<32 | (uint64_t)(slot&0xffff)<<16 | (voice&0xffff);
}
static int handle_kind(uint64_t h) { return h>>48==HANDLE_TAG ? (int)(h>>44&0xf) : 0; }
static int handle_slot(uint64_t h) { return (int)(h>>16&0xffff); }
static uint32_t handle_voice(uint64_t h) { return (uint32_t)(h&0xffff); }
static int handle_generation(uint64_t h) { return (int)(h>>32&0xfff); }
/* Both return the slot, or -1; called with the lock held. */
static int find_system(uint64_t h) {
    int slot=handle_slot(h);
    if (handle_kind(h)!=KIND_SYSTEM || slot>=MAX_SYSTEMS || !systems[slot].used) return -1;
    return (systems[slot].generation&0xfff)==handle_generation(h) ? slot : -1;
}
static int find_rack(uint64_t h,int kind) {
    int slot=handle_slot(h);
    if (handle_kind(h)!=kind || slot>=MAX_RACKS || !racks[slot].used) return -1;
    return (racks[slot].generation&0xfff)==handle_generation(h) ? slot : -1;
}

/* The sampler mixer, first version: every playing sampler voice is decoded (16-bit or float PCM, or
 * ATRAC9 through resogun_atrac9_*), resampled by its pitch and added to the system's first render
 * buffer at one fixed level. Not done yet: patches, port volumes and matrices, submixers, effects,
 * envelopes and filters, so the balance between sounds is not the game's. */
void *resogun_atrac9_open(const uint8_t config[4],int *channels,int *superframe_bytes,int *superframe_samples);
void resogun_atrac9_close(void *handle);
void resogun_atrac9_reset(void *handle);
int resogun_atrac9_decode(void *handle,const uint8_t *data,int size,float *out);

enum { MAX_BLOCKS=16, MAX_CHANNELS=8, DECODE_FRAMES=2048, TYPE_PCM16=0x12, TYPE_FLOAT=0x18, TYPE_VAG=0x1c, TYPE_ATRAC9=0x40 };
enum { CODEC_NONE, CODEC_PCM16, CODEC_FLOAT, CODEC_ATRAC9, CODEC_VAG };
static const float hevag_coefs[128][4]={
#include "hevag_coefs.inc"
};
#define MIX_LEVEL 0.35f
typedef struct { const uint8_t *data; uint32_t size, repeats, skip, samples; uint64_t user; } Block;
typedef struct Voice {
    int codec, playing, paused, primed;
    uint32_t type, channels, rate;
    Block blocks[MAX_BLOCKS];
    uint32_t num_blocks, block, repeat, byte_pos, skip, left; int limited, entered;
    float pitch; double phase;
    float frame[2][MAX_CHANNELS];
    void *atrac9; int superframe_bytes, superframe_samples;
    int32_t history[MAX_CHANNELS][4];
    float *decoded; uint32_t decoded_count, decoded_pos;
    uint64_t decoded_samples, decoded_bytes;
} Voice;

static void voice_release(Voice *v) {
    if (v->atrac9) resogun_atrac9_close(v->atrac9);
    free(v->decoded);
    memset(v,0,sizeof *v);
}
/* One 16-byte HE-VAG frame of one channel: 28 samples, written every `stride` floats. Byte 0 is
 * the low predictor nibble and the shift, byte 1 the high predictor nibble and the flags (7: a
 * silent frame); 28 4-bit codes follow, low nibble first. Channels alternate frame by frame. */
static void vag_frame(const uint8_t *frame,int32_t history[4],float *out,uint32_t stride) {
    uint32_t index=(frame[0]>>4)|(frame[1]&0xf0), shift=frame[0]&15, flag=frame[1]&15;
    if (index>127) index=0;
    const float *k=hevag_coefs[index];
    for (int i=0;i<28;++i) {
        int32_t sample=0;
        if (flag<7) {
            int32_t code=(frame[2+i/2]>>(i&1 ? 4 : 0))&15;
            if (code&8) code-=16;
            code=shift>12 ? 0 : (code*4096)>>shift;
            sample=code+(int32_t)(history[0]*k[0]+history[1]*k[1]+history[2]*k[2]+history[3]*k[3]);
        }
        history[3]=history[2]; history[2]=history[1]; history[1]=history[0]; history[0]=sample;
        out[(size_t)i*stride]=(float)(sample>32767 ? 32767 : sample<-32768 ? -32768 : sample)/32768.0f;
    }
}
/* Decodes the next piece of the block queue into v->decoded. 0 when the queue is finished. */
static int voice_decode(Voice *v) {
    for (int guard=0;guard<4*MAX_BLOCKS;++guard) {
        if (v->block>=v->num_blocks) return 0;
        const Block *b=&v->blocks[v->block];
        if (!v->entered) { v->entered=1; v->byte_pos=0; v->skip=b->skip; v->left=b->samples; v->limited=b->samples!=0; }
        uint32_t frames=0;
        if (v->byte_pos<b->size && !(v->limited && !v->left)) {
            const uint8_t *p=b->data+v->byte_pos;
            uint32_t rest=b->size-v->byte_pos;
            if (v->codec==CODEC_PCM16 || v->codec==CODEC_FLOAT) {
                uint32_t width=(v->codec==CODEC_PCM16 ? 2u : 4u)*v->channels;
                frames=rest/width<DECODE_FRAMES ? rest/width : DECODE_FRAMES;
                for (uint32_t i=0;i<frames*v->channels;++i) {
                    if (v->codec==CODEC_PCM16) { int16_t s; memcpy(&s,p+i*2,2); v->decoded[i]=(float)s/32768.0f; }
                    else memcpy(&v->decoded[i],p+i*4,4);
                }
                v->byte_pos+=frames ? frames*width : rest;
            } else if (v->codec==CODEC_VAG && rest>=16u*v->channels) {
                uint32_t groups=rest/(16u*v->channels);
                if (groups>DECODE_FRAMES/28) groups=DECODE_FRAMES/28;
                for (uint32_t g=0;g<groups;++g)
                    for (uint32_t c=0;c<v->channels;++c)
                        vag_frame(p+(size_t)(g*v->channels+c)*16,v->history[c],v->decoded+(size_t)g*28*v->channels+c,v->channels);
                frames=groups*28;
                v->byte_pos+=groups*16*v->channels;
            } else if (v->codec==CODEC_ATRAC9 && rest>=(uint32_t)v->superframe_bytes) {
                frames=(uint32_t)resogun_atrac9_decode(v->atrac9,p,(int)rest,v->decoded);
                v->byte_pos+=(uint32_t)v->superframe_bytes;
            } else v->byte_pos=b->size;
            v->decoded_bytes+=v->byte_pos-(b->size-rest);
        }
        if (frames) {
            uint32_t drop=v->skip<frames ? v->skip : frames;
            v->skip-=drop;
            v->decoded_pos=drop; v->decoded_count=frames;
            if (v->limited) {
                uint32_t keep=frames-drop<v->left ? frames-drop : v->left;
                v->left-=keep; v->decoded_count=drop+keep;
            }
            if (v->decoded_pos<v->decoded_count) return 1;
            continue;
        }
        /* The block pass is over: repeat it, or go on to the next block. */
        v->entered=0;
        if (b->repeats==UINT32_MAX || v->repeat<b->repeats) ++v->repeat;
        else { ++v->block; v->repeat=0; }
    }
    return 0;
}
static int voice_frame(Voice *v,float *out) {
    if (v->decoded_pos>=v->decoded_count && !voice_decode(v)) return 0;
    memcpy(out,&v->decoded[(size_t)v->decoded_pos*v->channels],sizeof(float)*v->channels);
    ++v->decoded_pos; ++v->decoded_samples;
    return 1;
}
/* Adds `frames` frames of the voice to mix (interleaved, MAX_CHANNELS wide). */
static void voice_mix(Voice *v,float *mix,uint32_t frames,uint32_t system_rate) {
    if (!v->playing || v->paused || v->codec==CODEC_NONE) return;
    double step=(double)(v->pitch>0 ? v->pitch : 1.0f)*v->rate/system_rate;
    if (!v->primed) {
        if (!voice_frame(v,v->frame[0])) { v->playing=0; return; }
        if (!voice_frame(v,v->frame[1])) memcpy(v->frame[1],v->frame[0],sizeof v->frame[0]);
        v->primed=1; v->phase=0;
    }
    for (uint32_t i=0;i<frames;++i) {
        while (v->phase>=1.0) {
            memcpy(v->frame[0],v->frame[1],sizeof v->frame[0]);
            if (!voice_frame(v,v->frame[1])) { v->playing=0; return; }
            v->phase-=1.0;
        }
        float t=(float)v->phase, *out=mix+(size_t)i*MAX_CHANNELS;
        if (v->channels==1) {
            float s=(v->frame[0][0]+(v->frame[1][0]-v->frame[0][0])*t)*MIX_LEVEL*0.7071f;
            out[0]+=s; out[1]+=s;
        } else for (uint32_t c=0;c<v->channels;++c)
            out[c]+=(v->frame[0][c]+(v->frame[1][c]-v->frame[0][c])*t)*MIX_LEVEL;
        v->phase+=step;
    }
}
/* Sampler setup (id 0x10000000): the waveform format. An ATRAC9 stream is known by its config
 * word (first byte 0xFE), since Sony's waveform type numbers for it are not public. */
static void voice_setup(Voice *v,const uint8_t *param) {
    uint32_t format[4];
    memcpy(format,param+8,sizeof format);
    voice_release(v);
    v->type=format[0]; v->channels=format[1]; v->rate=format[2]; v->pitch=1.0f;
    if (!v->channels || v->channels>MAX_CHANNELS || v->rate<1000 || v->rate>192000) return;
    uint32_t capacity=DECODE_FRAMES;
    if (v->type==TYPE_ATRAC9 || param[20]==0xFE || param[23]==0xFE) {
        /* The config word: the stream's four bytes, either as they are or as a big-endian number. */
        uint8_t config[4]={param[20],param[21],param[22],param[23]};
        if (config[0]!=0xFE) { config[0]=param[23]; config[1]=param[22]; config[2]=param[21]; config[3]=param[20]; }
        int channels=0;
        v->atrac9=resogun_atrac9_open(config,&channels,&v->superframe_bytes,&v->superframe_samples);
        if (v->atrac9 && (uint32_t)channels==v->channels) { v->codec=CODEC_ATRAC9; capacity=(uint32_t)v->superframe_samples; }
    } else if (v->type==TYPE_PCM16) v->codec=CODEC_PCM16;
    else if (v->type==TYPE_FLOAT) v->codec=CODEC_FLOAT;
    else if (v->type==TYPE_VAG) v->codec=CODEC_VAG;
    if (v->codec==CODEC_NONE) {
        static int reported;
        if (reported++<8) printf("Runtime: Ngs2 waveform type %#x (%u ch, %u Hz, config %#x) has no decoder%c",v->type,v->channels,v->rate,format[3],10);
        return;
    }
    if (!(v->decoded=malloc(sizeof(float)*capacity*v->channels))) v->codec=CODEC_NONE;
}
/* Waveform blocks (id 0x10000001): {data, flags, numBlocks, aBlock}. A playing voice with blocks
 * still queued gets the new ones appended (a stream being fed); otherwise they replace the queue. */
static void voice_blocks(Voice *v,const uint8_t *param) {
    const uint8_t *data; uint32_t count; const uint32_t *blocks;
    memcpy(&data,param+8,8); memcpy(&count,param+20,4); memcpy(&blocks,param+24,8);
    /* The game follows each block list with an empty one (no data, no blocks): nothing to queue. */
    if (!data || !blocks || !count) return;
    if (!(v->playing && v->block<v->num_blocks)) { v->num_blocks=v->block=v->repeat=0; v->entered=0; }
    else if (v->block) {
        memmove(v->blocks,v->blocks+v->block,sizeof(Block)*(v->num_blocks-v->block));
        v->num_blocks-=v->block; v->block=0;
    }
    for (uint32_t i=0;data && blocks && i<count && v->num_blocks<MAX_BLOCKS;++i) {
        const uint32_t *b=blocks+i*8;
        Block *to=&v->blocks[v->num_blocks++];
        to->data=data+b[0]; to->size=b[1]; to->repeats=b[2]; to->skip=b[3]; to->samples=b[4];
        memcpy(&to->user,b+6,8);
    }
}
/* Event (id 6): 0 starts the voice from its first block, 1 to 3 end it, 4 pauses, 5 resumes. */
static void voice_event(Voice *v,uint32_t event) {
    if (event==0) {
        v->playing=1; v->paused=0; v->primed=0; v->block=v->repeat=0; v->entered=0;
        v->decoded_pos=v->decoded_count=0; v->decoded_samples=v->decoded_bytes=0; v->phase=0;
        memset(v->history,0,sizeof v->history);
        if (v->atrac9) resogun_atrac9_reset(v->atrac9);
    } else if (event<=3) v->playing=0;
    else if (event==4) v->paused=1;
    else if (event==5) v->paused=0;
}
/* Called with the lock held: the rack's voices stop and their decoders go. */
static void rack_free(Ngs2Rack *r) {
    for (uint32_t i=0;r->voices && i<r->max_voices;++i) voice_release(&r->voices[i]);
    free(r->voices);
    r->voices=NULL; r->used=0;
}

static int32_t check_system_option(const Ngs2SystemOption *o) {
    if (o->size!=sizeof *o) return NGS2_INVALID_OPTION_SIZE;
    if (!o->max_grain || o->max_grain%64 || o->max_grain>1024) return NGS2_INVALID_MAX_GRAIN;
    if (!o->num_grain || o->num_grain%64 || o->num_grain>o->max_grain) return NGS2_INVALID_NUM_GRAIN;
    if (o->sample_rate<11025 || o->sample_rate>192000) return NGS2_INVALID_SAMPLE_RATE;
    return 0;
}
/* The option size each rack id takes: sampler, submixer, reverb, mastering, and the custom racks
 * (0x80 + numBuffers/numModules + 24 modules of 40 bytes + 16 ports of 8 bytes + padding). */
static uint64_t rack_option_size(uint32_t id) {
    switch (id) {
    case 0x1000: return 0xa8;
    case 0x2000: return 0x98;
    case 0x2001: return 0x98;
    case 0x3000: return 0x88;
    case 0x4001: case 0x4002: case 0x4003: return 0x4d8;
    default: return 0;
    }
}
static int32_t check_rack(uint32_t id,const Ngs2RackOption *o) {
    uint64_t size=rack_option_size(id);
    if (!size) return NGS2_INVALID_RACK_ID;
    if (!o) return NGS2_INVALID_OPTION_ADDRESS;
    if (o->size!=size && !(id==0x2001 && o->size>=sizeof *o)) return NGS2_INVALID_OPTION_SIZE;
    if (!o->max_grain || o->max_grain%64 || o->max_grain>1024) return NGS2_INVALID_MAX_GRAIN;
    if (!o->max_voices || o->max_voices>0xffff) return NGS2_INVALID_MAX_VOICES;
    return 0;
}

static int32_t system_create(const Ngs2SystemOption *option,uint64_t *out) {
    if (!out) return NGS2_INVALID_OUT_ADDRESS;
    if (!option) return NGS2_INVALID_OPTION_ADDRESS;
    int32_t error=check_system_option(option);
    if (error) return error;
    pthread_mutex_lock(&lock);
    int slot=0;
    while (slot<MAX_SYSTEMS && systems[slot].used) ++slot;
    if (slot==MAX_SYSTEMS) { pthread_mutex_unlock(&lock); return NGS2_FAIL; }
    Ngs2System *s=&systems[slot];
    s->used=1; ++s->generation; s->num_grain=option->num_grain; s->sample_rate=option->sample_rate;
    *out=make_handle(KIND_SYSTEM,s->generation,slot,0);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* sceNgs2SystemCreateWithAllocator(option, allocator, out handle). */
static ABI int32_t ngs2_system_create_alloc(const Ngs2SystemOption *option,const void *allocator,uint64_t *out) {
    if (!allocator) return NGS2_INVALID_BUFFER_ALLOCATOR;
    return system_create(option,out);
}
/* sceNgs2SystemDestroy(handle, out buffer info or NULL): its racks go with it. */
static ABI int32_t ngs2_system_destroy(uint64_t handle,Ngs2BufferInfo *info) {
    pthread_mutex_lock(&lock);
    int slot=find_system(handle);
    if (slot<0) { pthread_mutex_unlock(&lock); return NGS2_INVALID_SYSTEM_HANDLE; }
    for (int i=0;i<MAX_RACKS;++i) if (racks[i].used && racks[i].system==slot) rack_free(&racks[i]);
    systems[slot].used=0;
    pthread_mutex_unlock(&lock);
    if (info) memset(info,0,sizeof *info);
    return 0;
}
/* sceNgs2RackQueryBufferSize(rack id, option, out buffer info): the game allocates and zeroes
 * hostBufferSize bytes itself and hands them to sceNgs2RackCreate. */
static ABI int32_t ngs2_rack_query(uint32_t id,const Ngs2RackOption *option,Ngs2BufferInfo *info) {
    if (!info) return NGS2_INVALID_BUFFER_INFO;
    int32_t error=check_rack(id,option);
    if (error) return error;
    memset(info,0,sizeof *info);
    info->host_buffer_size=0x1000+(uint64_t)option->max_voices*0x100;
    return 0;
}
static int32_t rack_create(uint64_t system,uint32_t id,const Ngs2RackOption *option,uint64_t *out) {
    if (!out) return NGS2_INVALID_OUT_ADDRESS;
    int32_t error=check_rack(id,option);
    if (error) return error;
    pthread_mutex_lock(&lock);
    int owner=find_system(system);
    if (owner<0) { pthread_mutex_unlock(&lock); return NGS2_INVALID_SYSTEM_HANDLE; }
    int slot=0;
    while (slot<MAX_RACKS && racks[slot].used) ++slot;
    if (slot==MAX_RACKS) { pthread_mutex_unlock(&lock); return NGS2_FAIL; }
    Ngs2Rack *r=&racks[slot];
    r->used=1; ++r->generation; r->system=owner; r->id=id; r->max_voices=option->max_voices;
    r->voices=id==0x1000 ? calloc(option->max_voices,sizeof(Voice)) : NULL;
    *out=make_handle(KIND_RACK,r->generation,slot,0);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* sceNgs2RackCreate(system, rack id, option, buffer info, out handle). */
static ABI int32_t ngs2_rack_create(uint64_t system,uint32_t id,const Ngs2RackOption *option,const Ngs2BufferInfo *info,uint64_t *out) {
    if (!info) return NGS2_INVALID_BUFFER_INFO;
    if (!info->host_buffer) return NGS2_INVALID_BUFFER_ADDRESS;
    return rack_create(system,id,option,out);
}
/* sceNgs2RackCreateWithAllocator(system, rack id, option, allocator, out handle). */
static ABI int32_t ngs2_rack_create_alloc(uint64_t system,uint32_t id,const Ngs2RackOption *option,const void *allocator,uint64_t *out) {
    if (!allocator) return NGS2_INVALID_BUFFER_ALLOCATOR;
    return rack_create(system,id,option,out);
}
/* sceNgs2RackDestroy(handle, out buffer info or NULL). */
static ABI int32_t ngs2_rack_destroy(uint64_t handle,Ngs2BufferInfo *info) {
    pthread_mutex_lock(&lock);
    int slot=find_rack(handle,KIND_RACK);
    if (slot<0) { pthread_mutex_unlock(&lock); return NGS2_INVALID_RACK_HANDLE; }
    rack_free(&racks[slot]);
    pthread_mutex_unlock(&lock);
    if (info) memset(info,0,sizeof *info);
    return 0;
}
/* sceNgs2RackGetVoiceHandle(rack, index, out handle): one stable handle per rack and index. */
static ABI int32_t ngs2_rack_voice(uint64_t rack,uint32_t index,uint64_t *out) {
    if (!out) return NGS2_INVALID_OUT_ADDRESS;
    pthread_mutex_lock(&lock);
    int slot=find_rack(rack,KIND_RACK);
    int32_t result=slot<0 ? NGS2_INVALID_RACK_HANDLE : index>=racks[slot].max_voices ? NGS2_INVALID_VOICE_INDEX : 0;
    if (!result) *out=make_handle(KIND_VOICE,racks[slot].generation,slot,index);
    pthread_mutex_unlock(&lock);
    return result;
}
static int32_t check_voice(uint64_t voice) {
    pthread_mutex_lock(&lock);
    int slot=find_rack(voice,KIND_VOICE);
    int ok=slot>=0 && handle_voice(voice)<racks[slot].max_voices;
    pthread_mutex_unlock(&lock);
    return ok ? 0 : NGS2_INVALID_VOICE_HANDLE;
}
/* RESOGUN_NGS2_TRACE=1: the first few parameter blocks of each id the game sends, as evidence for
 * the mixer (SceNgs2VoiceParamHeader: u16 size, s16 next, u32 id; `next` is a byte offset). */
static void trace_params(uint64_t voice,const void *params) {
    static int enabled=-1;
    static struct { uint32_t id, seen; } ids[48];
    if (enabled<0) { const char *v=getenv("RESOGUN_NGS2_TRACE"); enabled=v && *v=='1'; }
    if (!enabled || !params) return;
    const uint8_t *p=params;
    for (int guard=0;guard<16;++guard) {
        uint16_t size; int16_t next; uint32_t id;
        memcpy(&size,p,2); memcpy(&next,p+2,2); memcpy(&id,p+4,4);
        pthread_mutex_lock(&lock);
        int slot=find_rack(voice,KIND_VOICE), n=0, show=0;
        uint32_t rack=slot<0 ? 0 : racks[slot].id;
        static unsigned sampler_lines;
        while (n<48 && ids[n].seen && ids[n].id!=id) ++n;
        if (n<48) { ids[n].id=id; show=ids[n].seen++<6u; }
        /* The sampler rack: every parameter but the matrices and volumes, in order. */
        if (rack==0x1000) show=id!=1 && id!=2 && id!=3 && sampler_lines++<400;
        pthread_mutex_unlock(&lock);
        if (show) {
            printf("Runtime: Ngs2 trace rack %#x voice %u id %#x size %#x:",rack,handle_voice(voice),id,size);
            for (unsigned i=8;i<size && i<0x38;i+=4) { uint32_t w; memcpy(&w,p+i,4); printf(" %08x",w); }
            if (id==0x10000001 && size>=0x20) {
                const uint8_t *data; uint32_t count; const uint32_t *block;
                memcpy(&data,p+8,8); memcpy(&count,p+0x14,4); memcpy(&block,p+0x18,8);
                for (uint32_t b=0;block && b<count && b<3;++b)
                    printf(" | block %u: offset %#x size %#x repeats %u skip %u samples %u",b,block[b*8],block[b*8+1],
                           block[b*8+2],block[b*8+3],block[b*8+4]);
                if (data) { printf(" | data"); for (int i=0;i<24;++i) printf(" %02x",data[i]); }
            }
            printf("%c",10);
        }
        if (!next) break;
        p+=next;
    }
}
/* The sampler voice of a handle, or NULL (another rack type); called with the lock held. */
static Voice *sampler_voice(uint64_t voice) {
    int slot=find_rack(voice,KIND_VOICE);
    if (slot<0 || !racks[slot].voices || handle_voice(voice)>=racks[slot].max_voices) return NULL;
    return &racks[slot].voices[handle_voice(voice)];
}
/* sceNgs2VoiceControl(voice, parameter list): sampler setup, waveform blocks, pitch and events
 * drive the mixer; every other parameter (patch, volumes, filters, other racks) is accepted and
 * not applied yet. */
static ABI int32_t ngs2_voice_control(uint64_t voice,const void *params) {
    int32_t error=check_voice(voice);
    if (error || !params) return error;
    trace_params(voice,params);
    pthread_mutex_lock(&lock);
    Voice *v=sampler_voice(voice);
    const uint8_t *p=params;
    for (int guard=0;v && guard<16;++guard) {
        uint16_t size; int16_t next; uint32_t id;
        memcpy(&size,p,2); memcpy(&next,p+2,2); memcpy(&id,p+4,4);
        if (id==0x10000000 && size>=0x28) voice_setup(v,p);
        else if (id==0x10000001 && size>=0x20) voice_blocks(v,p);
        else if (id==0x10000005 && size>=0x10) memcpy(&v->pitch,p+8,4);
        else if (id==6 && size>=0xc) { uint32_t event; memcpy(&event,p+8,4); voice_event(v,event); }
        if (!next) break;
        p+=next;
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Bit 0: the voice is in use (the game retires a sound whose bit 0 is clear); bit 1: playing. */
static uint32_t voice_flags(const Voice *v) { return v && v->playing ? (v->paused ? 1u : 3u) : 0; }
/* sceNgs2VoiceGetState(voice, out state, state size): SceNgs2SamplerVoiceState for a sampler voice
 * (flags, envelope and peak height, decoded samples and bytes, the current block's user data and
 * data); zeroes for the other racks. */
static ABI int32_t ngs2_voice_state(uint64_t voice,void *state,uint64_t size) {
    int32_t error=check_voice(voice);
    if (error) return error;
    if (!state) return NGS2_INVALID_OUT_ADDRESS;
    if (size>0x100) return NGS2_INVALID_OPTION_SIZE;
    memset(state,0,size);
    pthread_mutex_lock(&lock);
    const Voice *v=sampler_voice(voice);
    if (v && size>=0x30) {
        struct { uint32_t flags; float envelope, peak; uint32_t reserved; uint64_t samples, bytes, user; const void *data; } out={0};
        out.flags=voice_flags(v); out.envelope=v->playing ? 1.0f : 0.0f;
        out.samples=v->decoded_samples; out.bytes=v->decoded_bytes;
        if (v->block<v->num_blocks) { out.user=v->blocks[v->block].user; out.data=v->blocks[v->block].data; }
        memcpy(state,&out,0x30);
    } else if (v && size>=4) { uint32_t flags=voice_flags(v); memcpy(state,&flags,4); }
    pthread_mutex_unlock(&lock);
    return 0;
}
/* sceNgs2VoiceGetStateFlags(voice, out flags). */
static ABI int32_t ngs2_voice_flags(uint64_t voice,uint32_t *flags) {
    int32_t error=check_voice(voice);
    if (error) return error;
    if (!flags) return NGS2_INVALID_OUT_ADDRESS;
    pthread_mutex_lock(&lock);
    *flags=voice_flags(sampler_voice(voice));
    pthread_mutex_unlock(&lock);
    return 0;
}
/* sceNgs2SystemRender(system, buffers, count): one grain. The checks are those of shadPS4 (1, 2, 6
 * or 8 channels; 16-bit or float PCM, either byte order; room for a grain). The sampler mix goes
 * to the first buffer (the main output); the others get silence. RESOGUN_NGS2_MIX=0 keeps all of
 * them silent. */
static ABI int32_t ngs2_system_render(uint64_t system,const Ngs2RenderBuffer *buffers,uint32_t count) {
    static float mix[1024*MAX_CHANNELS];
    static int enabled=-1;
    if (enabled<0) { const char *v=getenv("RESOGUN_NGS2_MIX"); enabled=!(v && *v=='0'); }
    pthread_mutex_lock(&lock);
    int slot=find_system(system);
    uint32_t grain=slot<0 ? 0 : systems[slot].num_grain, rate=slot<0 ? 48000 : systems[slot].sample_rate;
    int32_t result=slot<0 ? NGS2_INVALID_SYSTEM_HANDLE : !buffers || !count ? NGS2_INVALID_BUFFER_ADDRESS : 0;
    for (uint32_t i=0;!result && i<count;++i) {
        const Ngs2RenderBuffer *b=&buffers[i];
        uint32_t bytes=b->waveform_type==0x12 || b->waveform_type==0x13 ? 2 : b->waveform_type==0x18 || b->waveform_type==0x19 ? 4 : 0;
        if (!b->buffer) result=NGS2_INVALID_BUFFER_ADDRESS;
        else if (!bytes) result=NGS2_INVALID_WAVEFORM_TYPE;
        else if (b->channels!=1 && b->channels!=2 && b->channels!=6 && b->channels!=8) result=NGS2_INVALID_NUM_CHANNELS;
        else if (b->size<(uint64_t)bytes*b->channels*grain) result=NGS2_INVALID_BUFFER_SIZE;
    }
    if (result) { pthread_mutex_unlock(&lock); return result; }
    for (uint32_t i=0;i<count;++i) memset(buffers[i].buffer,0,buffers[i].size);
    if (enabled && grain<=1024) {
        memset(mix,0,sizeof(float)*grain*MAX_CHANNELS);
        for (int r=0;r<MAX_RACKS;++r) {
            if (!racks[r].used || racks[r].system!=slot || !racks[r].voices) continue;
            for (uint32_t i=0;i<racks[r].max_voices;++i) voice_mix(&racks[r].voices[i],mix,grain,rate);
        }
        const Ngs2RenderBuffer *b=&buffers[0];
        int swap=b->waveform_type==0x13 || b->waveform_type==0x19, is_float=b->waveform_type>=0x18;
        uint32_t width=is_float ? 4 : 2;
        for (uint32_t i=0;i<grain;++i) {
            const float *in=mix+(size_t)i*MAX_CHANNELS;
            for (uint32_t c=0;c<b->channels;++c) {
                /* One or two output channels: the mix's left and right (and a mono sum). */
                float s=b->channels==1 ? (in[0]+in[1])*0.7071f : in[c];
                s=s>1.0f ? 1.0f : s<-1.0f ? -1.0f : s;
                uint8_t raw[4];
                if (is_float) memcpy(raw,&s,4); else { int16_t q=(int16_t)(s*32767.0f); memcpy(raw,&q,2); }
                if (swap) for (uint32_t k=0;k<width/2;++k) { uint8_t t=raw[k]; raw[k]=raw[width-1-k]; raw[width-1-k]=t; }
                memcpy((uint8_t *)b->buffer+((size_t)i*b->channels+c)*width,raw,width);
            }
        }
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

static const RuntimeExport exports[]={
    {"sceNgs2SystemCreateWithAllocator",ngs2_system_create_alloc}, {"sceNgs2SystemDestroy",ngs2_system_destroy},
    {"sceNgs2SystemRender",ngs2_system_render},
    {"sceNgs2RackQueryBufferSize",ngs2_rack_query}, {"sceNgs2RackCreate",ngs2_rack_create},
    {"sceNgs2RackCreateWithAllocator",ngs2_rack_create_alloc}, {"sceNgs2RackDestroy",ngs2_rack_destroy},
    {"sceNgs2RackGetVoiceHandle",ngs2_rack_voice},
    {"sceNgs2VoiceControl",ngs2_voice_control}, {"sceNgs2VoiceGetState",ngs2_voice_state},
    {"sceNgs2VoiceGetStateFlags",ngs2_voice_flags},
};
uintptr_t runtime_ngs2_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
