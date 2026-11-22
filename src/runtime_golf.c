/* Everybody's Golf (CUSA04687): a local PSN account. Sony's servers for this game are gone, so the
 * runtime plays their part: the one user is signed in under a local online id, and the NpWebApi
 * requests the game sends (shadPS4 np_web_api.cpp for the calls, the request paths from the
 * eboot's strings at 0x1219035..0x12191c6) are answered here, never sent anywhere.
 * GOLF_NP=offline turns this off: the game then sees a signed-out console, as before.
 * Resolved before the other tables (runtime.c), and only for this title. */
#include "runtime.h"
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/random.h>
#endif

#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define AUTH_INVALID_ARGUMENT ((int32_t)0x80550301)
#define AUTH_INVALID_SIZE ((int32_t)0x80550302)
#define AUTH_INVALID_ID ((int32_t)0x80550307)
#define WEBAPI_OUT_OF_MEMORY ((int32_t)0x80552901)
#define WEBAPI_INVALID_ARGUMENT ((int32_t)0x80552902)
#define WEBAPI_INVALID_LIB_CONTEXT_ID ((int32_t)0x80552903)
#define WEBAPI_REQUEST_NOT_FOUND ((int32_t)0x80552907)
#define USER_INVALID (-1)

static int local_account, replay_tickets, replay_auth, local_lobbies;
static pthread_mutex_t auth_lock=PTHREAD_MUTEX_INITIALIZER;
static int32_t auth_id, auth_next=0x10010000;
static int auth_ready;
static char online_id[17]="Golfer";
void runtime_golf_set_title(const char *title_id) {
    const char *mode=getenv("GOLF_NP"), *name=getenv("GOLF_ONLINE_ID");
    local_account=title_id && !strncmp(title_id,"CUSA04687",9) && !(mode && !strcmp(mode,"offline"));
    const char *grants=getenv("GOLF_TICKET_GRANTS");
    replay_tickets=grants && !strcmp(grants,"1");
    const char *lobbies=getenv("GOLF_LOCAL_LOBBIES");
    local_lobbies=local_account && lobbies && !strcmp(lobbies,"1");
    const char *auth=getenv("GOLF_LOCAL_AUTH");
    pthread_mutex_lock(&auth_lock);
    replay_auth=local_account && auth && !strcmp(auth,"1");
    auth_id=auth_ready=0;
    pthread_mutex_unlock(&auth_lock);
    if (name && *name) snprintf(online_id,sizeof(online_id),"%.16s",name);
    if (local_account) printf("Runtime: local PSN account '%s' signed in (GOLF_NP=offline turns this off)\n",online_id);
}
int runtime_golf_local_account(void) { return local_account; }

static ABI int32_t ok(void) { return 0; }
/* sceNpGetState(user, state): 2 is SIGNED_IN. */
static ABI int32_t np_state(int32_t user,int32_t *state) {
    if (!state || user==USER_INVALID) return NP_INVALID_ARGUMENT;
    *state=2; return 0;
}
/* sceNpGetOnlineId(user, id): OrbisNpOnlineId is char data[16], term, dummy[3]. */
static ABI int32_t np_online_id(int32_t user,char *id) {
    if (!id || user==USER_INVALID) return NP_INVALID_ARGUMENT;
    memset(id,0,20); memcpy(id,online_id,strlen(online_id)); return 0;
}
/* sceNpGetNpId(user, id): OrbisNpId is the online id, opt[8], reserved[8]. */
static ABI int32_t np_id(int32_t user,char *id) {
    if (!id || user==USER_INVALID) return NP_INVALID_ARGUMENT;
    memset(id,0,36); memcpy(id,online_id,strlen(online_id)); return 0;
}
/* sceNpGetAccountCountryA(user, country): OrbisNpCountryCode is char data[2], term, pad. */
static ABI int32_t np_country(int32_t user,char *country) {
    if (!country || user==USER_INVALID) return NP_INVALID_ARGUMENT;
    memcpy(country,"us\0",4); return 0;
}

/* ---- NpWebApi ---- */
typedef struct { int64_t id; int method; char group[32], path[256]; char *body; size_t size, read; int status; } Request;
#define REQUESTS 32
static Request requests[REQUESTS];
static int64_t next_request=1;
static int32_t next_context=1;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static const char *const methods[]={"GET","POST","PUT","DELETE"};

static Request *find(int64_t id) {
    for (int i=0;i<REQUESTS;++i) if (requests[i].id==id) return &requests[i];
    return NULL;
}
/* sceNpWebApiCreateContextA(libCtx, user): the user's context. */
static ABI int32_t webapi_context(int32_t lib,int32_t user) {
    if (lib>=0x8000 || user==USER_INVALID) printf("Runtime: NpWebApi context refused (library %d, user %#x)%c",lib,(unsigned)user,10);
    if (lib>=0x8000) return WEBAPI_INVALID_LIB_CONTEXT_ID;
    if (user==USER_INVALID) return WEBAPI_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    const int32_t id=next_context++;
    pthread_mutex_unlock(&lock);
    printf("Runtime: NpWebApi context %d created (library %d, user %#x)%c",id,lib,(unsigned)user,10);
    return id;
}
/* sceNpWebApiCreateRequest(userCtx, apiGroup, path, method, contentParameter, requestId). */
static ABI int32_t webapi_create(int32_t ctx,const char *group,const char *path,int32_t method,const void *content,int64_t *id) {
    (void)ctx; (void)content;
    if (!group || !path || method<0 || method>3) return WEBAPI_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    Request *r=find(0);
    if (!r) { r=&requests[0]; free(r->body); } /* a full table reuses the first slot */
    memset(r,0,sizeof(*r));
    r->id=next_request++; r->method=method;
    snprintf(r->group,sizeof(r->group),"%s",group); snprintf(r->path,sizeof(r->path),"%s",path);
    if (id) *id=r->id;
    printf("Runtime: NpWebApi request %lld created: %s %s %s%c",(long long)r->id,methods[method],r->group,r->path,10);
    pthread_mutex_unlock(&lock);
    return 0;
}
static const char entitlement_list[]="/v1/users/me/entitlements?entitlement_type=service";
static const char entitlement_item[]="/v1/users/me/entitlements/";
static const struct { const char *id; unsigned count; } ticket_grants[]={
    {"EN0001",1}, {"EN0002",10},
};
/* The opt-in fixture shares the WebAPI mutex. It lasts for the life of the process. */
static unsigned consumed_tickets;

static int find_ticket(const char *path) {
    if (strncmp(path,entitlement_item,sizeof(entitlement_item)-1)) return -1;
    const char *id=path+sizeof(entitlement_item)-1;
    for (size_t i=0;i<sizeof(ticket_grants)/sizeof(ticket_grants[0]);++i)
        if (!strcmp(id,ticket_grants[i].id)) return (int)i;
    return -1;
}

static int consume_count(const char *data,size_t size,unsigned *count) {
    static const char prefix[]="{\"use_count\":";
    const size_t prefix_size=sizeof(prefix)-1;
    if (!data || size<prefix_size+2 || size>prefix_size+11 ||
        data[size-1]!='}' || memcmp(data,prefix,prefix_size) ||
        (size>prefix_size+2 && data[prefix_size]=='0')) return 0;
    unsigned amount=0;
    for (size_t i=prefix_size;i<size-1;++i) {
        unsigned digit=(unsigned char)data[i]-'0';
        if (digit>9 || amount>(UINT_MAX-digit)/10) return 0;
        amount=amount*10+digit;
    }
    *count=amount;
    return 1;
}

/* The Store catalog for a label batch ("/v1/users/me/container/L1:L2:...?size=..."): one product per
 * label with a single display-only SKU that cannot be bought. C: eboot 0xcf1ee0 parses a top-level
 * array of {container_type, label, skus[0]{label, price, display_price, is_purchaseable,
 * use_count}} and needs each label to be one it asked for. Nothing is sold here. */
static const char commerce_container[]="/v1/users/me/container/";
static const char *catalog(const char *labels) {
    static char json[16384];
    size_t used=0;
    json[used++]='[';
    while (*labels && *labels!='?') {
        const size_t length=strcspn(labels,":?");
        if (length && length<=48 && used+2*length+160<sizeof json &&
            strspn(labels,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")>=length)
            used+=(size_t)snprintf(json+used,sizeof(json)-used,
                "%s{\"container_type\":\"product\",\"label\":\"%.*s\",\"skus\":[{\"label\":\"LOCAL_%.*s\","
                "\"price\":0,\"display_price\":\"\",\"is_purchaseable\":false,\"use_count\":0}]}",
                used>1 ? "," : "",(int)length,labels,(int)length,labels);
        labels+=length;
        if (*labels==':') ++labels;
    }
    json[used++]=']'; json[used]=0;
    return json;
}

/* Answers one NpWebApi request locally. The entitlement list is empty unless GOLF_TICKET_GRANTS=1
 * supplies the two test grants; a consume of a grant that is not there gets 404, never success. */
static int32_t answer(Request *r,const char *sent,size_t sent_size) {
    static const char not_found[]="{\"error\":{\"code\":2105356,\"message\":\"Not found\"}}";
    const char *body="{}";
    int status=200;
    char list[256];
    unsigned consume=0;
    if (!strcmp(r->group,"sdk:entitlement")) {
        if (r->method==0 && !strcmp(r->path,entitlement_list)) {
            body="{\"entitlements\":[],\"start\":0,\"size\":0,\"total_results\":0}";
            if (replay_tickets) {
                const int first=(consumed_tickets&1)!=0, second=(consumed_tickets&2)!=0;
                snprintf(list,sizeof(list),
                         "{\"entitlements\":[{\"id\":\"%s\",\"use_count\":%u,\"use_limit\":%u},"
                         "{\"id\":\"%s\",\"use_count\":%u,\"use_limit\":%u}],\"start\":0,\"size\":2,\"total_results\":2}",
                         ticket_grants[0].id,first ? ticket_grants[0].count : 0,first ? 0 : ticket_grants[0].count,
                         ticket_grants[1].id,second ? ticket_grants[1].count : 0,second ? 0 : ticket_grants[1].count);
                body=list;
            }
        } else {
            const int ticket=replay_tickets && r->method==2 ? find_ticket(r->path) : -1;
            if (ticket<0) { status=404; body=not_found; }
            else {
                unsigned count;
                const unsigned bit=1u<<(unsigned)ticket;
                if (!consume_count(sent,sent_size,&count)) status=400;
                else if (count!=ticket_grants[ticket].count || (consumed_tickets&bit)) status=409;
                else consume=bit;
            }
        }
    } else if (r->method==0 && !strcmp(r->group,"sdk:commerce") &&
               !strcmp(r->path,"/v1/users/me/container/?size=128&serviceLabel=0")) body="[]";
    else if (r->method==0 && !strcmp(r->group,"sdk:commerce") && !strncmp(r->path,commerce_container,sizeof(commerce_container)-1))
        body=catalog(r->path+sizeof(commerce_container)-1);
    else if (strstr(r->path,"/friendList")) body="{\"friendList\":[],\"start\":0,\"size\":0,\"totalResults\":0}";
    else if (strstr(r->path,"/presence/")) body="{}";
    else { status=404; body=not_found; }

    const size_t size=strlen(body);
    char *response=malloc(size+1);
    if (!response) return WEBAPI_OUT_OF_MEMORY;
    memcpy(response,body,size+1);
    free(r->body);
    r->body=response; r->status=status; r->size=size; r->read=0;
    consumed_tickets|=consume;
    printf("Runtime: NpWebApi %s %s %s -> %d (answered locally)",methods[r->method],r->group,r->path,r->status);
    if (sent && sent_size) printf(" body '%.*s'",(int)(sent_size>200 ? 200 : sent_size),sent);
    printf("\n");
    return 0;
}
/* sceNpWebApiSendRequest2(request, data, size, responseInformationOption): the option is
 * int32 httpStatus, char *errorObject @8, u64 errorObjectSize @16, u64 responseDataSize @24; an
 * HTTP error's body goes to errorObject. sceNpWebApiSendRequest: the same without the option. */
static ABI int32_t webapi_send2(int64_t id,const void *data,uint64_t size,void *option) {
    pthread_mutex_lock(&lock);
    Request *r=find(id);
    const int32_t result=r ? answer(r,data,size) : WEBAPI_REQUEST_NOT_FOUND;
    if (!result && option) {
        char *o=option, *error; uint64_t room, wrote=0;
        memcpy(o,&r->status,4); memcpy(&error,o+8,8); memcpy(&room,o+16,8);
        if (r->status>=400 && error && room) {
            wrote=r->size<room-1 ? r->size : room-1;
            memcpy(error,r->body,wrote); error[wrote]=0; r->read=r->size;
        }
        memcpy(o+24,&wrote,8);
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t webapi_send(int64_t id,const void *data,uint64_t size) { return webapi_send2(id,data,size,NULL); }
/* sceNpWebApiGetHttpStatusCode(request, status). */
static ABI int32_t webapi_status(int64_t id,int32_t *status) {
    pthread_mutex_lock(&lock);
    Request *r=find(id);
    if (r && status) *status=r->status;
    pthread_mutex_unlock(&lock);
    return r ? 0 : WEBAPI_REQUEST_NOT_FOUND;
}
/* sceNpWebApiReadData(request, data, size): the bytes copied; 0 once the body has been read. */
static ABI int32_t webapi_read(int64_t id,void *data,uint64_t size) {
    if (!data || !size) return WEBAPI_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    Request *r=find(id);
    int32_t n=WEBAPI_REQUEST_NOT_FOUND;
    if (r) {
        const size_t left=r->size-r->read;
        n=(int32_t)(left<size ? left : size);
        if (n) memcpy(data,r->body+r->read,(size_t)n);
        r->read+=(size_t)n;
    }
    pthread_mutex_unlock(&lock);
    return n;
}
/* sceNpWebApiDeleteRequest(request). */
static ABI int32_t webapi_delete(int64_t id) {
    pthread_mutex_lock(&lock);
    Request *r=find(id);
    if (r) { free(r->body); memset(r,0,sizeof(*r)); }
    pthread_mutex_unlock(&lock);
    return r ? 0 : WEBAPI_REQUEST_NOT_FOUND;
}

/* One async request for the guest's local title login probe. No platform credentials. */
typedef struct {
    uint64_t size;
    int32_t user, reserved;
    const char *client_id, *scope;
} LocalAuthCodeParam;

static ABI int32_t local_auth_create(const uint64_t *param) {
    if (!param) return AUTH_INVALID_ARGUMENT;
    if (param[0]!=24) return AUTH_INVALID_SIZE;
    pthread_mutex_lock(&auth_lock);
    int32_t result=AUTH_INVALID_ID;
    if (!auth_id && auth_next<INT32_MAX) result=auth_id=auth_next++;
    pthread_mutex_unlock(&auth_lock);
    printf("Runtime: NpAuth request created: %#x%c",(unsigned)result,10);
    return result;
}

static ABI int32_t local_auth_code(int32_t id,const LocalAuthCodeParam *param,char *code,int32_t *issuer) {
    if (!param || !code || !issuer) return AUTH_INVALID_ARGUMENT;
    if (param->size!=sizeof(*param)) return AUTH_INVALID_SIZE;
    if (param->user==USER_INVALID || !param->client_id || !param->scope) return AUTH_INVALID_ARGUMENT;
    pthread_mutex_lock(&auth_lock);
    int32_t result=AUTH_INVALID_ID;
    if (auth_id && id==auth_id && !auth_ready) {
        static const char synthetic[]="GolfLocalProbe";
        memcpy(code,synthetic,sizeof synthetic);
        *issuer=0;
        auth_ready=1;
        result=0;
    }
    pthread_mutex_unlock(&auth_lock);
    printf("Runtime: NpAuth authorization code for client %s, scope %s: %#x%c",param->client_id,param->scope,(unsigned)result,10);
    return result;
}

static ABI int32_t local_auth_poll(int32_t id,int32_t *operation) {
    if (!operation) return AUTH_INVALID_ARGUMENT;
    pthread_mutex_lock(&auth_lock);
    int32_t result=AUTH_INVALID_ID;
    if (auth_id && id==auth_id && auth_ready) {
        *operation=0;
        result=0;
    }
    pthread_mutex_unlock(&auth_lock);
    return result;
}

static ABI int32_t local_auth_abort(int32_t id) {
    pthread_mutex_lock(&auth_lock);
    const int match=auth_id && id==auth_id;
    if (match) auth_ready=0;
    pthread_mutex_unlock(&auth_lock);
    return match ? 0 : AUTH_INVALID_ID;
}

static ABI int32_t local_auth_delete(int32_t id) {
    pthread_mutex_lock(&auth_lock);
    const int match=auth_id && id==auth_id;
    if (match) auth_id=auth_ready=0;
    pthread_mutex_unlock(&auth_lock);
    return match ? 0 : AUTH_INVALID_ID;
}

/* ---- NpMatching2, answered locally (network up: GOLF_DISCOVERY_PORT) ----
 * Sony's matching server is gone. With the local account signed in and the network reported up,
 * the context calls succeed and a runtime thread delivers the events the library would deliver:
 * C: eboot 0xa7dfd0 creates the context (0xa7e1e1), registers the callbacks and calls ContextStart
 * (0xa7e482); each must return 0. Its context callback 0xa7e820 (ctxId, event, eventCause,
 * errorCode, arg) takes event 0x6f02 (started) with errorCode >= 0 as "context up", and 0x6f03
 * (stopped) to destroy the context. Callback argument lists: shadPS4 np_matching2.h.
 * Requests are answered through m2_post from the same thread, after the call returned, as the
 * library does: the game stores its "waiting" state after the request call returns. */
#define M2_OUT_OF_MEMORY ((int32_t)0x80550c01)
#define M2_CONTEXT_NOT_FOUND ((int32_t)0x80550c06)
#define M2_BUSY ((int32_t)0x80550c27)
#define M2_SERVER_NOT_AVAILABLE ((int32_t)0x80550c28)
#define M2_INVALID_LOBBY ((int32_t)0x80550c0e)

typedef ABI void (*M2ContextCb)(uint16_t ctx,uint16_t event,uint8_t cause,int32_t error,void *arg);
typedef ABI void (*M2RequestCb)(uint16_t ctx,uint32_t req,uint16_t event,int32_t error,const void *data,void *arg);
typedef struct { M2RequestCb cb; void *arg, *data; uint32_t req; int32_t error; uint16_t event, ctx; uint64_t generation, lobby; } M2Event;
static pthread_mutex_t m2_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t m2_wake=PTHREAD_COND_INITIALIZER;
static M2Event m2_queue[32];
static unsigned m2_head, m2_count;
static int m2_thread_up;
static M2ContextCb m2_context_cb;
static void *m2_context_arg;
static uint16_t m2_ctx;
static uint64_t m2_generation;
static int32_t m2_signaling_ctx;
typedef struct { M2RequestCb cb; void *arg; uint32_t timeout, pad; } M2Opt;
static M2Opt m2_default_opt;
typedef ABI void (*M2LobbyCb)(uint16_t ctx,uint64_t lobby,uint16_t event,const void *data,void *arg);
static M2LobbyCb m2_lobby_cb;
static void *m2_lobby_arg;
typedef ABI void (*M2RoomCb)(uint16_t ctx,uint64_t room,uint16_t event,const void *data,void *arg);
static M2RoomCb m2_room_cb;
static void *m2_room_arg;
static pthread_cond_t m2_room_delivery_done=PTHREAD_COND_INITIALIZER;
static uint64_t m2_room_delivery,m2_room_delivery_sequence;
static _Thread_local int m2_inside_room_cb;
typedef ABI void (*M2RoomMessageCb)(uint16_t ctx,uint64_t room,uint16_t member,uint16_t event,
                                     const void *data,void *arg);
static M2RoomMessageCb m2_room_message_cb;
static void *m2_room_message_arg;
static uint64_t m2_room_message_delivery,m2_room_message_registration;
typedef ABI void (*M2RoomSignalCb)(uint16_t ctx,uint64_t room,uint16_t member,uint16_t event,
                                    int32_t error,void *arg);
static M2RoomSignalCb m2_room_signal_cb;
static void *m2_room_signal_arg;
static uint64_t m2_room_signal_registration,m2_room_signal_delivery;

static void m2_room_signal_wait_locked(uint64_t delivery) {
    if (m2_inside_room_cb) return;
    while (delivery && delivery==m2_room_signal_delivery)
        pthread_cond_wait(&m2_room_delivery_done,&m2_lock);
}

static void m2_room_message_wait_locked(uint64_t delivery) {
    if (m2_inside_room_cb) return;
    while (delivery && delivery==m2_room_message_delivery)
        pthread_cond_wait(&m2_room_delivery_done,&m2_lock);
}
static void m2_room_wait_locked(uint64_t delivery) {
    if (m2_inside_room_cb) return;
    while (delivery && delivery==m2_room_delivery) pthread_cond_wait(&m2_room_delivery_done,&m2_lock);
}

enum { M2_LOCAL_JOIN=1, M2_LOCAL_POLL=2, M2_LOCAL_LEAVE=3, M2_LOCAL_CAPACITY=50 };
typedef struct { uint16_t id; char name[17]; } M2LocalMember;
typedef struct {
    uint16_t own_id;
    uint8_t token[16], count;
    M2LocalMember members[M2_LOCAL_CAPACITY];
} M2LocalRoster;
typedef struct {
    uint64_t id, revision;
    uint16_t owner, count, hub;
    uint8_t signaling;
    M2LocalMember members[M2_LOCAL_CAPACITY];
} M2LocalRoom;
typedef struct {
    uint64_t generation;
    uint16_t ctx;
    uint8_t world, ready;
    struct timespec next_poll;
    M2LocalRoster roster;
    M2LocalRoom room, advertised_links;
    uint64_t signal_registration, room_message_ack;
    uint8_t room_message_relay[16];
    uint64_t game_lobby; /* the lobby id the game joined (GOLF_LOCAL_WORLD may map it to another world) */
} M2LocalSession;

static int m2_lobby_name_char(uint8_t ch) {
    return (ch>='A' && ch<='Z') || (ch>='a' && ch<='z') ||
           (ch>='0' && ch<='9') || ch=='_' || ch=='-';
}

static int m2_lobby_snapshot(uint8_t operation,uint8_t world,const uint8_t *token,M2LocalRoster *roster) {
    uint8_t request[GOLF_LOBBY_REQUEST_BYTES]={operation,world};
    uint8_t response[GOLF_LOBBY_RESPONSE_BYTES];
    size_t length=0;
    const uint8_t zero_token[16]={0};
    if (token) memcpy(request+4,token,16);
    if (operation==M2_LOCAL_JOIN) {
#ifdef _WIN32
        if (compat_random(request+4,16)) return 0;
#else
        if (getrandom(request+4,16,0)!=16) return 0;
#endif
        if (!memcmp(request+4,zero_token,16)) return 0;
        memcpy(request+20,online_id,strlen(online_id));
    }
    int status=runtime_golf_lobby_exchange(request,response,&length);
    /* A lost join reply may already have created the lease. Retry its nonce once. */
    if (!status && operation==M2_LOCAL_JOIN) {
        length=0;
        status=runtime_golf_lobby_exchange(request,response,&length);
    }
    if (status!=200) return status;
    if (operation==M2_LOCAL_LEAVE) return 200;
    if (length<20 || !response[2] || response[2]>M2_LOCAL_CAPACITY || response[3] ||
        length!=20+(size_t)response[2]*18 || !memcmp(response+4,zero_token,16) ||
        (token && memcmp(response+4,token,16))) return 0;
    memset(roster,0,sizeof *roster);
    memcpy(&roster->own_id,response,2);
    memcpy(roster->token,response+4,16);
    roster->count=response[2];
    int found_own=0;
    uint16_t previous_id=0;
    for (uint8_t i=0;i<roster->count;++i) {
        const uint8_t *row=response+20+(size_t)i*18;
        M2LocalMember *member=&roster->members[i];
        memcpy(&member->id,row,2);
        if (member->id<=previous_id || !m2_lobby_name_char(row[2])) return 0;
        previous_id=member->id;
        const size_t name_len=strnlen((const char *)row+2,16);
        for (size_t j=0;j<name_len;++j) if (!m2_lobby_name_char(row[2+j])) return 0;
        for (size_t j=name_len;j<16;++j) if (row[2+j]) return 0;
        memcpy(member->name,row+2,name_len);
        if (member->id==roster->own_id) found_own=1;
    }
    return found_own ? 200 : 0;
}

static uint64_t m2_lobby_account_id(uint8_t world,uint16_t id) {
    return 0x45474c4200000000ull | ((uint64_t)world<<16) | id;
}

static void m2_lobby_record(uint8_t *record,const M2LocalMember *member,void *next,uint8_t world) {
    memcpy(record,&next,8);
    if (world) {
        /* Golf copies OrbisNpPeerAddressA from +0x10 (eboot 0xa79866). */
        const uint64_t account=m2_lobby_account_id(world,member->id);
        const uint32_t platform=3; /* PS4 */
        memcpy(record+0x10,&account,8);
        memcpy(record+0x18,&platform,4);
    }
    memcpy(record+0x20,member->name,17);
    memcpy(record+0x34,&member->id,2);
}

static void m2_lobby_deadline(M2LocalSession *session) {
    clock_gettime(CLOCK_REALTIME,&session->next_poll);
    ++session->next_poll.tv_sec;
}

static int m2_lobby_due(const M2LocalSession *session) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME,&now);
    return now.tv_sec>session->next_poll.tv_sec ||
           (now.tv_sec==session->next_poll.tv_sec && now.tv_nsec>=session->next_poll.tv_nsec);
}

static int m2_roster_has(const M2LocalRoster *roster,uint16_t id) {
    for (uint8_t i=0;i<roster->count;++i) if (roster->members[i].id==id) return 1;
    return 0;
}

/* Standalone signaling for peers in the opt-in local Open Course roster.
 * The per-peer IP is a loopback identifier for the local packet relay. */
enum { GOLF_SIGNAL_NOT_INITIALIZED=0x80552701u, GOLF_SIGNAL_ALREADY_INITIALIZED=0x80552702u,
       GOLF_SIGNAL_CONTEXT_NOT_FOUND=0x80552705u, GOLF_SIGNAL_CONNECTION_NOT_FOUND=0x8055270eu,
       GOLF_SIGNAL_PEER_UNREACHABLE=0x8055270fu, GOLF_SIGNAL_CONTEXT_MAX=0x80552712u,
       GOLF_SIGNAL_INVALID_ARGUMENT=0x80552715u, GOLF_SIGNAL_TOO_MANY_CONNECTIONS=0x80552717u,
       GOLF_SIGNAL_OWN_PEER_ADDRESS=0x8055271bu };

typedef struct { uint64_t account; uint32_t platform, padding; } GolfSignalAddress;
typedef struct { uint32_t id; uint16_t member; int departed, logged_status; } GolfSignalConnection;
/* These fields share the Matching2 mutex. Only its worker publishes rosters. */
static struct {
    uint64_t generation;
    struct timespec expires;
    M2LocalRoster roster;
    uint8_t world;
} m2_peer_roster;
static GolfSignalConnection golf_signals[M2_LOCAL_CAPACITY-1];
static uint32_t golf_next_signal=1;
static int golf_signaling_up, golf_signaling_context;

/* M0 diagnostics: print the lobby roster when its size or member ids change. */
static void m2_log_roster(uint8_t world,const M2LocalRoster *roster) {
    static uint8_t last_world, last_count;
    static uint16_t last_ids[M2_LOCAL_CAPACITY], last_own;
    int changed=world!=last_world || roster->count!=last_count || roster->own_id!=last_own;
    for (size_t i=0;!changed && i<roster->count && i<M2_LOCAL_CAPACITY;++i) changed=roster->members[i].id!=last_ids[i];
    if (!changed) return;
    last_world=world; last_count=roster->count; last_own=roster->own_id;
    char text[M2_LOCAL_CAPACITY*24+1]; size_t used=0; text[0]=0;
    for (size_t i=0;i<roster->count && i<M2_LOCAL_CAPACITY;++i) {
        last_ids[i]=roster->members[i].id;
        int n=snprintf(text+used,sizeof text-used,"%s%u:%.16s",i?",":"",(unsigned)roster->members[i].id,roster->members[i].name);
        if (n<0 || (size_t)n>=sizeof text-used) break;
        used+=(size_t)n;
    }
    printf("Runtime: lobby roster world %u own %u count %u members %s\n",(unsigned)world,(unsigned)roster->own_id,(unsigned)roster->count,text);
    fflush(stdout);
}
static void m2_publish_peer_roster(const M2LocalSession *session,const M2LocalRoster *roster) {
    if (roster) m2_log_roster(session->world,roster);
    pthread_mutex_lock(&m2_lock);
    if (roster && session->generation==m2_generation && m2_ctx==session->ctx &&
        !clock_gettime(CLOCK_MONOTONIC,&m2_peer_roster.expires)) {
        if (m2_peer_roster.world!=session->world ||
            memcmp(m2_peer_roster.roster.token,roster->token,16)) {
            memset(golf_signals,0,sizeof golf_signals);
        } else {
            for (size_t i=0;i<M2_LOCAL_CAPACITY-1;++i) {
                GolfSignalConnection *connection=&golf_signals[i];
                if (connection->id && !m2_roster_has(roster,connection->member)) connection->departed=1;
            }
        }
        m2_peer_roster.generation=session->generation;
        m2_peer_roster.expires.tv_sec+=15;
        m2_peer_roster.world=session->world;
        m2_peer_roster.roster=*roster;
    } else if (session->generation==m2_peer_roster.generation) {
        m2_peer_roster.world=0;
        memset(golf_signals,0,sizeof golf_signals);
    }
    pthread_mutex_unlock(&m2_lock);
}

/* Caller holds m2_lock. */
static int m2_peer_roster_live(void) {
    struct timespec now;
    return m2_peer_roster.world && m2_peer_roster.generation==m2_generation &&
           !clock_gettime(CLOCK_MONOTONIC,&now) &&
           (now.tv_sec<m2_peer_roster.expires.tv_sec ||
            (now.tv_sec==m2_peer_roster.expires.tv_sec && now.tv_nsec<m2_peer_roster.expires.tv_nsec));
}

static int32_t golf_signal_check_context(int32_t context) {
    if (!golf_signaling_up) return (int32_t)GOLF_SIGNAL_NOT_INITIALIZED;
    if (!golf_signaling_context || context!=1) return (int32_t)GOLF_SIGNAL_CONTEXT_NOT_FOUND;
    return 0;
}

static ABI int32_t golf_signal_initialize(int64_t memory,int32_t priority,int32_t affinity,int64_t stack) {
    (void)memory; (void)priority; (void)affinity; (void)stack;
    pthread_mutex_lock(&m2_lock);
    const int32_t result=golf_signaling_up ? (int32_t)GOLF_SIGNAL_ALREADY_INITIALIZED : 0;
    golf_signaling_up=1;
    pthread_mutex_unlock(&m2_lock);
    return result;
}

static ABI int32_t golf_signal_terminate(void) {
    pthread_mutex_lock(&m2_lock);
    const int32_t result=golf_signaling_up ? 0 : (int32_t)GOLF_SIGNAL_NOT_INITIALIZED;
    golf_signaling_up=golf_signaling_context=0;
    memset(golf_signals,0,sizeof golf_signals);
    pthread_mutex_unlock(&m2_lock);
    return result;
}

static ABI int32_t golf_signal_create(int32_t user,void *handler,void *arg,int32_t *context) {
    /* The peer tick queries status directly at 0xa747d7. */
    (void)handler; (void)arg;
    pthread_mutex_lock(&m2_lock);
    int32_t result=0;
    if (!golf_signaling_up) result=(int32_t)GOLF_SIGNAL_NOT_INITIALIZED;
    else if (user==USER_INVALID || !context) result=(int32_t)GOLF_SIGNAL_INVALID_ARGUMENT;
    else if (golf_signaling_context) result=(int32_t)GOLF_SIGNAL_CONTEXT_MAX;
    else {
        golf_signaling_context=1;
        *context=1;
    }
    pthread_mutex_unlock(&m2_lock);
    return result;
}

static ABI int32_t golf_signal_delete(int32_t context) {
    pthread_mutex_lock(&m2_lock);
    const int32_t result=golf_signal_check_context(context);
    if (!result) {
        golf_signaling_context=0;
        memset(golf_signals,0,sizeof golf_signals);
    }
    pthread_mutex_unlock(&m2_lock);
    return result;
}

static int32_t golf_signal_resolve_member(const GolfSignalAddress *address,uint16_t *member) {
    if (!address) return (int32_t)GOLF_SIGNAL_INVALID_ARGUMENT;
    GolfSignalAddress peer;
    memcpy(&peer,address,sizeof peer);
    const uint16_t candidate=(uint16_t)peer.account;
    if (peer.platform!=3 || !candidate || !m2_peer_roster_live() ||
        peer.account!=m2_lobby_account_id(m2_peer_roster.world,candidate) ||
        !m2_roster_has(&m2_peer_roster.roster,candidate)) return (int32_t)GOLF_SIGNAL_PEER_UNREACHABLE;
    if (candidate==m2_peer_roster.roster.own_id) return (int32_t)GOLF_SIGNAL_OWN_PEER_ADDRESS;
    *member=candidate;
    return 0;
}

static int32_t golf_signal_connection(int32_t context,const GolfSignalAddress *peer,uint32_t *id,int activate) {
    pthread_mutex_lock(&m2_lock);
    int32_t result=golf_signal_check_context(context);
    uint16_t member=0;
    if (!result) result=id ? golf_signal_resolve_member(peer,&member) : (int32_t)GOLF_SIGNAL_INVALID_ARGUMENT;
    if (!result) {
        GolfSignalConnection *connection=NULL, *free_slot=NULL;
        for (size_t i=0;i<M2_LOCAL_CAPACITY-1;++i) {
            GolfSignalConnection *candidate=&golf_signals[i];
            if (candidate->id && !candidate->departed && candidate->member==member) {
                connection=candidate;
                break;
            }
            if (!free_slot && (!candidate->id || candidate->departed))
                free_slot=candidate;
        }
        if (!connection && activate && free_slot) {
            *free_slot=(GolfSignalConnection){.id=golf_next_signal++,.member=member,.logged_status=-1};
            printf("Runtime: signaling activate connection %u -> member %u\n",(unsigned)free_slot->id,(unsigned)member);
            if (!golf_next_signal) golf_next_signal=1;
            connection=free_slot;
        }
        if (connection) *id=connection->id;
        else result=(int32_t)(activate ? GOLF_SIGNAL_TOO_MANY_CONNECTIONS : GOLF_SIGNAL_CONNECTION_NOT_FOUND);
    }
    pthread_mutex_unlock(&m2_lock);
    return result;
}

static ABI int32_t golf_signal_activate(int32_t context,const GolfSignalAddress *peer,uint32_t *id) {
    return golf_signal_connection(context,peer,id,1);
}

static ABI int32_t golf_signal_lookup(int32_t context,const GolfSignalAddress *peer,uint32_t *id) {
    return golf_signal_connection(context,peer,id,0);
}

/* Caller holds m2_lock. */
static GolfSignalConnection *golf_signal_find(uint32_t id) {
    for (size_t i=0;i<M2_LOCAL_CAPACITY-1;++i)
        if (id && golf_signals[i].id==id) return &golf_signals[i];
    return NULL;
}

static ABI int32_t golf_signal_status(int32_t context,uint32_t id,int32_t *status,uint32_t *ip,uint16_t *port) {
    pthread_mutex_lock(&m2_lock);
    int32_t result=golf_signal_check_context(context);
    if (!result) {
        const GolfSignalConnection *connection=golf_signal_find(id);
        if (!status || !ip || !port) result=(int32_t)GOLF_SIGNAL_INVALID_ARGUMENT;
        else if (!connection) result=(int32_t)GOLF_SIGNAL_CONNECTION_NOT_FOUND;
        else {
            const uint16_t member=connection->member;
            const int active=!connection->departed && m2_peer_roster_live();
            const uint8_t address[4]={127,0,(uint8_t)(member>>8),(uint8_t)member};
            const uint8_t physical[2]={3658>>8,3658&255};
            *status=active ? 2 : 0;
            if (((GolfSignalConnection *)connection)->logged_status!=*status) {
                ((GolfSignalConnection *)connection)->logged_status=*status;
                printf("Runtime: signaling connection %u member %u status %d (%s)\n",(unsigned)id,(unsigned)member,
                       (int)*status,active ? "active" : "inactive");
                fflush(stdout);
            }
            *ip=0;
            *port=0;
            if (active) {
                memcpy(ip,address,4);
                memcpy(port,physical,2);
            }
        }
    }
    pthread_mutex_unlock(&m2_lock);
    return result;
}

static ABI int32_t golf_signal_disconnect(int32_t context,uint32_t id) {
    pthread_mutex_lock(&m2_lock);
    const int32_t status=golf_signal_check_context(context);
    GolfSignalConnection *connection=status ? NULL : golf_signal_find(id);
    if (connection) *connection=(GolfSignalConnection){0};
    pthread_mutex_unlock(&m2_lock);
    return status ? status : connection ? 0 : (int32_t)GOLF_SIGNAL_CONNECTION_NOT_FOUND;
}

static ABI int32_t golf_signal_deactivate(int32_t context,uint32_t id) {
    /* With an inactive link, Golf passes context 0 at 0xa746cd. */
    return golf_signal_disconnect(context ? context : 1,id);
}

static const RuntimeExport golf_signaling_exports[]={
    {"sceNpSignalingInitialize",golf_signal_initialize},
    {"sceNpSignalingTerminate",golf_signal_terminate},
    {"sceNpSignalingCreateContextA",golf_signal_create},
    {"sceNpSignalingDeleteContext",golf_signal_delete},
    {"sceNpSignalingActivateConnectionA",golf_signal_activate},
    {"sceNpSignalingGetConnectionFromPeerAddressA",golf_signal_lookup},
    {"sceNpSignalingGetConnectionStatus",golf_signal_status},
    {"sceNpSignalingDeactivateConnection",golf_signal_deactivate},
    {"sceNpSignalingTerminateConnection",golf_signal_disconnect},
};

static void m2_room_clear(M2LocalSession *session,int notify);
static void m2_room_poll(M2LocalSession *session);
static void m2_room_messages_poll(M2LocalSession *session);

static void m2_lobby_close(M2LocalSession *session) {
    if (!session->world) return;
    m2_room_clear(session,0);
    m2_publish_peer_roster(session,NULL);
    m2_lobby_snapshot(M2_LOCAL_LEAVE,session->world,session->roster.token,NULL);
    memset(session,0,sizeof *session);
}

static void m2_lobby_poll(M2LocalSession *session);
static void m2_local_request(M2LocalSession *session,M2Event *event);

static void *m2_thread(void *unused) {
    (void)unused;
    runtime_thread_attach_host("NpMatching2");
    runtime_fault_trace=getenv("RESOGUN_FAULT_LOG")!=NULL;
    M2LocalSession session={0};
    for (;;) {
        pthread_mutex_lock(&m2_lock);
        while (!m2_count && !session.world) pthread_cond_wait(&m2_wake,&m2_lock);
        while (!m2_count && session.world && session.generation==m2_generation && !m2_lobby_due(&session)) {
            pthread_cond_timedwait(&m2_wake,&m2_lock,&session.next_poll);
        }
        if (session.world && session.generation!=m2_generation) {
            pthread_mutex_unlock(&m2_lock);
            m2_lobby_close(&session);
            continue;
        }
        if (session.world && (!m2_count || m2_lobby_due(&session))) {
            pthread_mutex_unlock(&m2_lock);
            m2_lobby_poll(&session);
            continue;
        }
        M2Event e=m2_queue[m2_head];
        m2_head=(m2_head+1)%32; --m2_count;
        pthread_mutex_unlock(&m2_lock);
        /* The start function stores "starting" (4) at obj+0x48 after ContextStart returns and some
         * setup (C: eboot 0xa7e5c9); the started event must come after it, as a server round trip would. */
        struct timespec nap={0,(e.req ? 30 : 500)*1000*1000}; nanosleep(&nap,NULL);
        pthread_mutex_lock(&m2_lock);
        const int current=m2_ctx && e.ctx==m2_ctx && e.generation==m2_generation;
        pthread_mutex_unlock(&m2_lock);
        if (!current) { free(e.data); continue; }
        if (local_lobbies) m2_local_request(&session,&e);
        pthread_mutex_lock(&m2_lock);
        const int deliver=m2_ctx && e.ctx==m2_ctx && e.generation==m2_generation;
        const M2ContextCb context_cb=m2_context_cb; void *context_arg=m2_context_arg;
        pthread_mutex_unlock(&m2_lock);
        if (!deliver) { free(e.data); continue; }
        if (e.cb) {
            printf("Runtime: NpMatching2 request %u answered: event 0x%x, error 0x%x\n",(unsigned)e.req,(unsigned)e.event,(unsigned)e.error);
            e.cb(e.ctx,e.req,e.event,e.error,e.error ? NULL : e.data,e.arg);
        } else if (!e.req && context_cb) {
            printf("Runtime: NpMatching2 context %u event 0x%x\n",(unsigned)e.ctx,(unsigned)e.event);
            context_cb(e.ctx,e.event,0,e.error,context_arg);
        }
        free(e.data);
    }
    return NULL;
}
/* m2_lock is held. The queue takes ownership of data on success. */
static int32_t m2_post_locked(M2Event e) {
    if (!m2_ctx || e.ctx!=m2_ctx) return M2_CONTEXT_NOT_FOUND;
    if (m2_count==32) return M2_BUSY;
    if (!m2_thread_up) {
        pthread_t thread;
        if (pthread_create(&thread,NULL,m2_thread,NULL)) return M2_OUT_OF_MEMORY;
        pthread_detach(thread);
        m2_thread_up=1;
    }
    e.generation=m2_generation;
    m2_queue[(m2_head+m2_count)%32]=e;
    ++m2_count;
    pthread_cond_signal(&m2_wake);
    return 0;
}
static int32_t m2_post(M2Event e) {
    pthread_mutex_lock(&m2_lock);
    const int32_t result=m2_post_locked(e);
    pthread_mutex_unlock(&m2_lock);
    return result;
}
/* The worker owns any event already popped from the queue. */
static void m2_clear_queue_locked(void) {
    while (m2_count) {
        free(m2_queue[m2_head].data);
        m2_head=(m2_head+1)%32;
        --m2_count;
    }
}
static ABI int32_t m2_create_context_a(const uint8_t *param,uint16_t *ctx) {
    if (!param || !ctx) return NP_INVALID_ARGUMENT;
    pthread_mutex_lock(&m2_lock);
    ++m2_generation;
    m2_clear_queue_locked();
    m2_lobby_cb=NULL; m2_lobby_arg=NULL;
    m2_room_cb=NULL; m2_room_arg=NULL;
    const uint64_t old_room_delivery=m2_room_delivery;
    pthread_cond_signal(&m2_wake);
    m2_context_cb=NULL; m2_context_arg=NULL;
    memset(&m2_default_opt,0,sizeof m2_default_opt);
    const uint64_t old_message_delivery=m2_room_message_delivery;
    const uint64_t old_signal_delivery=m2_room_signal_delivery;
    m2_room_signal_cb=NULL; m2_room_signal_arg=NULL;
    ++m2_room_signal_registration;
    m2_room_message_cb=NULL; m2_room_message_arg=NULL;
    ++m2_room_message_registration;
    *ctx=m2_ctx=1;
    m2_room_wait_locked(old_room_delivery);
    m2_room_message_wait_locked(old_message_delivery);
    m2_room_signal_wait_locked(old_signal_delivery);
    pthread_mutex_unlock(&m2_lock);
    puts("Runtime: NpMatching2 context 1 created (local)");
    return 0;
}
/* RegisterContextCallback(callback, arg): one callback for all contexts, no context id. */
static ABI int32_t m2_register_context_cb(M2ContextCb cb,void *arg) {
    pthread_mutex_lock(&m2_lock);
    m2_context_cb=cb; m2_context_arg=arg;
    pthread_mutex_unlock(&m2_lock);
    return 0;
}
static ABI int32_t m2_register_lobby_cb(uint16_t ctx,M2LobbyCb cb,void *arg) {
    pthread_mutex_lock(&m2_lock);
    if (!m2_ctx || ctx!=m2_ctx) {
        pthread_mutex_unlock(&m2_lock);
        return M2_CONTEXT_NOT_FOUND;
    }
    m2_lobby_cb=cb; m2_lobby_arg=arg;
    pthread_mutex_unlock(&m2_lock);
    return 0;
}
static ABI int32_t m2_register_room_cb(uint16_t ctx,M2RoomCb cb,void *arg) {
    if (!local_lobbies) return 0;
    pthread_mutex_lock(&m2_lock);
    if (!m2_ctx || ctx!=m2_ctx) {
        pthread_mutex_unlock(&m2_lock);
        return M2_CONTEXT_NOT_FOUND;
    }
    const uint64_t old_room_delivery=m2_room_delivery;
    m2_room_cb=cb; m2_room_arg=arg;
    m2_room_wait_locked(old_room_delivery);
    pthread_mutex_unlock(&m2_lock);
    return 0;
}
static ABI int32_t m2_register_room_message_cb(uint16_t ctx,M2RoomMessageCb cb,void *arg) {
    if (!local_lobbies) return 0;
    pthread_mutex_lock(&m2_lock);
    if (!m2_ctx || ctx!=m2_ctx) {
        pthread_mutex_unlock(&m2_lock);
        return M2_CONTEXT_NOT_FOUND;
    }
    const uint64_t old_delivery=m2_room_message_delivery;
    m2_room_message_cb=cb; m2_room_message_arg=arg;
    ++m2_room_message_registration;
    m2_room_message_wait_locked(old_delivery);
    pthread_mutex_unlock(&m2_lock);
    return 0;
}
static ABI int32_t m2_register_room_signal_cb(uint16_t ctx,M2RoomSignalCb cb,void *arg) {
    if (!local_lobbies) return 0;
    pthread_mutex_lock(&m2_lock);
    if (!m2_ctx || ctx!=m2_ctx) {
        pthread_mutex_unlock(&m2_lock);
        return M2_CONTEXT_NOT_FOUND;
    }
    const uint64_t old_delivery=m2_room_signal_delivery;
    m2_room_signal_cb=cb; m2_room_signal_arg=arg;
    ++m2_room_signal_registration;
    m2_room_signal_wait_locked(old_delivery);
    pthread_mutex_unlock(&m2_lock);
    return 0;
}
static ABI int32_t m2_register_cb(uint16_t ctx,void *cb,void *arg) { (void)ctx; (void)cb; (void)arg; return 0; }
static ABI int32_t m2_context_start(uint16_t ctx,uint64_t timeout) {
    (void)timeout;
    return m2_post((M2Event){.ctx=ctx,.event=0x6f02});
}
static ABI int32_t m2_context_stop(uint16_t ctx) {
    return m2_post((M2Event){.ctx=ctx,.event=0x6f03});
}
static ABI int32_t m2_destroy_context(uint16_t ctx) {
    (void)ctx;
    pthread_mutex_lock(&m2_lock);
    ++m2_generation;
    m2_clear_queue_locked();
    m2_lobby_cb=NULL; m2_lobby_arg=NULL;
    m2_room_cb=NULL; m2_room_arg=NULL;
    const uint64_t old_room_delivery=m2_room_delivery;
    pthread_cond_signal(&m2_wake);
    const uint64_t old_message_delivery=m2_room_message_delivery;
    const uint64_t old_signal_delivery=m2_room_signal_delivery;
    m2_room_signal_cb=NULL; m2_room_signal_arg=NULL;
    ++m2_room_signal_registration;
    m2_room_message_cb=NULL; m2_room_message_arg=NULL;
    ++m2_room_message_registration;
    m2_ctx=0; m2_context_cb=NULL; m2_context_arg=NULL;
    memset(&m2_default_opt,0,sizeof m2_default_opt);
    m2_room_wait_locked(old_room_delivery);
    m2_room_message_wait_locked(old_message_delivery);
    m2_room_signal_wait_locked(old_signal_delivery);
    pthread_mutex_unlock(&m2_lock);
    return 0;
}
/* sceNpMatching2GetServerId(ctx, u16 *serverId): the one local server. C: eboot 0xa8133b. */
static ABI int32_t m2_server_id(uint16_t ctx,uint16_t *server) {
    (void)ctx;
    if (!server) return NP_INVALID_ARGUMENT;
    *server=1;
    return 0;
}
/* sceNpSignalingCreateContextA(user, callback, arg, ctxId *): C: eboot 0xa7e4f0, must return 0. */
static ABI int32_t m2_signaling_context_a(int32_t user,void *cb,void *arg,int32_t *ctx) {
    (void)cb; (void)arg;
    if (user==USER_INVALID || !ctx) return NP_INVALID_ARGUMENT;
    *ctx=m2_signaling_ctx=1;
    return 0;
}
/* Requests (ctx, request *, optParam *, u32 *requestId). optParam is {callback, arg, u32 timeout}
 * (24 bytes; C: eboot 0xa81385..0xa813ac); a null optParam means the one stored by
 * SetDefaultRequestOptParam. Each answer is the smallest one the game accepts, layouts from the
 * game's own reads (analysis/online/matching2-entry-contract.md):
 * GetWorldInfoList -> event 0x2, {rows *, u32 count}, 0x28-byte rows (u32 worldId at +8, u32 lobby
 *   count at +0xc, population at +0x14): worlds 1..50 (M2_WORLDS), one lobby each.
 * GetLobbyInfoList -> event 0x6, {.., u32 count at +8, rows * at +0x10}, 0x50-byte rows (u64 lobbyId
 *   at +0x10, u16 members at +0x1a): the one lobby, empty.
 * JoinLobby -> event 0x201, {P *}: P+0x10 u64 lobbyId, P+0x18 u16 memberIds *, P+0x20 u64 count,
 *   P+0x28 u16 own member id.
 * GetLobbyMemberDataInternalList -> event 0x7207, {first record *, u64 count}; record: next * at +0,
 *   online id at +0x20, u16 member id at +0x34: the local player alone.
 * LeaveLobby -> event 0x202, no data. */
/* The game's world table (*(*0x2c7e0a0+0x38), 0x18-byte entries, ids 0..49) names worlds 1..50; the
 * Open Course screen only goes on when the list has the row for its chosen world (eboot 0xef44a). */
#define M2_WORLDS 50
static uint32_t m2_next_request=1;
/* The SDK response is the prefix. Its nested pointers refer to the same allocation. */
typedef struct {
    void *rows;
    uint64_t count;
    uint8_t records[M2_WORLDS][0x28];
} M2WorldResult;
typedef struct {
    uint64_t pad;
    uint32_t count, pad2;
    void *rows;
    uint8_t record[0x50];
} M2LobbyResult;
typedef struct {
    void *info;
    uint8_t record[0x30];
    uint16_t member_ids[M2_LOCAL_CAPACITY];
} M2JoinResult;
typedef struct {
    void *first;
    uint64_t count;
    uint8_t records[M2_LOCAL_CAPACITY][0x40];
} M2MemberResult;
static int m2_lobby_notify(const M2LocalSession *session,uint16_t event,const M2LocalMember *member) {
    pthread_mutex_lock(&m2_lock);
    const int current=m2_ctx==session->ctx && m2_generation==session->generation;
    const M2LobbyCb cb=m2_lobby_cb;
    void *arg=m2_lobby_arg;
    pthread_mutex_unlock(&m2_lock);
    if (!current || !cb) return 0;
    uint8_t record[0x40]={0};
    const void *data=record;
    if (member) m2_lobby_record(record,member,NULL,session->world);
    cb(session->ctx,session->game_lobby ? session->game_lobby : session->world,event,member ? &data : NULL,arg);
    return 1;
}

static void m2_lobby_poll(M2LocalSession *session) {
    M2LocalRoster latest;
    const int status=m2_lobby_snapshot(M2_LOCAL_POLL,session->world,session->roster.token,&latest);
    m2_lobby_deadline(session);
    if (status==404) {
        m2_room_clear(session,1);
        m2_publish_peer_roster(session,NULL);
        m2_lobby_notify(session,0x3203,NULL);
        memset(session,0,sizeof *session);
        return;
    }
    if (status!=200 || latest.own_id!=session->roster.own_id) return;
    m2_publish_peer_roster(session,&latest);
    m2_room_poll(session);
    m2_room_messages_poll(session);
    if (!session->ready) return;
    M2LocalRoster *known=&session->roster;
    const uint8_t previous_count=known->count;
    uint8_t remaining=0;
    for (uint8_t i=0;i<previous_count;++i) {
        const M2LocalMember *member=&known->members[i];
        if (!m2_roster_has(&latest,member->id) && m2_lobby_notify(session,0xa202,member)) continue;
        known->members[remaining++]=*member;
    }
    known->count=remaining;
    /* Give the guest update loop time to release departed members' slots. */
    if (remaining!=previous_count) return;
    for (uint8_t i=0;i<latest.count;++i) {
        const M2LocalMember *member=&latest.members[i];
        if (!m2_roster_has(known,member->id) && !m2_lobby_notify(session,0xa201,member)) return;
    }
    *known=latest;
}

static void m2_room_complete(M2LocalSession *session,M2Event *event);
static void m2_room_message_complete(M2LocalSession *session,M2Event *event);

static void m2_local_request(M2LocalSession *session,M2Event *event) {
    if (event->event==0x0108) {
        m2_room_message_complete(session,event);
        return;
    }
    if (event->event==0x7101 || event->event==0x7102 || event->event==0x0103 ||
        event->event==0x7106 || event->event==0x7005) {
        m2_room_complete(session,event);
        return;
    }
    if (event->event==0x201) {
        M2JoinResult *reply=event->data;
        const uint8_t world=(uint8_t)event->lobby;
        M2LocalRoster roster;
        int status;
        if (session->world==world) {
            status=m2_lobby_snapshot(M2_LOCAL_POLL,world,session->roster.token,&roster);
            if (status!=200) {
                event->error=M2_SERVER_NOT_AVAILABLE;
                return;
            }
        } else {
            m2_lobby_close(session);
            status=m2_lobby_snapshot(M2_LOCAL_JOIN,world,NULL,&roster);
            if (status!=200) {
                event->error=status==409 ? M2_BUSY : M2_SERVER_NOT_AVAILABLE;
                return;
            }
        }
        session->world=world;
        memcpy(&session->game_lobby,reply->record+0x10,8);
        session->ctx=event->ctx;
        session->generation=event->generation;
        session->ready=0;
        session->roster=roster;
        m2_publish_peer_roster(session,&roster);
        m2_lobby_deadline(session);
        const uint64_t count=roster.count;
        memcpy(reply->record+0x20,&count,8);
        memcpy(reply->record+0x28,&roster.own_id,2);
        for (uint8_t i=0;i<roster.count;++i) reply->member_ids[i]=roster.members[i].id;
    } else if (event->event==0x7207) {
        if (!session->world || session->world!=event->lobby) {
            event->error=M2_INVALID_LOBBY;
            return;
        }
        M2MemberResult *reply=event->data;
        reply->first=reply->records;
        reply->count=session->roster.count;
        for (uint8_t i=0;i<session->roster.count;++i) {
            void *next=i+1<session->roster.count ? reply->records[i+1] : NULL;
            m2_lobby_record(reply->records[i],&session->roster.members[i],next,session->world);
        }
        session->ready=1;
    } else if (event->event==0x202 && event->lobby==session->world) {
        m2_lobby_close(session);
    } else if (event->event==0x6f03) {
        m2_lobby_close(session);
    }
}

static ABI int32_t m2_set_default_opt(uint16_t ctx,const M2Opt *opt) {
    (void)ctx;
    pthread_mutex_lock(&m2_lock);
    if (opt) m2_default_opt=*opt; else memset(&m2_default_opt,0,sizeof m2_default_opt);
    pthread_mutex_unlock(&m2_lock);
    return 0;
}
static int32_t m2_answer(const char *what,uint16_t ctx,const M2Opt *opt,uint32_t *id,uint16_t event,void *data,uint64_t lobby) {
    pthread_mutex_lock(&m2_lock);
    const M2Opt use=opt ? *opt : m2_default_opt;
    const uint32_t request=m2_next_request++;
    int32_t result=0;
    if (!m2_ctx || ctx!=m2_ctx) result=M2_CONTEXT_NOT_FOUND;
    else if (use.cb || event==0x0108) {
        result=m2_post_locked((M2Event){.cb=use.cb,.arg=use.arg,.data=data,.req=request,.event=event,.ctx=ctx,.lobby=lobby});
    }
    if (!result && id) *id=request;
    pthread_mutex_unlock(&m2_lock);
    if (result || (!use.cb && event!=0x0108)) free(data);
    if (result) return result;
    printf("Runtime: NpMatching2 %s: request %u (local answer follows)\n",what,(unsigned)request);
    return 0;
}
/* Translate the Golf room requests and account-ID callbacks to the local
 * room route. All guest request data is copied before queueing. */
enum { M2_ROOM_CREATE=1, M2_ROOM_JOIN=2, M2_ROOM_VIEW=3, M2_ROOM_LEAVE=4, M2_ROOM_SEARCH=5, M2_ROOM_EXTERNAL=6,
       M2_ROOM_ATTRIBUTE=256, M2_ROOM_RESULTS=20, M2_ROOM_HEADER=20 };

typedef struct { uint16_t id; uint8_t pad[6]; const uint8_t *data; uint64_t size; } M2RoomBin;
typedef struct {
    uint64_t tick;
    uint16_t member;
    uint8_t pad[6];
    M2RoomBin bin;
} M2RoomInternalBin;
typedef struct { uint32_t world; size_t size; uint8_t request[GOLF_ROOM_REQUEST_MAX]; } M2RoomJob;
typedef struct {
    M2LocalRoom room;
    uint32_t flags;
    uint16_t capacity, size;
    const uint8_t *listing;
} M2RoomView;
typedef struct {
    void *room, *members;
    uint64_t count;
    void *self, *owner;
    uint8_t internal[0x58], records[M2_LOCAL_CAPACITY][0x58], listing[M2_ROOM_ATTRIBUTE];
    M2RoomInternalBin bin;
} M2RoomJoinReply;
typedef struct {
    union {
        struct { uint32_t start, total, count, pad; void *first; } search;
        struct { void *first; uint64_t count; } external;
    } result;
    uint8_t records[M2_ROOM_RESULTS][0xa8], listing[M2_ROOM_RESULTS][M2_ROOM_ATTRIBUTE];
    M2RoomBin bins[M2_ROOM_RESULTS];
} M2RoomListReply;

static uint16_t m2_room_u16(const void *p) { uint16_t n; memcpy(&n,p,2); return n; }
static uint32_t m2_room_u32(const void *p) { uint32_t n; memcpy(&n,p,4); return n; }
static uint64_t m2_room_u64(const void *p) { uint64_t n; memcpy(&n,p,8); return n; }

static const uint8_t *m2_room_pointer(const void *p) {
    const uint8_t *address;
    memcpy(&address,p,sizeof address);
    return address;
}

static const M2LocalMember *m2_room_find(const M2LocalRoom *room,uint16_t member) {
    for (uint16_t i=0;i<room->count;++i)
        if (room->members[i].id==member) return &room->members[i];
    return NULL;
}

static int m2_room_decode(const M2LocalSession *session,const uint8_t *body,size_t length,M2RoomView *view) {
    if (length<30) return 0;
    const uint64_t room=m2_room_u64(body);
    const uint16_t capacity=m2_room_u16(body+20), count=m2_room_u16(body+22);
    const uint16_t owner=m2_room_u16(body+24), self=m2_room_u16(body+26), size=m2_room_u16(body+28);
    const uint64_t revision=m2_room_u64(body+8);
    const size_t base_size=30+(size_t)count*18+size;
    if ((room>>56)!=session->world || !revision || !count || capacity>M2_LOCAL_CAPACITY || count>capacity ||
        size>M2_ROOM_ATTRIBUTE || (length!=base_size && length!=base_size+4) ||
        self!=session->roster.own_id) return 0;
    uint8_t signaling=0;
    uint16_t hub=0;
    if (length>base_size) {
        const uint8_t *tail=body+base_size;
        signaling=tail[0];
        hub=m2_room_u16(tail+2);
        if (tail[1] || (signaling!=1 && signaling!=2) || (signaling==1 ? hub!=0 : hub!=owner)) return 0;
    }
    *view=(M2RoomView){.room={.id=room,.revision=revision,.owner=owner,.hub=hub,.signaling=signaling},.flags=m2_room_u32(body+16),
                       .capacity=capacity,.size=size,.listing=body+30+(size_t)count*18};
    for (uint16_t i=0;i<count;++i) {
        const uint8_t *row=body+30+(size_t)i*18;
        const uint16_t id=m2_room_u16(row);
        if (!id || m2_room_find(&view->room,id) || !m2_lobby_name_char(row[2])) return 0;
        const size_t name_len=strnlen((const char *)row+2,16);
        for (size_t j=0;j<name_len;++j) if (!m2_lobby_name_char(row[2+j])) return 0;
        for (size_t j=name_len;j<16;++j) if (row[2+j]) return 0;
        M2LocalMember *member=&view->room.members[view->room.count++];
        member->id=id;
        memcpy(member->name,row+2,name_len);
    }
    return m2_room_find(&view->room,self) && m2_room_find(&view->room,owner);
}

static void m2_room_member_record(uint8_t *record,uint8_t world,const M2LocalMember *member,uint16_t owner) {
    const uint64_t account=m2_lobby_account_id(world,member->id);
    const uint32_t platform=3, flags=member->id==owner ? 0x80000000u : 0;
    memcpy(record+0x10,&account,8);
    memcpy(record+0x18,&platform,4);
    memcpy(record+0x20,member->name,17);
    memcpy(record+0x38,&member->id,2);
    memcpy(record+0x3c,&flags,4);
}

static void *m2_room_join_reply(const M2LocalSession *session,const M2RoomView *view) {
    /* Internal room at response +0, then chain/count/self/owner (0xa79d51–0xa79f40). */
    const uint64_t room=view->room.id;
    const uint16_t capacity=view->capacity, count=view->room.count;
    const uint16_t owner=view->room.owner, self=session->roster.own_id, size=view->size;
    M2RoomJoinReply *reply=calloc(1,sizeof *reply);
    if (!reply) return NULL;
    reply->room=reply->internal;
    reply->members=reply->records[0];
    reply->count=count;
    const uint16_t open=capacity-count, server=1;
    const uint32_t world=session->world;
    memcpy(reply->internal,&capacity,2);
    memcpy(reply->internal+4,&open,2);
    memcpy(reply->internal+8,&capacity,2);
    memcpy(reply->internal+0x0a,&server,2);
    memcpy(reply->internal+0x0c,&world,4);
    memcpy(reply->internal+0x18,&room,8);
    memcpy(reply->internal+0x40,&view->flags,4);
    if (size) {
        memcpy(reply->listing,view->listing,size);
        reply->bin=(M2RoomInternalBin){.member=owner,.bin={.id=0x57,.data=reply->listing,.size=size}};
        const M2RoomInternalBin *bin=&reply->bin;
        const uint64_t one=1;
        memcpy(reply->internal+0x48,&bin,8);
        memcpy(reply->internal+0x50,&one,8);
    }
    for (uint16_t i=0;i<count;++i) {
        const M2LocalMember *member=&view->room.members[i];
        uint8_t *record=reply->records[i];
        void *next=i+1<count ? reply->records[i+1] : NULL;
        memcpy(record,&next,8);
        m2_room_member_record(record,session->world,member,owner);
        if (member->id==self) reply->self=record;
        if (member->id==owner) reply->owner=record;
    }
    return reply;
}

static void *m2_room_list_reply(uint8_t world,int search,const uint8_t *body,size_t length) {
    if (length<12) return NULL;
    const uint32_t start=m2_room_u32(body), total=m2_room_u32(body+4), count=m2_room_u32(body+8);
    if (!start || count>M2_ROOM_RESULTS || count>total || (!search && count!=1)) return NULL;
    M2RoomListReply *reply=calloc(1,sizeof *reply);
    if (!reply) return NULL;
    void *first=count ? reply->records[0] : NULL;
    if (search) {
        reply->result.search.start=start;
        reply->result.search.total=total;
        reply->result.search.count=count;
        reply->result.search.first=first;
    } else {
        reply->result.external.first=first;
        reply->result.external.count=count;
    }
    size_t offset=12;
    const uint16_t server=1;
    const uint32_t world_id=world, platform=3;
    for (uint32_t i=0;i<count;++i) {
        if (length-offset<44) goto invalid;
        const uint8_t *row=body+offset;
        const uint64_t id=m2_room_u64(row);
        const uint16_t capacity=m2_room_u16(row+20), members=m2_room_u16(row+22);
        const uint16_t owner=m2_room_u16(row+24), blob=m2_room_u16(row+26);
        offset+=44;
        if ((id>>56)!=world || !owner || !members || capacity>M2_LOCAL_CAPACITY || members>capacity ||
            blob>M2_ROOM_ATTRIBUTE || length-offset<blob) goto invalid;
        const uint64_t account=m2_lobby_account_id(world,owner);
        const uint16_t open=capacity-members;
        void *next=i+1<count ? reply->records[i+1] : NULL;
        uint8_t *record=reply->records[i];
        memcpy(record,&next,8);
        memcpy(record+8,row+20,4);
        memcpy(record+0x0c,row+16,4);
        memcpy(record+0x10,&server,2);
        memcpy(record+0x14,&world_id,4);
        memcpy(record+0x20,&id,8);
        memcpy(record+0x38,&capacity,2);
        memcpy(record+0x3c,&open,2);
        memcpy(record+0x40,&account,8);
        memcpy(record+0x48,&platform,4);
        memcpy(record+0x50,row+28,16);
        if (blob) {
            memcpy(reply->listing[i],body+offset,blob);
            reply->bins[i]=(M2RoomBin){.id=0x55,.data=reply->listing[i],.size=blob};
            const M2RoomBin *bin=&reply->bins[i];
            const uint64_t one=1;
            memcpy(record+0x98,&bin,8);
            memcpy(record+0xa0,&one,8);
        }
        offset+=blob;
    }
    if (offset==length) return reply;
invalid:
    free(reply);
    return NULL;
}

/* A status call reads the worker's last valid room snapshot under m2_lock. */
static struct {
    M2LocalRoom room;
    uint64_t generation, transport_epoch;
    uint8_t world;
    struct timespec expires;
} m2_room_status_view;

static void m2_room_publish(const M2LocalSession *session) {
    pthread_mutex_lock(&m2_lock);
    if (m2_ctx==session->ctx && m2_generation==session->generation) {
        if (m2_room_status_view.room.id!=session->room.id)
            ++m2_room_status_view.transport_epoch;
        m2_room_status_view.room=session->room;
        m2_room_status_view.generation=session->generation;
        m2_room_status_view.world=session->world;
        if (clock_gettime(CLOCK_MONOTONIC,&m2_room_status_view.expires))
            m2_room_status_view.room.id=0;
        else m2_room_status_view.expires.tv_sec+=3;
    } else if (m2_room_status_view.generation==session->generation) {
        if (m2_room_status_view.room.id) ++m2_room_status_view.transport_epoch;
        m2_room_status_view.room.id=0;
    }
    pthread_mutex_unlock(&m2_lock);
}

static void m2_room_notify(const M2LocalSession *session,uint64_t room,uint16_t event,const void *data) {
    pthread_mutex_lock(&m2_lock);
    const int current=m2_ctx==session->ctx && m2_generation==session->generation;
    M2RoomCb cb=current ? m2_room_cb : NULL;
    void *arg=m2_room_arg;
    if (cb) m2_room_delivery=++m2_room_delivery_sequence;
    pthread_mutex_unlock(&m2_lock);
    if (!cb) return;
    m2_inside_room_cb=1;
    cb(session->ctx,room,event,data,arg);
    m2_inside_room_cb=0;
    pthread_mutex_lock(&m2_lock);
    m2_room_delivery=0;
    pthread_cond_broadcast(&m2_room_delivery_done);
    pthread_mutex_unlock(&m2_lock);
}

static void m2_room_notify_member(const M2LocalSession *session,uint16_t event,
                                  const M2LocalMember *member,uint16_t owner) {
    /* Callback 0xa7ea80 dereferences data once to copy the member record. */
    uint8_t record[0x58]={0};
    struct { const uint8_t *member; uint8_t cause, pad[7]; uint64_t presence[3]; } data={
        .member=record,.cause=event==0x8102 ? 5 : 0
    };
    m2_room_member_record(record,session->world,member,owner);
    m2_room_notify(session,session->room.id,event,&data);
}

static int m2_room_link(const M2LocalRoom *room,uint16_t self,uint16_t peer) {
    return self!=peer && (!room->hub || self==room->hub || peer==room->hub) && m2_room_find(room,peer);
}

static int m2_room_signal_emit(const M2LocalSession *session,uint64_t registration,uint64_t room,
                               uint16_t member,uint16_t event,int32_t error) {
    pthread_mutex_lock(&m2_lock);
    const int current=m2_ctx==session->ctx && m2_generation==session->generation &&
                      m2_room_signal_registration==registration;
    const M2RoomSignalCb cb=current ? m2_room_signal_cb : NULL;
    void *arg=m2_room_signal_arg;
    if (cb) m2_room_signal_delivery=++m2_room_delivery_sequence;
    pthread_mutex_unlock(&m2_lock);
    if (!cb) return 0;
    m2_inside_room_cb=1;
    cb(session->ctx,room,member,event,error,arg);
    m2_inside_room_cb=0;
    pthread_mutex_lock(&m2_lock);
    m2_room_signal_delivery=0;
    pthread_cond_broadcast(&m2_room_delivery_done);
    pthread_mutex_unlock(&m2_lock);
    return 1;
}

static void m2_room_signals_sync(M2LocalSession *session) {
    pthread_mutex_lock(&m2_lock);
    const int enabled=m2_room_signal_cb && m2_ctx==session->ctx && m2_generation==session->generation;
    const uint64_t registration=m2_room_signal_registration;
    pthread_mutex_unlock(&m2_lock);
    if (!enabled) return;
    if (registration!=session->signal_registration) {
        session->signal_registration=registration;
        session->advertised_links=(M2LocalRoom){0};
    }
    M2LocalRoom current={.id=session->room.id};
    const uint16_t self=session->roster.own_id;
    if (session->room.signaling) {
        for (uint16_t i=0;i<session->room.count;++i) {
            const M2LocalMember *member=&session->room.members[i];
            if (member->id!=self && (!session->room.hub || self==session->room.hub || member->id==session->room.hub))
                current.members[current.count++].id=member->id;
        }
    }
    const M2LocalRoom *previous=&session->advertised_links;
    for (uint16_t i=0;i<previous->count;++i) {
        const uint16_t member=previous->members[i].id;
        if ((previous->id!=current.id || !m2_room_find(&current,member)) &&
            !m2_room_signal_emit(session,registration,previous->id,member,0x5101,(int32_t)0x80550e10u))
            return;
    }
    for (uint16_t i=0;i<current.count;++i) {
        const uint16_t member=current.members[i].id;
        if ((previous->id!=current.id || !m2_room_find(previous,member)) &&
            !m2_room_signal_emit(session,registration,current.id,member,0x5102,0))
            return;
    }
    session->advertised_links=current;
}

static void m2_room_clear(M2LocalSession *session,int notify) {
    const uint64_t id=session->room.id;
    if (!id) return;
    memset(&session->room,0,sizeof session->room);
    memset(session->room_message_relay,0,sizeof session->room_message_relay);
    session->room_message_ack=0;
    m2_room_publish(session);
    if (notify) {
        /* 0xa7eab0 copies 32 bytes. 0xa81ebb reads error at +4 and cause at +0. */
        struct { uint8_t cause,pad[3]; int32_t error; uint64_t presence[3]; } data={
            .cause=4,.error=(int32_t)0x80550c30u
        };
        m2_room_notify(session,id,0x1104,&data);
        m2_room_signals_sync(session);
    } else session->advertised_links=(M2LocalRoom){0};
}

static void m2_room_poll(M2LocalSession *session) {
    if (!session->room.id) return;
    const M2LocalRoom previous=session->room;
    uint8_t request[M2_ROOM_HEADER+8]={M2_ROOM_VIEW,session->world};
    uint8_t response[GOLF_ROOM_RESPONSE_MAX];
    size_t length=0;
    memcpy(request+4,session->roster.token,16);
    memcpy(request+M2_ROOM_HEADER,&previous.id,8);
    const int status=runtime_golf_room_exchange(request,sizeof request,response,&length);
    if (status==404) { m2_room_clear(session,1); return; }
    M2RoomView view;
    if (status!=200 || !m2_room_decode(session,response,length,&view) || view.room.id!=previous.id ||
        view.room.revision<previous.revision) return;
    session->room=view.room;
    m2_room_publish(session);
    if (view.room.revision==previous.revision) {
        m2_room_signals_sync(session);
        return;
    }
    for (uint16_t i=0;i<previous.count;++i) {
        const M2LocalMember *member=&previous.members[i];
        if (!m2_room_find(&view.room,member->id)) m2_room_notify_member(session,0x8102,member,previous.owner);
    }
    for (uint16_t i=0;i<view.room.count;++i) {
        const M2LocalMember *member=&view.room.members[i];
        if (!m2_room_find(&previous,member->id)) m2_room_notify_member(session,0x8101,member,view.room.owner);
    }
    if (previous.owner!=view.room.owner) {
        /* The owner tick reads the new ID at payload offset 2 (0xa81ef3). */
        uint8_t data[0x28]={0};
        memcpy(data,&previous.owner,2);
        memcpy(data+2,&view.room.owner,2);
        m2_room_notify(session,view.room.id,0x1105,data);
    }
    m2_room_signals_sync(session);
}

static ABI int32_t m2_room_connection_status(uint16_t ctx,uint64_t room,uint16_t member,
                                            int32_t *status,uint32_t *ip,uint16_t *port) {
    pthread_mutex_lock(&m2_lock);
    int32_t result=0;
    struct timespec now;
    if (!m2_ctx || ctx!=m2_ctx) result=(int32_t)0x80550e05u;
    else if (!status || !ip || !port || !member || member==m2_peer_roster.roster.own_id)
        result=(int32_t)0x80550e15u;
    else if (!room || m2_room_status_view.room.id!=room || m2_room_status_view.generation!=m2_generation ||
             !m2_peer_roster_live() || m2_room_status_view.world!=m2_peer_roster.world ||
             clock_gettime(CLOCK_MONOTONIC,&now) || now.tv_sec>m2_room_status_view.expires.tv_sec ||
             (now.tv_sec==m2_room_status_view.expires.tv_sec && now.tv_nsec>=m2_room_status_view.expires.tv_nsec)) {
        result=(int32_t)0x80550c30u;
    } else {
        const int active=m2_room_link(&m2_room_status_view.room,m2_peer_roster.roster.own_id,member);
        const uint8_t address[4]={127,0,(uint8_t)(member>>8),(uint8_t)member};
        const uint8_t physical[2]={3658>>8,3658&255};
        *status=active ? 2 : 0;
        *ip=0;
        *port=0;
        if (active) {
            memcpy(ip,address,4);
            memcpy(port,physical,2);
        }
    }
    pthread_mutex_unlock(&m2_lock);
    return result;
}

static void m2_room_complete(M2LocalSession *session,M2Event *event) {
    M2RoomJob *job=event->data;
    const uint8_t operation=job->request[0];
    const uint64_t leaving=operation==M2_ROOM_LEAVE ? m2_room_u64(job->request+M2_ROOM_HEADER) : 0;
    uint8_t response[GOLF_ROOM_RESPONSE_MAX];
    size_t length=0;
    int status=404;
    /* Game Room calls CreateJoinRoomA straight after GetWorldInfoList, with no JoinLobby, so no
       roster holds this member yet: enter the request's world (world 1 when it names none) here.
       ready stays 0, so this roster sends no lobby member events the game never asked for. */
    if (!session->world && job->world<=255) {
        const uint8_t world=job->world ? (uint8_t)job->world : 1;
        M2LocalRoster roster;
        const int joined=m2_lobby_snapshot(M2_LOCAL_JOIN,world,NULL,&roster);
        fprintf(stderr,"Runtime: NpMatching2 room request entered world %u: status %d\n",world,joined);
        if (joined==200) {
            session->world=world;
            session->ctx=event->ctx;
            session->generation=event->generation;
            session->ready=0;
            session->roster=roster;
            m2_publish_peer_roster(session,&roster);
            m2_lobby_deadline(session);
        }
    }
    if (session->world && session->generation==event->generation &&
        (!job->world || job->world==session->world)) {
        job->request[1]=session->world;
        memcpy(job->request+4,session->roster.token,16);
        if (operation==M2_ROOM_CREATE) {
            const uint64_t nonce=event->req;
            memcpy(job->request+M2_ROOM_HEADER,&nonce,8);
        }
        status=runtime_golf_room_exchange(job->request,job->size,response,&length);
        /* The listener can have committed an idempotent create or join before its reply was lost. */
        if (!status && (operation==M2_ROOM_CREATE || operation==M2_ROOM_JOIN)) {
            length=0;
            status=runtime_golf_room_exchange(job->request,job->size,response,&length);
        }
    }
    free(job);
    event->data=NULL;
    if (status!=200) {
        event->error=status==404 ? (int32_t)0x80550c30u :
                     status==409 && operation==M2_ROOM_JOIN ? (int32_t)0x80550d19u :
                     status==409 ? M2_BUSY : status==400 ? NP_INVALID_ARGUMENT : M2_SERVER_NOT_AVAILABLE;
        return;
    }
    if (operation==M2_ROOM_LEAVE) {
        if (session->room.id==leaving) m2_room_clear(session,0);
        return;
    }
    if (operation==M2_ROOM_CREATE || operation==M2_ROOM_JOIN) {
        M2RoomView view;
        if (m2_room_decode(session,response,length,&view)) {
            event->data=m2_room_join_reply(session,&view);
            if (event->data) {
                session->room=view.room;
                m2_room_publish(session);
            }
        }
    } else event->data=m2_room_list_reply(session->world,operation==M2_ROOM_SEARCH,response,length);
    if (!event->data) event->error=M2_SERVER_NOT_AVAILABLE;
}

static M2RoomJob *m2_room_job(uint8_t operation,uint32_t world,const void *body,size_t size) {
    M2RoomJob *job=calloc(1,sizeof *job);
    if (!job) return NULL;
    job->world=world;
    job->request[0]=operation;
    job->size=M2_ROOM_HEADER+size;
    memcpy(job->request+M2_ROOM_HEADER,body,size);
    return job;
}

static int m2_room_read_bin(const uint8_t *source,uint16_t id,M2RoomBin *bin) {
    const uint64_t count=m2_room_u64(source+8);
    const uint8_t *attr=m2_room_pointer(source);
    if (!count) return 1;
    if (count!=1 || !attr) return 0;
    memcpy(bin,attr,sizeof *bin);
    return bin->id==id && bin->size<=M2_ROOM_ATTRIBUTE && (!bin->size || bin->data);
}

static ABI int32_t m2_room_create(uint16_t ctx,const uint8_t *req,const M2Opt *opt,uint32_t *id) {
    if (!req) return NP_INVALID_ARGUMENT;
    const uint64_t count=m2_room_u64(req+0x78);
    const uint8_t *ints=m2_room_pointer(req+0x70);
    M2RoomBin tag={0}, listing={0}, internal={0};
    if (count>8 || (count && !ints) || !m2_room_read_bin(req+0x80,0x54,&tag) ||
        !m2_room_read_bin(req+0x90,0x55,&listing) || !m2_room_read_bin(req+0x60,0x57,&internal) ||
        internal.size!=listing.size || (listing.size && memcmp(internal.data,listing.data,(size_t)listing.size)))
        return NP_INVALID_ARGUMENT;
    const uint8_t *signaling=m2_room_pointer(req+0xb0);
    if (signaling && (signaling[0]>2 || signaling[1] || m2_room_u16(signaling+2)))
        return NP_INVALID_ARGUMENT;
    uint8_t body[19+8*6+2*M2_ROOM_ATTRIBUTE+4]={0};
    memcpy(body+8,req,2);
    memcpy(body+10,req+8,4);
    body[14]=(uint8_t)count;
    const uint16_t tag_size=(uint16_t)tag.size, list_size=(uint16_t)listing.size;
    memcpy(body+15,&tag_size,2);
    memcpy(body+17,&list_size,2);
    size_t offset=19;
    for (uint64_t i=0;i<count;++i,offset+=6) {
        memcpy(body+offset,ints+i*8,2);
        memcpy(body+offset+2,ints+i*8+4,4);
    }
    if (tag_size) memcpy(body+offset,tag.data,tag_size);
    if (list_size) memcpy(body+offset+tag_size,listing.data,list_size);
    size_t size=offset+tag_size+list_size;
    if (signaling && signaling[0]) { memcpy(body+size,signaling,4); size+=4; }
    M2RoomJob *job=m2_room_job(M2_ROOM_CREATE,m2_room_u32(req+12),body,size);
    return job ? m2_answer("CreateJoinRoomA",ctx,opt,id,0x7101,job,0) : M2_OUT_OF_MEMORY;
}

static ABI int32_t m2_room_search(uint16_t ctx,const uint8_t *req,const M2Opt *opt,uint32_t *id) {
    if (!req) return NP_INVALID_ARGUMENT;
    const uint64_t count=m2_room_u64(req+0x28), bins=m2_room_u64(req+0x38), attrs=m2_room_u64(req+0x48);
    const uint8_t *ints=m2_room_pointer(req+0x20), *binary=m2_room_pointer(req+0x30), *wanted=m2_room_pointer(req+0x40);
    if (count>8 || (count && !ints) || bins>1 || (bins && (!binary || binary[0]!=1)) ||
        attrs>1 || (attrs && (!wanted || m2_room_u16(wanted)!=0x55))) return NP_INVALID_ARGUMENT;
    M2RoomBin tag={0};
    if (bins) {
        memcpy(&tag,binary+8,sizeof tag);
        if (tag.id!=0x54 || !tag.size || tag.size>M2_ROOM_ATTRIBUTE || !tag.data) return NP_INVALID_ARGUMENT;
    }
    uint8_t body[20+8*8+M2_ROOM_ATTRIBUTE]={0};
    memcpy(body,req+0x10,16);
    body[16]=(uint8_t)count;
    body[17]=(uint8_t)bins;
    const uint16_t tag_size=(uint16_t)tag.size;
    memcpy(body+18,&tag_size,2);
    size_t offset=20;
    for (uint64_t i=0;i<count;++i,offset+=8) {
        const uint8_t *row=ints+i*16;
        memcpy(body+offset,row+8,2);
        body[offset+2]=row[0];
        memcpy(body+offset+4,row+12,4);
    }
    if (tag_size) memcpy(body+offset,tag.data,tag_size);
    M2RoomJob *job=m2_room_job(M2_ROOM_SEARCH,m2_room_u32(req+4),body,offset+tag_size);
    return job ? m2_answer("SearchRoom",ctx,opt,id,0x7106,job,0) : M2_OUT_OF_MEMORY;
}

static int32_t m2_room_by_id(const char *name,uint16_t ctx,const uint8_t *req,const M2Opt *opt,uint32_t *id,
                             uint8_t operation,uint16_t event) {
    if (!req || !m2_room_u64(req)) return NP_INVALID_ARGUMENT;
    M2RoomJob *job=m2_room_job(operation,0,req,8);
    return job ? m2_answer(name,ctx,opt,id,event,job,0) : M2_OUT_OF_MEMORY;
}

static ABI int32_t m2_room_join(uint16_t ctx,const uint8_t *req,const M2Opt *opt,uint32_t *id) {
    return m2_room_by_id("JoinRoomA",ctx,req,opt,id,M2_ROOM_JOIN,0x7102);
}

static ABI int32_t m2_room_leave(uint16_t ctx,const uint8_t *req,const M2Opt *opt,uint32_t *id) {
    return m2_room_by_id("LeaveRoom",ctx,req,opt,id,M2_ROOM_LEAVE,0x0103);
}

static ABI int32_t m2_room_external(uint16_t ctx,const uint8_t *req,const M2Opt *opt,uint32_t *id) {
    if (!req || m2_room_u64(req+8)!=1 || m2_room_u64(req+24)!=1) return NP_INVALID_ARGUMENT;
    const uint8_t *room=m2_room_pointer(req), *wanted=m2_room_pointer(req+16);
    if (!wanted || m2_room_u16(wanted)!=0x55) return NP_INVALID_ARGUMENT;
    return m2_room_by_id("GetRoomDataExternalList",ctx,room,opt,id,M2_ROOM_EXTERNAL,0x7005);
}

enum { M2_MSG_SEND=1, M2_MSG_POLL=2, M2_MSG_HEADER=36, M2_MSG_BATCH=8, M2_MSG_LIMIT=1024 };
typedef struct { size_t size; uint8_t body[GOLF_ROOM_MESSAGE_REQUEST_MAX]; } M2RoomMessageJob;

static void m2_room_message_header(const M2LocalSession *session,uint8_t *request,uint8_t operation,uint64_t cursor) {
    request[0]=session->world;
    request[1]=operation;
    memcpy(request+4,session->room_message_relay,16);
    memcpy(request+20,&session->room.id,8);
    memcpy(request+28,&cursor,8);
}

static int m2_room_message_transport(M2LocalSession *session) {
    const uint8_t empty[16]={0};
    if (memcmp(session->room_message_relay,empty,16)) return 1;
    uint8_t request[GOLF_TRANSPORT_REQUEST_BYTES]={session->world};
    uint8_t reply[GOLF_TRANSPORT_RESPONSE_BYTES];
    size_t length=0;
    memcpy(request+4,session->roster.token,16);
    memcpy(request+20,&session->room.id,8);
    if (runtime_golf_transport_exchange(request,reply,&length)!=200 || length!=sizeof reply ||
        m2_room_u64(reply)!=session->room.id || !memcmp(reply+8,empty,16)) return 0;
    memcpy(session->room_message_relay,reply+8,16);
    return 1;
}

static int m2_room_message_deliver(const M2LocalSession *session,uint16_t sender,const uint8_t *message,
                                   uint16_t size,uint64_t registration) {
    pthread_mutex_lock(&m2_lock);
    const int current=m2_ctx==session->ctx && m2_generation==session->generation &&
                      m2_room_message_registration==registration;
    const M2RoomMessageCb cb=current ? m2_room_message_cb : NULL;
    void *arg=m2_room_message_arg;
    if (cb) m2_room_message_delivery=++m2_room_delivery_sequence;
    pthread_mutex_unlock(&m2_lock);
    if (!cb) return 0;
    /* Golf's 0x9102 callback reads the message pointer at +0x38 and length at +0x40. */
    uint8_t data[0x48]={0};
    const uint32_t length=size;
    memcpy(data+0x38,&message,8);
    memcpy(data+0x40,&length,4);
    m2_inside_room_cb=1;
    cb(session->ctx,session->room.id,sender,0x9102,data,arg);
    m2_inside_room_cb=0;
    pthread_mutex_lock(&m2_lock);
    m2_room_message_delivery=0;
    pthread_cond_broadcast(&m2_room_delivery_done);
    pthread_mutex_unlock(&m2_lock);
    return 1;
}

static void m2_room_messages_poll(M2LocalSession *session) {
    if (!session->room.id) return;
    pthread_mutex_lock(&m2_lock);
    const int active=m2_room_message_cb && m2_ctx==session->ctx && m2_generation==session->generation;
    const uint64_t registration=m2_room_message_registration;
    pthread_mutex_unlock(&m2_lock);
    if (!active || !m2_room_message_transport(session)) return;
    uint8_t request[M2_MSG_HEADER]={0}, reply[GOLF_ROOM_MESSAGE_RESPONSE_MAX];
    size_t length=0, offset=2;
    m2_room_message_header(session,request,M2_MSG_POLL,session->room_message_ack);
    const int status=runtime_golf_room_message_exchange(request,sizeof request,reply,&length);
    if (status!=200 || length<2) return;
    const uint16_t count=m2_room_u16(reply);
    if (count>M2_MSG_BATCH) return;
    const uint8_t *rows[M2_MSG_BATCH];
    uint64_t previous=session->room_message_ack;
    for (uint16_t i=0;i<count;++i) {
        if (length-offset<14) return;
        const uint8_t *row=rows[i]=reply+offset;
        const uint64_t sequence=m2_room_u64(row);
        const uint16_t sender=m2_room_u16(row+8), size=m2_room_u16(row+12);
        if (sequence<=previous || !sender || sender==session->roster.own_id || row[10]<1 || row[10]>3 ||
            row[11] || !size || size>M2_MSG_LIMIT || length-offset-14<size) return;
        previous=sequence;
        offset+=14+size;
    }
    if (offset!=length) return;
    for (uint16_t i=0;i<count;++i) {
        const uint8_t *row=rows[i];
        const uint16_t sender=m2_room_u16(row+8);
        if (!m2_room_find(&session->room,sender) ||
            !m2_room_message_deliver(session,sender,row+14,m2_room_u16(row+12),registration)) break;
        session->room_message_ack=m2_room_u64(row);
    }
}

static void m2_room_message_complete(M2LocalSession *session,M2Event *event) {
    M2RoomMessageJob *job=event->data;
    int status=404;
    if (session->world && session->generation==event->generation && session->room.id==event->lobby &&
        m2_room_message_transport(session)) {
        uint8_t reply[GOLF_ROOM_MESSAGE_RESPONSE_MAX];
        size_t length=0;
        m2_room_message_header(session,job->body,M2_MSG_SEND,event->req);
        status=runtime_golf_room_message_exchange(job->body,job->size,reply,&length);
        if (!status) status=runtime_golf_room_message_exchange(job->body,job->size,reply,&length);
        if (status==200 && length) status=0;
    }
    free(job);
    event->data=NULL;
    if (status!=200)
        event->error=status==404 ? (int32_t)0x80550c30u : status==400 ? NP_INVALID_ARGUMENT :
                     status==409 || status==429 ? M2_BUSY : M2_SERVER_NOT_AVAILABLE;
}

static ABI int32_t m2_room_message_send(uint16_t ctx,const uint8_t *request,const M2Opt *opt,uint32_t *id) {
    if (!request || !id) return NP_INVALID_ARGUMENT;
    const uint64_t room=m2_room_u64(request);
    const uint8_t cast=request[8];
    const uint8_t *message=m2_room_pointer(request+0x20);
    const uint32_t size=m2_room_u32(request+0x28);
    if (!room || cast<1 || cast>3 || !message || !size || size>M2_MSG_LIMIT || m2_room_u32(request+0x2c))
        return NP_INVALID_ARGUMENT;
    uint64_t count=0;
    const uint8_t *targets=request+0x10;
    if (cast==2) count=1;
    else if (cast==3) {
        count=m2_room_u64(request+0x18);
        targets=m2_room_pointer(request+0x10);
        if (!count || count>=M2_LOCAL_CAPACITY || !targets) return NP_INVALID_ARGUMENT;
    }
    const uint16_t length=(uint16_t)size;
    M2RoomMessageJob *job=calloc(1,sizeof *job);
    if (!job) return M2_OUT_OF_MEMORY;
    job->size=M2_MSG_HEADER+4+2*(size_t)count+size;
    uint8_t *body=job->body+M2_MSG_HEADER;
    body[0]=cast;
    body[1]=(uint8_t)count;
    memcpy(body+2,&length,2);
    memcpy(body+4,targets,2*(size_t)count);
    memcpy(body+4+2*(size_t)count,message,size);
    return m2_answer("SendRoomMessage",ctx,opt,id,0x0108,job,room);
}

static const RuntimeExport m2_room_exports[]={
    {"sceNpMatching2SendRoomMessage",m2_room_message_send},
    {"sceNpMatching2CreateJoinRoomA",m2_room_create}, {"sceNpMatching2JoinRoomA",m2_room_join},
    {"sceNpMatching2LeaveRoom",m2_room_leave}, {"sceNpMatching2SearchRoom",m2_room_search},
    {"sceNpMatching2GetRoomDataExternalList",m2_room_external},
    {"sceNpMatching2SignalingGetConnectionStatus",m2_room_connection_status},
};

static ABI int32_t m2_world_info_list(uint16_t ctx,const void *request,const M2Opt *opt,uint32_t *id) {
    if (!request) return NP_INVALID_ARGUMENT;
    M2WorldResult *reply=calloc(1,sizeof *reply);
    if (!reply) return M2_OUT_OF_MEMORY;
    const uint32_t lobbies=1;
    for (uint32_t i=0;i<M2_WORLDS;++i) {
        const uint32_t world=i+1;
        void *next=i+1<M2_WORLDS ? reply->records[i+1] : NULL;
        memcpy(reply->records[i],&next,8);
        memcpy(reply->records[i]+8,&world,4);
        memcpy(reply->records[i]+0xc,&lobbies,4);
    }
    reply->rows=reply->records;
    reply->count=M2_WORLDS;
    return m2_answer("GetWorldInfoList",ctx,opt,id,0x2,reply,0);
}
static ABI int32_t m2_lobby_info_list(uint16_t ctx,const void *request,const M2Opt *opt,uint32_t *id) {
    if (!request) return NP_INVALID_ARGUMENT;
    M2LobbyResult *reply=calloc(1,sizeof *reply);
    if (!reply) return M2_OUT_OF_MEMORY;
    uint32_t world=1;
    if (local_lobbies) memcpy(&world,request,4);
    if (!world || world>M2_WORLDS) {
        free(reply);
        return NP_INVALID_ARGUMENT;
    }
    const uint64_t lobby=world;
    memcpy(reply->record+0x10,&lobby,8);
    reply->rows=reply->record;
    reply->count=1;
    return m2_answer("GetLobbyInfoList",ctx,opt,id,0x6,reply,0);
}
/* GOLF_LOCAL_WORLD=N (1..50): every lobby the game joins maps to the server's world N, so two
 * instances whose Auto Entrance picks different worlds still share one roster. The game keeps its own
 * lobby id in the replies. */
static uint64_t m2_host_lobby(uint64_t lobby) {
    const char *text=getenv("GOLF_LOCAL_WORLD");
    const long world=text && *text ? strtol(text,NULL,10) : 0;
    return lobby && world>=1 && world<=M2_WORLDS ? (uint64_t)world : lobby;
}
static ABI int32_t m2_join_lobby(uint16_t ctx,const uint64_t *request,const M2Opt *opt,uint32_t *id) {
    if (!request) return NP_INVALID_ARGUMENT;
    if (local_lobbies && (!*request || *request>M2_WORLDS)) return M2_INVALID_LOBBY;
    M2JoinResult *reply=calloc(1,sizeof *reply);
    if (!reply) return M2_OUT_OF_MEMORY;
    const uint64_t lobby=*request, count=1;
    void *ids=reply->member_ids;
    reply->info=reply->record;
    reply->member_ids[0]=1;
    memcpy(reply->record+0x10,&lobby,8);
    memcpy(reply->record+0x18,&ids,8);
    memcpy(reply->record+0x20,&count,8);
    memcpy(reply->record+0x28,reply->member_ids,2);
    return m2_answer("JoinLobby",ctx,opt,id,0x201,reply,m2_host_lobby(lobby));
}
static ABI int32_t m2_lobby_members(uint16_t ctx,const void *request,const M2Opt *opt,uint32_t *id) {
    if (!request) return NP_INVALID_ARGUMENT;
    M2MemberResult *reply=calloc(1,sizeof *reply);
    if (!reply) return M2_OUT_OF_MEMORY;
    uint64_t lobby=0;
    if (local_lobbies) memcpy(&lobby,request,8);
    else {
        M2LocalMember member={.id=1};
        snprintf(member.name,sizeof member.name,"%s",online_id);
        m2_lobby_record(reply->records[0],&member,NULL,0);
        reply->first=reply->records;
        reply->count=1;
    }
    return m2_answer("GetLobbyMemberDataInternalList",ctx,opt,id,0x7207,reply,m2_host_lobby(lobby));
}
static ABI int32_t m2_leave_lobby(uint16_t ctx,const void *request,const M2Opt *opt,uint32_t *id) {
    uint64_t lobby=0;
    if (local_lobbies && request) memcpy(&lobby,request,8);
    return m2_answer("LeaveLobby",ctx,opt,id,0x202,NULL,m2_host_lobby(lobby));
}
/* ---- NpWordFilter, answered locally (same condition as Matching2) ----
 * Sony's word filter server is gone. The Open Course entry controller (C: eboot 0xa5ada0) censors the
 * player's comment before any lobby code runs (0xece6f0): CreateTitleCtxA(user) must return >= 0
 * (0xa5aae2), CreateAsyncRequest(titleCtx, param) > 0 (0xece748), CensorComment(req, comment, option)
 * >= 0, then PollAsync(req, int *result) returns 0 (done; > 0 still running) with *result 0, which the
 * game takes as "comment accepted" (0xa5b412). The comment is left as written. */
static uint32_t wf_next_request=1;
static ABI int32_t wf_create_title_ctx(int32_t user,const void *a,const void *b) { (void)user; (void)a; (void)b; return 1; }
static ABI int32_t wf_create_request(int32_t title_ctx,const void *param) {
    (void)title_ctx; (void)param;
    return (int32_t)(__atomic_fetch_add(&wf_next_request,1,__ATOMIC_RELAXED) & 0x7fffffff);
}
static ABI int32_t wf_censor(int32_t request,const char *comment,const void *option) {
    (void)option;
    printf("Runtime: NpWordFilter request %d: comment of %zu bytes accepted locally\n",(int)request,comment ? strlen(comment) : 0);
    return 0;
}
static ABI int32_t wf_poll(int32_t request,int32_t *result) {
    (void)request;
    if (result) *result=0;
    return 0;
}
static ABI int32_t wf_ok(int32_t id) { (void)id; return 0; }

static const RuntimeExport matching_exports[]={
    {"sceNpWordFilterCreateTitleCtxA",wf_create_title_ctx},
    {"sceNpWordFilterCreateAsyncRequest",wf_create_request},
    {"sceNpWordFilterCensorComment",wf_censor},
    {"sceNpWordFilterPollAsync",wf_poll},
    {"sceNpWordFilterDeleteRequest",wf_ok},
    {"sceNpWordFilterAbortRequest",wf_ok},
    {"sceNpWordFilterDeleteTitleCtx",wf_ok},
    {"sceNpMatching2CreateContextA",m2_create_context_a},
    {"sceNpMatching2RegisterContextCallback",m2_register_context_cb},
    {"sceNpMatching2RegisterLobbyEventCallback",m2_register_lobby_cb},
    {"sceNpMatching2RegisterLobbyMessageCallback",m2_register_cb},
    {"sceNpMatching2RegisterRoomEventCallback",m2_register_room_cb},
    {"sceNpMatching2RegisterRoomMessageCallback",m2_register_room_message_cb},
    {"sceNpMatching2RegisterSignalingCallback",m2_register_room_signal_cb},
    {"sceNpMatching2ContextStart",m2_context_start},
    {"sceNpMatching2ContextStop",m2_context_stop},
    {"sceNpMatching2DestroyContext",m2_destroy_context},
    {"sceNpMatching2GetServerId",m2_server_id},
    {"sceNpMatching2SetDefaultRequestOptParam",m2_set_default_opt},
    {"sceNpMatching2GetWorldInfoList",m2_world_info_list},
    {"sceNpMatching2GetLobbyInfoList",m2_lobby_info_list},
    {"sceNpMatching2JoinLobby",m2_join_lobby},
    {"sceNpMatching2GetLobbyMemberDataInternalList",m2_lobby_members},
    {"sceNpMatching2LeaveLobby",m2_leave_lobby},
    {"sceNpSignalingCreateContextA",m2_signaling_context_a},
};

static const RuntimeExport auth_exports[]={
    {"sceNpAuthCreateAsyncRequest",local_auth_create},
    {"sceNpAuthGetAuthorizationCodeA",local_auth_code},
    {"sceNpAuthPollAsync",local_auth_poll},
    {"sceNpAuthAbortRequest",local_auth_abort},
    {"sceNpAuthDeleteRequest",local_auth_delete},
};

static const RuntimeExport exports[]={
    {"sceNpGetState",np_state}, {"sceNpGetOnlineId",np_online_id}, {"sceNpGetNpId",np_id},
    {"sceNpGetAccountCountryA",np_country},
    {"sceNpWebApiCreateContextA",webapi_context}, {"sceNpWebApiDeleteContext",ok},
    {"sceNpWebApiCreateRequest",webapi_create}, {"sceNpWebApiSendRequest2",webapi_send2},
    {"sceNpWebApiSendRequest",webapi_send}, {"sceNpWebApiGetHttpStatusCode",webapi_status},
    {"sceNpWebApiReadData",webapi_read}, {"sceNpWebApiDeleteRequest",webapi_delete},
    {"sceNpWebApiAbortRequest",ok},
};
/* Private P2P sockets for Golf's four RUDP channels. They identify a
 * virtual port only. The localhost relay handles all packet I/O. */
enum { GOLF_P2P_SOCKET_COUNT=4, GOLF_P2P_FIRST_PORT=25000, GOLF_P2P_PHYSICAL_PORT=3658 };
#define GOLF_NET_EBADF ((int32_t)0x80410109)
#define GOLF_NET_EINVAL ((int32_t)0x80410116)
#define GOLF_NET_EMFILE ((int32_t)0x80410118)
#define GOLF_NET_EADDRINUSE ((int32_t)0x80410130)
#define GOLF_NET_ENETUNREACH ((int32_t)0x80410133)

typedef struct { int32_t id; uint16_t port; int stopped; } GolfPeerSocket;
static GolfPeerSocket golf_peer_sockets[GOLF_P2P_SOCKET_COUNT];
static pthread_mutex_t golf_peer_socket_lock=PTHREAD_MUTEX_INITIALIZER;
static uint32_t golf_peer_next_socket=0x20000000;

/* Caller holds golf_peer_socket_lock. */
static GolfPeerSocket *golf_peer_find_socket(int32_t id) {
    for (size_t i=0;i<GOLF_P2P_SOCKET_COUNT;++i) {
        if (id && golf_peer_sockets[i].id==id) return &golf_peer_sockets[i];
    }
    return NULL;
}

static ABI int32_t golf_peer_socket(const char *name,int32_t family,int32_t type,int32_t protocol) {
    (void)name;
    /* sceNetSocket(name, AF_INET, SOCK_DGRAM_P2P, 0) at 0xa7494f. */
    if (family!=2 || type!=6 || protocol) return GOLF_NET_ENETUNREACH;
    int32_t result=GOLF_NET_EMFILE;
    pthread_mutex_lock(&golf_peer_socket_lock);
    for (size_t i=0;i<GOLF_P2P_SOCKET_COUNT;++i) {
        GolfPeerSocket *socket=&golf_peer_sockets[i];
        if (!socket->id) {
            result=(int32_t)golf_peer_next_socket++;
            if (golf_peer_next_socket==0x40000000) golf_peer_next_socket=0x20000000;
            *socket=(GolfPeerSocket){.id=result};
            break;
        }
    }
    pthread_mutex_unlock(&golf_peer_socket_lock);
    return result;
}

static ABI int32_t golf_peer_bind(int32_t id,const uint8_t *address,uint32_t size) {
    /* sceNetBind at 0xa749a7 uses a 16-byte sockaddr with network-order ports. */
    if (!address || size!=16 || address[0]!=16 || address[1]!=2) return GOLF_NET_EINVAL;
    const uint16_t physical=(uint16_t)(address[2]<<8 | address[3]);
    const uint16_t port=(uint16_t)(address[8]<<8 | address[9]);
    const uint8_t wildcard[4]={0};
    if (physical!=GOLF_P2P_PHYSICAL_PORT || memcmp(address+4,wildcard,4) ||
        port<GOLF_P2P_FIRST_PORT || port>=GOLF_P2P_FIRST_PORT+GOLF_P2P_SOCKET_COUNT) return GOLF_NET_EINVAL;
    pthread_mutex_lock(&golf_peer_socket_lock);
    GolfPeerSocket *socket=golf_peer_find_socket(id);
    int32_t result=GOLF_NET_EBADF;
    if (socket) {
        result=GOLF_NET_EINVAL;
        if (!socket->port && !socket->stopped) {
            result=0;
            for (size_t i=0;i<GOLF_P2P_SOCKET_COUNT;++i) {
                if (golf_peer_sockets[i].port==port) result=GOLF_NET_EADDRINUSE;
            }
            if (!result) socket->port=port;
        }
    }
    pthread_mutex_unlock(&golf_peer_socket_lock);
    return result;
}

uint16_t runtime_golf_peer_socket_port(int32_t id) {
    pthread_mutex_lock(&golf_peer_socket_lock);
    const GolfPeerSocket *socket=golf_peer_find_socket(id);
    const uint16_t port=socket && !socket->stopped ? socket->port : 0;
    pthread_mutex_unlock(&golf_peer_socket_lock);
    return port;
}

static ABI int32_t golf_peer_socket_abort(int32_t id,int32_t flags) {
    /* Golf uses abort flags 0 and shutdown mode 2 before close. */
    if (flags) return GOLF_NET_EINVAL;
    pthread_mutex_lock(&golf_peer_socket_lock);
    GolfPeerSocket *socket=golf_peer_find_socket(id);
    if (socket) socket->stopped=1;
    pthread_mutex_unlock(&golf_peer_socket_lock);
    return socket ? 0 : GOLF_NET_EBADF;
}

static ABI int32_t golf_peer_shutdown(int32_t id,int32_t mode) {
    return mode==2 ? golf_peer_socket_abort(id,0) : GOLF_NET_EINVAL;
}

static ABI int32_t golf_peer_socket_close(int32_t id) {
    pthread_mutex_lock(&golf_peer_socket_lock);
    GolfPeerSocket *socket=golf_peer_find_socket(id);
    if (socket) *socket=(GolfPeerSocket){0};
    pthread_mutex_unlock(&golf_peer_socket_lock);
    return socket ? 0 : GOLF_NET_EBADF;
}

static const RuntimeExport golf_peer_socket_exports[]={
    {"sceNetSocket",golf_peer_socket},
    {"sceNetBind",golf_peer_bind},
    {"sceNetSocketAbort",golf_peer_socket_abort},
    {"sceNetShutdown",golf_peer_shutdown},
    {"sceNetSocketClose",golf_peer_socket_close},
};

/* Title RUDP imports backed by the localhost channel and packet routes.
 * Guest threads edit queues and state. Only the transport worker does HTTP. */
enum {
    GOLF_RUDP_CONTEXTS=(M2_LOCAL_CAPACITY-1)*GOLF_P2P_SOCKET_COUNT,
    GOLF_RUDP_MAX_PAYLOAD=UINT16_MAX, GOLF_RUDP_QUEUE_BYTES=65536, GOLF_RUDP_QUEUE_PACKETS=64,
    GOLF_RUDP_BATCH=64, GOLF_RUDP_TICK_MS=20, GOLF_RUDP_PROBE_MS=100,
    GOLF_RUDP_REFRESH_MS=500, GOLF_RUDP_READY_MS=3000
};
enum {
    GOLF_RUDP_NOT_INITIALIZED=0x80770001u, GOLF_RUDP_ALREADY_INITIALIZED=0x80770002u,
    GOLF_RUDP_INVALID_CONTEXT=0x80770003u, GOLF_RUDP_INVALID_ARGUMENT=0x80770004u,
    GOLF_RUDP_TIMED_OUT=0x8077000bu, GOLF_RUDP_TOO_LARGE=0x80770012u,
    GOLF_RUDP_NOT_BOUND=0x80770013u, GOLF_RUDP_WRITE_RETRY=0x80770016u,
    GOLF_RUDP_READ_RETRY=0x80770018u, GOLF_RUDP_INVALID_SOCKET=0x80770019u,
    GOLF_RUDP_TOO_MANY_CONTEXTS=0x80770020u
};

typedef struct RudpPacket {
    struct RudpPacket *next;
    int32_t context;
    uint16_t peer, source, destination, length;
    uint64_t incarnation;
    uint8_t bytes[];
} RudpPacket;
typedef struct { RudpPacket *head, *tail; size_t count, bytes; } RudpQueue;
typedef ABI void (*GolfRudpHandler)(int32_t,int32_t,int32_t,void *);
typedef struct {
    int32_t id, socket;
    uint16_t port, member;
    int dead, established, writable_notified, close_pending;
    int32_t close_error;
    GolfRudpHandler handler;
    void *handler_arg;
} GolfRudpContext;
typedef struct {
    uint16_t member;
    uint8_t requested, ready, status_observed;
    uint64_t revision, binding_generation, next_query_ms, confirmed_ms, writable_after_ms, incarnation;
} GolfRudpPeer;
typedef struct {
    uint16_t member, source, destination, length;
    uint64_t sequence, incarnation;
    const uint8_t *data;
} GolfRudpIncoming;

static pthread_mutex_t golf_rudp_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t golf_rudp_wake=PTHREAD_COND_INITIALIZER;
static pthread_cond_t golf_rudp_callback_done=PTHREAD_COND_INITIALIZER;
/* Locked state, except the I/O worker's temporary exchange buffers. */
static GolfRudpContext golf_rudp_contexts[GOLF_RUDP_CONTEXTS];
static GolfRudpPeer golf_rudp_peers[M2_LOCAL_CAPACITY-1];
static RudpQueue golf_rudp_tx, golf_rudp_rx;
static uint8_t golf_rudp_world, golf_rudp_token[16], golf_rudp_relay[16];
static uint64_t golf_rudp_scope, golf_rudp_scope_epoch;
static uint64_t golf_rudp_epoch, golf_rudp_revision, golf_rudp_upload, golf_rudp_ack;
static uint32_t golf_rudp_next_id;
static uint8_t golf_rudp_pending[GOLF_PEER_REQUEST_MAX];
static size_t golf_rudp_pending_size, golf_rudp_peer_cursor;
static int golf_rudp_up, golf_rudp_io, golf_rudp_relay_ready;
static int32_t golf_rudp_callback_id;
static _Thread_local int golf_rudp_in_callback;

/* M0 diagnostics: bytes and packets per peer member, printed every 10 s and at sceRudpEnd/exit. */
typedef struct { uint16_t member; uint64_t tx_bytes, tx_packets, rx_bytes, rx_packets; } GolfRudpStats;
static GolfRudpStats golf_rudp_stats[64];
static uint64_t golf_rudp_stats_next_ms;
static GolfRudpStats *golf_rudp_stat(uint16_t member) {
    for (size_t i=0;i<64;++i) {
        if (golf_rudp_stats[i].member==member) return &golf_rudp_stats[i];
        if (!golf_rudp_stats[i].member) { golf_rudp_stats[i].member=member; return &golf_rudp_stats[i]; }
    }
    return NULL;
}
/* Caller holds golf_rudp_lock (or the process is exiting). */
static void golf_rudp_stats_print(const char *why) {
    for (size_t i=0;i<64 && golf_rudp_stats[i].member;++i) {
        const GolfRudpStats *s=&golf_rudp_stats[i];
        printf("Runtime: rudp peer %u (%s): sent %llu bytes in %llu packets, received %llu bytes in %llu packets\n",
               (unsigned)s->member,why,(unsigned long long)s->tx_bytes,(unsigned long long)s->tx_packets,
               (unsigned long long)s->rx_bytes,(unsigned long long)s->rx_packets);
    }
    fflush(stdout);
}
static void golf_rudp_stats_exit(void) { golf_rudp_stats_print("exit"); }
static uint64_t golf_rudp_now_ms(void) {
    struct timespec now={0};
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;
}

static int golf_rudp_queue_push(RudpQueue *queue,int32_t context,uint16_t member,uint16_t src,uint16_t dst,
                                const void *data,uint16_t length,uint64_t incarnation) {
    if (queue->count==GOLF_RUDP_QUEUE_PACKETS || length>GOLF_RUDP_QUEUE_BYTES-queue->bytes) return 0;
    RudpPacket *packet=malloc(sizeof *packet+length);
    if (!packet) return 0;
    *packet=(RudpPacket){.context=context,.peer=member,.source=src,.destination=dst,
                         .length=length,.incarnation=incarnation};
    memcpy(packet->bytes,data,length);
    if (queue->tail) queue->tail->next=packet;
    else queue->head=packet;
    queue->tail=packet;
    ++queue->count;
    queue->bytes+=length;
    return 1;
}

static void golf_rudp_queue_remove(RudpQueue *queue,RudpPacket *packet,RudpPacket *previous) {
    if (previous) previous->next=packet->next;
    else queue->head=packet->next;
    if (packet==queue->tail) queue->tail=previous;
    --queue->count;
    queue->bytes-=packet->length;
    free(packet);
}

static void golf_rudp_queue_clear(RudpQueue *queue,int32_t context) {
    RudpPacket *previous=NULL;
    for (RudpPacket *packet=queue->head;packet;) {
        RudpPacket *next=packet->next;
        if (!context || packet->context==context) golf_rudp_queue_remove(queue,packet,previous);
        else previous=packet;
        packet=next;
    }
}

static GolfRudpContext *golf_rudp_context(int32_t id) {
    for (size_t i=0;i<GOLF_RUDP_CONTEXTS;++i)
        if (id && golf_rudp_contexts[i].id==id) return &golf_rudp_contexts[i];
    return NULL;
}

static GolfRudpPeer *golf_rudp_peer(uint16_t member,int add) {
    GolfRudpPeer *free_slot=NULL;
    for (size_t i=0;i<M2_LOCAL_CAPACITY-1;++i) {
        GolfRudpPeer *peer=&golf_rudp_peers[i];
        if (peer->member==member) return peer;
        if (!peer->member && !free_slot) free_slot=peer;
    }
    if (add && free_slot) {
        free_slot->member=member;
        return free_slot;
    }
    return NULL;
}

static int golf_rudp_socket_live(const GolfRudpContext *context) {
    return context->port && runtime_golf_peer_socket_port(context->socket)==context->port;
}

static uint8_t golf_rudp_port_bit(uint16_t port) {
    return (uint8_t)(1u<<(port-GOLF_P2P_FIRST_PORT));
}

static int golf_rudp_ready(const GolfRudpContext *context) {
    if (!context->member || context->dead || !golf_rudp_socket_live(context)) return 0;
    GolfRudpPeer *peer=golf_rudp_peer(context->member,0);
    return peer && golf_rudp_now_ms()-peer->confirmed_ms<GOLF_RUDP_READY_MS &&
           (peer->ready & golf_rudp_port_bit(context->port));
}

static void golf_rudp_mark_dead(GolfRudpContext *context,int32_t error) {
    if (!context->member || context->dead) return;
    context->dead=1;
    context->close_error=error;
    context->close_pending=context->handler!=NULL;
    golf_rudp_queue_clear(&golf_rudp_rx,context->id);
    golf_rudp_queue_clear(&golf_rudp_tx,context->id);
}

/* The I/O worker is the only dispatcher. Teardown invalidates pending events
 * and waits for an already running callback before returning to the guest. */
static void golf_rudp_dispatch_events(void) {
    for (size_t i=0;i<GOLF_RUDP_CONTEXTS;++i) {
        GolfRudpContext *context=&golf_rudp_contexts[i];
        if (!context->id || !context->handler) continue;
        int32_t event=0, error=0;
        if (context->close_pending) {
            event=1;
            error=context->close_error;
            context->close_pending=0;
        } else if (!context->writable_notified && golf_rudp_ready(context)) {
            const GolfRudpPeer *peer=golf_rudp_peer(context->member,0);
            /* The title sets peer phase 2 after it has read state 4 for all
             * four channels. Give that tick time to publish phase 2. */
            if (peer->ready==0x0f && peer->status_observed==0x0f &&
                golf_rudp_now_ms()>=peer->writable_after_ms) {
                event=2;
                context->writable_notified=1;
            }
        }
        if (!event) continue;
        const GolfRudpHandler handler=context->handler;
        const int32_t id=context->id;
        void *arg=context->handler_arg;
        golf_rudp_callback_id=id;
        pthread_mutex_unlock(&golf_rudp_lock);
        golf_rudp_in_callback=1;
        handler(id,event,error,arg);
        golf_rudp_in_callback=0;
        pthread_mutex_lock(&golf_rudp_lock);
        golf_rudp_callback_id=0;
        pthread_cond_broadcast(&golf_rudp_callback_done);
    }
}

static void golf_rudp_wait_for_callback(int32_t id) {
    if (golf_rudp_in_callback) return;
    while (id && id==golf_rudp_callback_id) pthread_cond_wait(&golf_rudp_callback_done,&golf_rudp_lock);
}

static void golf_rudp_reset_transport(void) {
    ++golf_rudp_epoch;
    golf_rudp_upload=golf_rudp_ack=0;
    golf_rudp_pending_size=0;
    memset(golf_rudp_peers,0,sizeof golf_rudp_peers);
    golf_rudp_queue_clear(&golf_rudp_tx,0);
    golf_rudp_queue_clear(&golf_rudp_rx,0);
    for (size_t i=0;i<GOLF_RUDP_CONTEXTS;++i)
        golf_rudp_mark_dead(&golf_rudp_contexts[i],(int32_t)GOLF_RUDP_TIMED_OUT);
}

/* A different roster lease or room membership invalidates its transport. */
static void golf_rudp_refresh_lease(void) {
    pthread_mutex_lock(&m2_lock);
    const uint8_t world=m2_peer_roster.generation==m2_generation ? m2_peer_roster.world : 0;
    const M2LocalRoster roster=m2_peer_roster.roster;
    const uint64_t scope_epoch=m2_room_status_view.transport_epoch;
    const M2LocalRoom room=m2_room_status_view.generation==m2_generation && m2_room_status_view.world==world ?
        m2_room_status_view.room : (M2LocalRoom){0};
    pthread_mutex_unlock(&m2_lock);
    if (world!=golf_rudp_world || (world && memcmp(golf_rudp_token,roster.token,16)) ||
        golf_rudp_scope!=room.id || golf_rudp_scope_epoch!=scope_epoch) {
        golf_rudp_world=world;
        golf_rudp_scope=room.id;
        golf_rudp_scope_epoch=scope_epoch;
        memcpy(golf_rudp_token,roster.token,16);
        memset(golf_rudp_relay,0,sizeof golf_rudp_relay);
        golf_rudp_relay_ready=0;
        golf_rudp_reset_transport();
    }
    for (size_t i=0;i<M2_LOCAL_CAPACITY-1;++i) {
        GolfRudpPeer *peer=&golf_rudp_peers[i];
        if (!peer->member || (room.id ? m2_room_link(&room,roster.own_id,peer->member)
                                     : m2_roster_has(&roster,peer->member))) continue;
        for (size_t j=0;j<GOLF_RUDP_CONTEXTS;++j)
            if (golf_rudp_contexts[j].member==peer->member)
                golf_rudp_mark_dead(&golf_rudp_contexts[j],(int32_t)GOLF_RUDP_TIMED_OUT);
        *peer=(GolfRudpPeer){0};
    }
}

static int golf_rudp_transport_ready(void) {
    if (golf_rudp_relay_ready) return 1;
    const uint64_t epoch=golf_rudp_epoch, scope=golf_rudp_scope;
    uint8_t request[GOLF_TRANSPORT_REQUEST_BYTES]={golf_rudp_world};
    uint8_t response[GOLF_TRANSPORT_RESPONSE_BYTES];
    const uint8_t empty[16]={0};
    size_t length=0;
    memcpy(request+4,golf_rudp_token,16);
    memcpy(request+20,&scope,8);
    pthread_mutex_unlock(&golf_rudp_lock);
    const int status=runtime_golf_transport_exchange(request,response,&length);
    pthread_mutex_lock(&golf_rudp_lock);
    golf_rudp_refresh_lease();
    if (epoch!=golf_rudp_epoch || status!=200 || length!=sizeof response ||
        memcmp(response,&scope,8) || !memcmp(response+8,empty,16)) return 0;
    if (memcmp(golf_rudp_relay,empty,16) && memcmp(golf_rudp_relay,response+8,16))
        golf_rudp_reset_transport();
    memcpy(golf_rudp_relay,response+8,16);
    golf_rudp_relay_ready=1;
    return 1;
}

static void golf_rudp_confirm_mask(uint16_t member,uint8_t ready,int present) {
    for (size_t i=0;i<GOLF_RUDP_CONTEXTS;++i) {
        GolfRudpContext *context=&golf_rudp_contexts[i];
        if (context->member!=member || context->dead) continue;
        if (!present) golf_rudp_mark_dead(context,(int32_t)GOLF_RUDP_TIMED_OUT);
        else if (!golf_rudp_socket_live(context)) golf_rudp_mark_dead(context,0);
        else if (ready & golf_rudp_port_bit(context->port)) context->established=1;
        else if (context->established) golf_rudp_mark_dead(context,0);
    }
}

static uint8_t golf_rudp_peer_mask(uint16_t member) {
    uint8_t mask=0;
    for (size_t i=0;i<GOLF_RUDP_CONTEXTS;++i) {
        const GolfRudpContext *context=&golf_rudp_contexts[i];
        if (context->member==member && !context->dead && golf_rudp_socket_live(context))
            mask |= golf_rudp_port_bit(context->port);
    }
    return mask;
}

/* With the queue lock held, make one channel request and release the lock during HTTP. */
static void golf_rudp_update_channels(void) {
    const uint64_t now=golf_rudp_now_ms();
    for (size_t i=0;i<M2_LOCAL_CAPACITY-1;++i) {
        const size_t index=golf_rudp_peer_cursor++ % (M2_LOCAL_CAPACITY-1);
        GolfRudpPeer *peer=&golf_rudp_peers[index];
        if (!peer->member) continue;
        const uint8_t mask=golf_rudp_peer_mask(peer->member);
        if (mask!=peer->requested || !peer->revision) {
            peer->requested=mask;
            peer->ready&=mask;
            peer->status_observed&=mask;
            peer->revision=++golf_rudp_revision;
            peer->next_query_ms=0;
        }
        if (now<peer->next_query_ms) continue;
        const uint64_t epoch=golf_rudp_epoch, revision=peer->revision, bindings=peer->binding_generation;
        const uint16_t member=peer->member;
        uint8_t request[GOLF_CHANNEL_REQUEST_BYTES]={golf_rudp_world,mask};
        uint8_t response[GOLF_CHANNEL_RESPONSE_MAX];
        size_t length=0;
        memcpy(request+2,&member,2);
        memcpy(request+4,golf_rudp_relay,16);
        memcpy(request+20,&revision,8);
        pthread_mutex_unlock(&golf_rudp_lock);
        const int status=runtime_golf_channel_exchange(request,response,&length);
        pthread_mutex_lock(&golf_rudp_lock);
        golf_rudp_refresh_lease();
        if (epoch!=golf_rudp_epoch || member!=peer->member || revision!=peer->revision) return;
        if (bindings!=peer->binding_generation || mask!=golf_rudp_peer_mask(member)) {
            /* Local teardown or rebind happened during HTTP. Send the current
             * mask on the next tick and leave the replacement contexts alone. */
            peer->next_query_ms=0;
            return;
        }
        peer->next_query_ms=golf_rudp_now_ms()+GOLF_RUDP_PROBE_MS;
        const size_t expected=golf_rudp_scope ? GOLF_CHANNEL_RESPONSE_MAX : GOLF_CHANNEL_RESPONSE_BYTES;
        if (status==200 && length==expected && !memcmp(response,&revision,8) &&
            !(response[8]&~mask) && !response[9] && !memcmp(response+10,&member,2)) {
            if (golf_rudp_scope) {
                const uint64_t incarnation=m2_room_u64(response+12);
                if (!incarnation) return;
                if (peer->incarnation && peer->incarnation!=incarnation && mask) {
                    golf_rudp_confirm_mask(member,0,0);
                    peer->ready=0;
                    peer->next_query_ms=0;
                    return;
                }
                peer->incarnation=incarnation;
            }
            golf_rudp_confirm_mask(member,response[8],1);
            if (!mask) *peer=(GolfRudpPeer){0};
            else {
                peer->ready=response[8];
                peer->confirmed_ms=golf_rudp_now_ms();
                if (peer->ready==mask) peer->next_query_ms=peer->confirmed_ms+GOLF_RUDP_REFRESH_MS;
            }
        } else if (status==404) {
            golf_rudp_confirm_mask(member,0,0);
            peer->ready=0;
            golf_rudp_relay_ready=0;
        } else if (status==409) {
            peer->ready=0;
        }
        return;
    }
}

static int golf_rudp_parse_packets(const uint8_t *response,size_t length,uint64_t receive_ack,int scoped,
                                  GolfRudpIncoming incoming[GOLF_RUDP_BATCH],uint16_t *count) {
    if (length<12) return 0;
    uint16_t rows;
    memcpy(&rows,response+8,2);
    if (rows>GOLF_RUDP_BATCH || response[10] || response[11]) return 0;
    size_t offset=12;
    uint64_t previous=receive_ack;
    for (uint16_t i=0;i<rows;++i) {
        if (length-offset<(scoped ? 24u : 16u)) return 0;
        GolfRudpIncoming *packet=&incoming[i];
        memcpy(&packet->sequence,response+offset,8);
        offset+=8;
        packet->incarnation=0;
        if (scoped) {
            packet->incarnation=m2_room_u64(response+offset);
            if (!packet->incarnation) return 0;
            offset+=8;
        }
        memcpy(&packet->member,response+offset,2);
        memcpy(&packet->source,response+offset+2,2);
        memcpy(&packet->destination,response+offset+4,2);
        memcpy(&packet->length,response+offset+6,2);
        offset+=8;
        if (packet->sequence<=previous || !packet->member ||
            packet->source<GOLF_P2P_FIRST_PORT || packet->source>=GOLF_P2P_FIRST_PORT+GOLF_P2P_SOCKET_COUNT ||
            packet->destination!=(packet->source^1) || !packet->length ||
            length-offset<packet->length) return 0;
        packet->data=response+offset;
        offset+=packet->length;
        previous=packet->sequence;
    }
    *count=rows;
    return offset==length;
}

static void golf_rudp_receive(GolfRudpIncoming incoming[GOLF_RUDP_BATCH],uint16_t count) {
    for (uint16_t i=0;i<count;++i) {
        const GolfRudpIncoming *packet=&incoming[i];
        GolfRudpContext *context=NULL;
        for (size_t j=0;j<GOLF_RUDP_CONTEXTS;++j) {
            GolfRudpContext *candidate=&golf_rudp_contexts[j];
            if (candidate->id && !candidate->dead && candidate->member==packet->member &&
                candidate->port==packet->destination && golf_rudp_socket_live(candidate)) {
                context=candidate;
                break;
            }
        }
        if (context && golf_rudp_scope) {
            const GolfRudpPeer *peer=golf_rudp_peer(packet->member,0);
            if (!peer || !peer->incarnation) break;
            if (peer->incarnation!=packet->incarnation) context=NULL;
        }
        if (context && !golf_rudp_queue_push(&golf_rudp_rx,context->id,packet->member,packet->source,
                                             packet->destination,packet->data,packet->length,packet->incarnation)) break;
        golf_rudp_ack=packet->sequence;
    }
}

static void golf_rudp_prepare_upload(void) {
    if (golf_rudp_pending_size) return;
    uint16_t count=0, member=0;
    size_t length=GOLF_PEER_REQUEST_MIN;
    while (golf_rudp_tx.head && count<GOLF_RUDP_BATCH) {
        RudpPacket *packet=golf_rudp_tx.head;
        if (count && packet->peer!=member) break;
        const GolfRudpContext *context=golf_rudp_context(packet->context);
        const GolfRudpPeer *peer=golf_rudp_peer(packet->peer,0);
        if (context && !context->dead && golf_rudp_socket_live(context) && peer &&
            (!golf_rudp_scope || (packet->incarnation && packet->incarnation==peer->incarnation))) {
            member=packet->peer;
            ++count;
            if (golf_rudp_scope) {
                memcpy(golf_rudp_pending+length,&packet->incarnation,8);
                length+=8;
            }
            memcpy(golf_rudp_pending+length,&packet->peer,2);
            memcpy(golf_rudp_pending+length+2,&packet->source,2);
            memcpy(golf_rudp_pending+length+4,&packet->destination,2);
            memcpy(golf_rudp_pending+length+6,&packet->length,2);
            memcpy(golf_rudp_pending+length+8,packet->bytes,packet->length);
            length+=8+packet->length;
        }
        golf_rudp_queue_remove(&golf_rudp_tx,packet,NULL);
    }
    if (!count) return;
    golf_rudp_pending[0]=golf_rudp_world;
    golf_rudp_pending[1]=golf_rudp_scope!=0;
    memcpy(golf_rudp_pending+2,&count,2);
    memcpy(golf_rudp_pending+4,golf_rudp_relay,16);
    const uint64_t sequence=golf_rudp_upload+1;
    memcpy(golf_rudp_pending+20,&sequence,8);
    golf_rudp_pending_size=length;
}

static int golf_rudp_packets(uint8_t response[GOLF_PEER_RESPONSE_MAX],int poll_only) {
    if (!poll_only) golf_rudp_prepare_upload();
    const uint64_t epoch=golf_rudp_epoch, receive_ack=golf_rudp_ack;
    uint8_t request[GOLF_PEER_REQUEST_MAX];
    size_t length=0, request_size=GOLF_PEER_REQUEST_MIN;
    const int upload=!poll_only && golf_rudp_pending_size;
    if (upload) {
        request_size=golf_rudp_pending_size;
        memcpy(request,golf_rudp_pending,request_size);
    } else {
        memset(request,0,GOLF_PEER_REQUEST_MIN);
        request[0]=golf_rudp_world;
        request[1]=golf_rudp_scope!=0;
        memcpy(request+4,golf_rudp_relay,16);
    }
    memcpy(request+28,&receive_ack,8);
    pthread_mutex_unlock(&golf_rudp_lock);
    const int status=runtime_golf_peer_exchange(request,request_size,response,&length);
    pthread_mutex_lock(&golf_rudp_lock);
    golf_rudp_refresh_lease();
    if (epoch!=golf_rudp_epoch) return 0;
    if (status==200) {
        GolfRudpIncoming incoming[GOLF_RUDP_BATCH];
        uint16_t count=0;
        uint64_t accepted;
        if (!golf_rudp_parse_packets(response,length,receive_ack,golf_rudp_scope!=0,incoming,&count)) return upload;
        memcpy(&accepted,response,8);
        const uint64_t pending_sequence=golf_rudp_upload+(golf_rudp_pending_size!=0);
        if (accepted<golf_rudp_upload || accepted>pending_sequence || (upload && accepted!=pending_sequence))
            return upload;
        if (golf_rudp_pending_size && accepted==pending_sequence) {
            golf_rudp_pending_size=0;
            golf_rudp_upload=accepted;
        }
        golf_rudp_receive(incoming,count);
    } else if (upload && status==404) {
        /* The server acknowledges an accepted duplicate even after the recipient leaves. */
        golf_rudp_pending_size=0;
    }
    if (status==404) golf_rudp_relay_ready=0;
    /* An empty exchange on the next tick can read and ACK while uploads back up. */
    return upload && status!=200;
}

static int golf_rudp_has_work(void) {
    if (golf_rudp_pending_size || golf_rudp_tx.head) return 1;
    for (size_t i=0;i<M2_LOCAL_CAPACITY-1;++i)
        if (golf_rudp_peers[i].member) return 1;
    for (size_t i=0;i<GOLF_RUDP_CONTEXTS;++i)
        if (golf_rudp_contexts[i].close_pending) return 1;
    return 0;
}

static void *golf_rudp_worker(void *unused) {
    (void)unused;
    runtime_thread_attach_host("NpRudp");
    int poll_only=0;
    uint8_t response[GOLF_PEER_RESPONSE_MAX];
    pthread_mutex_lock(&golf_rudp_lock);
    for (;;) {
        while (!golf_rudp_has_work()) pthread_cond_wait(&golf_rudp_wake,&golf_rudp_lock);
        golf_rudp_refresh_lease();
        if (golf_rudp_world && golf_rudp_has_work() && golf_rudp_transport_ready()) golf_rudp_update_channels();
        golf_rudp_dispatch_events();
        if (golf_rudp_stats[0].member) {
            const uint64_t now_ms=golf_rudp_now_ms();
            if (!golf_rudp_stats_next_ms) golf_rudp_stats_next_ms=now_ms+10000;
            else if (now_ms>=golf_rudp_stats_next_ms) { golf_rudp_stats_next_ms=now_ms+10000; golf_rudp_stats_print("10 s"); }
        }
        if (golf_rudp_world && golf_rudp_has_work() && golf_rudp_relay_ready)
            poll_only=golf_rudp_packets(response,poll_only);
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME,&deadline);
        deadline.tv_nsec+=GOLF_RUDP_TICK_MS*1000000;
        if (deadline.tv_nsec>=1000000000) { deadline.tv_nsec-=1000000000; ++deadline.tv_sec; }
        pthread_cond_timedwait(&golf_rudp_wake,&golf_rudp_lock,&deadline);
    }
    return NULL;
}

static ABI int32_t golf_rudp_init(const void *pool,int32_t size) {
    if (!pool || size<=0) return (int32_t)GOLF_RUDP_INVALID_ARGUMENT;
    static int stats_registered;
    if (!stats_registered++) atexit(golf_rudp_stats_exit);
    pthread_mutex_lock(&golf_rudp_lock);
    const int32_t result=golf_rudp_up ? (int32_t)GOLF_RUDP_ALREADY_INITIALIZED : 0;
    golf_rudp_up=1;
    pthread_mutex_unlock(&golf_rudp_lock);
    return result;
}

static ABI int32_t golf_rudp_end(void) {
    pthread_mutex_lock(&golf_rudp_lock);
    golf_rudp_stats_print("sceRudpEnd");
    const int32_t result=golf_rudp_up ? 0 : (int32_t)GOLF_RUDP_NOT_INITIALIZED;
    golf_rudp_up=0;
    memset(golf_rudp_contexts,0,sizeof golf_rudp_contexts);
    for (size_t i=0;i<M2_LOCAL_CAPACITY-1;++i)
        if (golf_rudp_peers[i].member) ++golf_rudp_peers[i].binding_generation;
    golf_rudp_queue_clear(&golf_rudp_tx,0);
    golf_rudp_queue_clear(&golf_rudp_rx,0);
    pthread_cond_signal(&golf_rudp_wake);
    golf_rudp_wait_for_callback(golf_rudp_callback_id);
    pthread_mutex_unlock(&golf_rudp_lock);
    return result;
}

static ABI int32_t golf_rudp_start(uint32_t stack,uint32_t priority) {
    (void)stack; (void)priority;
    pthread_mutex_lock(&golf_rudp_lock);
    int32_t result=0;
    if (!golf_rudp_up) result=(int32_t)GOLF_RUDP_NOT_INITIALIZED;
    else if (!golf_rudp_io) {
        pthread_t thread;
        if (pthread_create(&thread,NULL,golf_rudp_worker,NULL)) result=(int32_t)0x80770007u;
        else { pthread_detach(thread); golf_rudp_io=1; }
    }
    pthread_mutex_unlock(&golf_rudp_lock);
    return result;
}

static ABI int32_t golf_rudp_set_handler(void *callback,void *arg) {
    (void)callback; (void)arg;
    pthread_mutex_lock(&golf_rudp_lock);
    const int32_t result=golf_rudp_up ? 0 : (int32_t)GOLF_RUDP_NOT_INITIALIZED;
    pthread_mutex_unlock(&golf_rudp_lock);
    return result;
}

static ABI int32_t golf_rudp_create(GolfRudpHandler handler,void *arg,int32_t *id) {
    pthread_mutex_lock(&golf_rudp_lock);
    int32_t result=(int32_t)GOLF_RUDP_NOT_INITIALIZED;
    if (golf_rudp_up && !id) result=(int32_t)GOLF_RUDP_INVALID_ARGUMENT;
    else if (golf_rudp_up) {
        result=(int32_t)GOLF_RUDP_TOO_MANY_CONTEXTS;
        for (size_t i=0;i<GOLF_RUDP_CONTEXTS;++i) {
            if (golf_rudp_contexts[i].id) continue;
            golf_rudp_next_id=golf_rudp_next_id==INT32_MAX ? 1 : golf_rudp_next_id+1;
            *id=(int32_t)golf_rudp_next_id;
            golf_rudp_contexts[i]=(GolfRudpContext){.id=*id,.handler=handler,.handler_arg=arg};
            result=0;
            break;
        }
    }
    pthread_mutex_unlock(&golf_rudp_lock);
    return result;
}

static int32_t golf_rudp_check(const GolfRudpContext *context) {
    if (!golf_rudp_up) return (int32_t)GOLF_RUDP_NOT_INITIALIZED;
    return context ? 0 : (int32_t)GOLF_RUDP_INVALID_CONTEXT;
}

static ABI int32_t golf_rudp_bind(int32_t id,int32_t socket,uint16_t vport,uint8_t mux) {
    pthread_mutex_lock(&golf_rudp_lock);
    GolfRudpContext *context=golf_rudp_context(id);
    int32_t result=golf_rudp_check(context);
    if (!result) {
        const uint16_t port=runtime_golf_peer_socket_port(socket);
        if (!port) result=(int32_t)GOLF_RUDP_INVALID_SOCKET;
        else if (vport!=0x14 || mux!=1 || context->port) result=(int32_t)GOLF_RUDP_INVALID_ARGUMENT;
        else { context->socket=socket; context->port=port; }
    }
    pthread_mutex_unlock(&golf_rudp_lock);
    return result;
}

static ABI int32_t golf_rudp_option(int32_t id,uint32_t option,const void *value,uint32_t length) {
    pthread_mutex_lock(&golf_rudp_lock);
    int32_t result=golf_rudp_check(golf_rudp_context(id));
    if (!result && (!value || length!=4 ||
        (option!=2 && option!=3 && option!=4 && option!=5 && option!=6 && option!=0x0b && option!=0x10)))
        result=(int32_t)GOLF_RUDP_INVALID_ARGUMENT;
    pthread_mutex_unlock(&golf_rudp_lock);
    return result;
}

static ABI int32_t golf_rudp_initiate(int32_t id,const uint8_t *address,uint32_t length,uint16_t vport) {
    pthread_mutex_lock(&golf_rudp_lock);
    GolfRudpContext *context=golf_rudp_context(id);
    int32_t result=golf_rudp_check(context);
    if (!result && !golf_rudp_socket_live(context)) result=(int32_t)GOLF_RUDP_NOT_BOUND;
    if (!result && (!address || length!=16 || address[0]!=8 || address[1]!=2 || vport ||
        address[2]!=0x0e || address[3]!=0x4a || address[4]!=127 || address[5] ||
        (uint16_t)(address[8]<<8 | address[9])!=(context->port^1) || context->member))
        result=(int32_t)GOLF_RUDP_INVALID_ARGUMENT;
    if (!result) {
        const uint16_t member=(uint16_t)(address[6]<<8 | address[7]);
        golf_rudp_refresh_lease();
        pthread_mutex_lock(&m2_lock);
        const int valid=m2_peer_roster_live() && m2_peer_roster.world==golf_rudp_world &&
            !memcmp(m2_peer_roster.roster.token,golf_rudp_token,16) &&
            m2_peer_roster.roster.own_id!=member &&
            (golf_rudp_scope ? (m2_room_status_view.generation==m2_generation &&
                m2_room_status_view.room.id==golf_rudp_scope && m2_room_link(&m2_room_status_view.room,m2_peer_roster.roster.own_id,member))
                : m2_roster_has(&m2_peer_roster.roster,member));
        pthread_mutex_unlock(&m2_lock);
        if (!valid || !golf_rudp_io) result=(int32_t)GOLF_RUDP_TIMED_OUT;
        else if (golf_rudp_peer_mask(member) & golf_rudp_port_bit(context->port))
            result=(int32_t)GOLF_RUDP_INVALID_ARGUMENT;
        else {
            GolfRudpPeer *peer=golf_rudp_peer(member,1);
            if (!peer) result=(int32_t)GOLF_RUDP_TOO_MANY_CONTEXTS;
            else {
                context->member=member;
                printf("Runtime: rudp context %d initiated: vport %u -> member %u\n",(int)id,(unsigned)context->port,(unsigned)member);
                ++peer->binding_generation;
                peer->ready&=(uint8_t)~golf_rudp_port_bit(context->port);
                peer->status_observed&=(uint8_t)~golf_rudp_port_bit(context->port);
                peer->next_query_ms=0;
                pthread_cond_signal(&golf_rudp_wake);
            }
        }
    }
    pthread_mutex_unlock(&golf_rudp_lock);
    return result;
}

static ABI int32_t golf_rudp_terminate(int32_t id) {
    pthread_mutex_lock(&golf_rudp_lock);
    GolfRudpContext *context=golf_rudp_context(id);
    const int32_t result=golf_rudp_check(context);
    if (!result) {
        GolfRudpPeer *peer=context->member ? golf_rudp_peer(context->member,0) : NULL;
        if (peer) ++peer->binding_generation;
        *context=(GolfRudpContext){0};
        golf_rudp_queue_clear(&golf_rudp_rx,id);
        golf_rudp_queue_clear(&golf_rudp_tx,id);
        pthread_cond_signal(&golf_rudp_wake);
    }
    golf_rudp_wait_for_callback(id);
    pthread_mutex_unlock(&golf_rudp_lock);
    return result;
}

static ABI int32_t golf_rudp_status(int32_t id,void *status,uint32_t size) {
    pthread_mutex_lock(&golf_rudp_lock);
    golf_rudp_refresh_lease();
    const GolfRudpContext *context=golf_rudp_context(id);
    int32_t result=golf_rudp_check(context);
    if (!result) {
        if (!status || size!=0x30) result=(int32_t)GOLF_RUDP_INVALID_ARGUMENT;
        else {
            const uint32_t state=golf_rudp_ready(context) ? 4 : context->dead ? 1 : context->member ? 2 : 0;
            if (state==4) {
                GolfRudpPeer *peer=golf_rudp_peer(context->member,0);
                const uint8_t bit=golf_rudp_port_bit(context->port);
                if (!(peer->status_observed & bit)) {
                    peer->status_observed|=bit;
                    if (peer->status_observed==0x0f)
                        peer->writable_after_ms=golf_rudp_now_ms()+GOLF_RUDP_TICK_MS;
                }
            }
            memset(status,0,size);
            memcpy(status,&state,4);
        }
    }
    pthread_mutex_unlock(&golf_rudp_lock);
    return result;
}

static ABI int32_t golf_rudp_write(int32_t id,const void *data,uint32_t length,uint8_t flags) {
    pthread_mutex_lock(&golf_rudp_lock);
    golf_rudp_refresh_lease();
    const GolfRudpContext *context=golf_rudp_context(id);
    int32_t result=golf_rudp_check(context);
    if (!result) {
        const GolfRudpPeer *peer=context->member ? golf_rudp_peer(context->member,0) : NULL;
        if (!data || !length || (flags&~0x48u)) result=(int32_t)GOLF_RUDP_INVALID_ARGUMENT;
        else if (length>GOLF_RUDP_MAX_PAYLOAD) result=(int32_t)GOLF_RUDP_TOO_LARGE;
        else if (!peer || !golf_rudp_ready(context) ||
                 !golf_rudp_queue_push(&golf_rudp_tx,id,context->member,context->port,context->port^1,data,
                                       (uint16_t)length,peer->incarnation))
            result=(int32_t)GOLF_RUDP_WRITE_RETRY; /* Golf explicitly retries this value at 0xa7f5ba. */
        else {
            pthread_cond_signal(&golf_rudp_wake); result=(int32_t)length;
            GolfRudpStats *stat=golf_rudp_stat(context->member);
            if (stat) { stat->tx_bytes+=length; ++stat->tx_packets; }
        }
    }
    pthread_mutex_unlock(&golf_rudp_lock);
    return result;
}

static int32_t golf_rudp_readable(int32_t id,int count) {
    pthread_mutex_lock(&golf_rudp_lock);
    golf_rudp_refresh_lease();
    int32_t result=golf_rudp_check(golf_rudp_context(id));
    if (!result) {
        for (const RudpPacket *packet=golf_rudp_rx.head;packet;packet=packet->next) {
            if (packet->context!=id) continue;
            if (!count) { result=packet->length; break; }
            ++result;
        }
    }
    pthread_mutex_unlock(&golf_rudp_lock);
    return result;
}

static ABI int32_t golf_rudp_count(int32_t id) { return golf_rudp_readable(id,1); }
static ABI int32_t golf_rudp_size(int32_t id) { return golf_rudp_readable(id,0); }

static ABI int32_t golf_rudp_read(int32_t id,void *data,uint32_t length,uint8_t flags,void *info) {
    (void)info; /* Golf uses only the returned byte count and payload at 0xa7fa15. */
    pthread_mutex_lock(&golf_rudp_lock);
    golf_rudp_refresh_lease();
    int32_t result=golf_rudp_check(golf_rudp_context(id));
    if (!result && (!data || !length || flags)) result=(int32_t)GOLF_RUDP_INVALID_ARGUMENT;
    if (!result) {
        RudpPacket *previous=NULL, *packet=golf_rudp_rx.head;
        while (packet && packet->context!=id) { previous=packet; packet=packet->next; }
        if (!packet) result=(int32_t)GOLF_RUDP_READ_RETRY;
        else if (length<packet->length) result=(int32_t)GOLF_RUDP_TOO_LARGE;
        else {
            memcpy(data,packet->bytes,packet->length);
            result=packet->length;
            GolfRudpStats *stat=golf_rudp_stat(packet->peer);
            if (stat) { stat->rx_bytes+=packet->length; ++stat->rx_packets; }
            golf_rudp_queue_remove(&golf_rudp_rx,packet,previous);
        }
    }
    pthread_mutex_unlock(&golf_rudp_lock);
    return result;
}

static const RuntimeExport golf_rudp_exports[]={
    {"sceRudpInit",golf_rudp_init}, {"sceRudpEnd",golf_rudp_end},
    {"sceRudpEnableInternalIOThread",golf_rudp_start}, {"sceRudpSetEventHandler",golf_rudp_set_handler},
    {"sceRudpCreateContext",golf_rudp_create}, {"sceRudpBind",golf_rudp_bind},
    {"sceRudpSetOption",golf_rudp_option}, {"sceRudpInitiate",golf_rudp_initiate},
    {"sceRudpTerminate",golf_rudp_terminate}, {"sceRudpGetContextStatus",golf_rudp_status},
    {"sceRudpWrite",golf_rudp_write}, {"sceRudpRead",golf_rudp_read},
    {"sceRudpGetSizeReadable",golf_rudp_size}, {"sceRudpGetNumberOfPacketsToRead",golf_rudp_count},
};

uintptr_t runtime_golf_resolve(const char *name) {
    /* name is the import key (NID#library): the tables resolve it, a prefix test here cannot. */
    if (replay_auth) {
        uintptr_t address=RUNTIME_LOOKUP(auth_exports,name);
        if (address) return address;
    }
    if (local_lobbies && getenv("GOLF_DISCOVERY_PORT")) {
        uintptr_t address=RUNTIME_LOOKUP(m2_room_exports,name);
        if (address) return address;
        address=RUNTIME_LOOKUP(golf_signaling_exports,name);
        if (address) return address;
    }
    if (local_account && getenv("GOLF_DISCOVERY_PORT")) {
        uintptr_t address=RUNTIME_LOOKUP(matching_exports,name);
        if (address) return address;
    }
    if (local_lobbies && getenv("GOLF_DISCOVERY_PORT")) {
        uintptr_t address=RUNTIME_LOOKUP(golf_rudp_exports,name);
        if (address) return address;
        address=RUNTIME_LOOKUP(golf_peer_socket_exports,name);
        if (address) return address;
    }
    return local_account ? RUNTIME_LOOKUP(exports,name) : 0;
}
