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
typedef struct { int used; uint16_t generation; int system; uint32_t id, max_voices; } Ngs2Rack;
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
    for (int i=0;i<MAX_RACKS;++i) if (racks[i].used && racks[i].system==slot) racks[i].used=0;
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
    racks[slot].used=0;
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
/* sceNgs2VoiceControl(voice, parameter list): accepted; nothing plays yet, so the parameters
 * (setup, waveform blocks, pitch, patch, event) are not kept. */
static ABI int32_t ngs2_voice_control(uint64_t voice,const void *params) {
    (void)params;
    return check_voice(voice);
}
/* sceNgs2VoiceGetState(voice, out state, state size): a zeroed state, so stateFlags says the voice
 * is not running and the game retires a started sound at its next poll. */
static ABI int32_t ngs2_voice_state(uint64_t voice,void *state,uint64_t size) {
    int32_t error=check_voice(voice);
    if (error) return error;
    if (!state) return NGS2_INVALID_OUT_ADDRESS;
    if (size>0x100) return NGS2_INVALID_OPTION_SIZE;
    memset(state,0,size);
    return 0;
}
/* sceNgs2VoiceGetStateFlags(voice, out flags). */
static ABI int32_t ngs2_voice_flags(uint64_t voice,uint32_t *flags) {
    int32_t error=check_voice(voice);
    if (error) return error;
    if (!flags) return NGS2_INVALID_OUT_ADDRESS;
    *flags=0;
    return 0;
}
/* sceNgs2SystemRender(system, buffers, count): one grain of silence into each buffer, after the
 * checks shadPS4 makes (1, 2, 6 or 8 channels; 16-bit or float PCM, either byte order; room for a
 * grain). */
static ABI int32_t ngs2_system_render(uint64_t system,const Ngs2RenderBuffer *buffers,uint32_t count) {
    pthread_mutex_lock(&lock);
    int slot=find_system(system);
    uint32_t grain=slot<0 ? 0 : systems[slot].num_grain;
    pthread_mutex_unlock(&lock);
    if (slot<0) return NGS2_INVALID_SYSTEM_HANDLE;
    if (!buffers || !count) return NGS2_INVALID_BUFFER_ADDRESS;
    for (uint32_t i=0;i<count;++i) {
        const Ngs2RenderBuffer *b=&buffers[i];
        if (!b->buffer) return NGS2_INVALID_BUFFER_ADDRESS;
        uint32_t bytes=b->waveform_type==0x12 || b->waveform_type==0x13 ? 2 : b->waveform_type==0x18 || b->waveform_type==0x19 ? 4 : 0;
        if (!bytes) return NGS2_INVALID_WAVEFORM_TYPE;
        if (b->channels!=1 && b->channels!=2 && b->channels!=6 && b->channels!=8) return NGS2_INVALID_NUM_CHANNELS;
        if (b->size<(uint64_t)bytes*b->channels*grain) return NGS2_INVALID_BUFFER_SIZE;
    }
    for (uint32_t i=0;i<count;++i) memset(buffers[i].buffer,0,buffers[i].size);
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
