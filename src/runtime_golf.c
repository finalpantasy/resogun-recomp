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

#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define AUTH_INVALID_ARGUMENT ((int32_t)0x80550301)
#define AUTH_INVALID_SIZE ((int32_t)0x80550302)
#define AUTH_INVALID_ID ((int32_t)0x80550307)
#define WEBAPI_OUT_OF_MEMORY ((int32_t)0x80552901)
#define WEBAPI_INVALID_ARGUMENT ((int32_t)0x80552902)
#define WEBAPI_INVALID_LIB_CONTEXT_ID ((int32_t)0x80552903)
#define WEBAPI_REQUEST_NOT_FOUND ((int32_t)0x80552907)
#define USER_INVALID (-1)

static int local_account, replay_tickets, replay_auth;
static pthread_mutex_t auth_lock=PTHREAD_MUTEX_INITIALIZER;
static int32_t auth_id, auth_next=0x10010000;
static int auth_ready;
static char online_id[17]="Golfer";
void runtime_golf_set_title(const char *title_id) {
    const char *mode=getenv("GOLF_NP"), *name=getenv("GOLF_ONLINE_ID");
    local_account=title_id && !strncmp(title_id,"CUSA04687",9) && !(mode && !strcmp(mode,"offline"));
    const char *grants=getenv("GOLF_TICKET_GRANTS");
    replay_tickets=grants && !strcmp(grants,"1");
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
typedef ABI void (*M2ContextCb)(uint16_t ctx,uint16_t event,uint8_t cause,int32_t error,void *arg);
typedef ABI void (*M2RequestCb)(uint16_t ctx,uint32_t req,uint16_t event,int32_t error,const void *data,void *arg);
typedef struct { M2RequestCb cb; void *arg, *data; uint32_t req; int32_t error; uint16_t event, ctx; } M2Event;
static pthread_mutex_t m2_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t m2_wake=PTHREAD_COND_INITIALIZER;
static M2Event m2_queue[32];
static unsigned m2_head, m2_count;
static int m2_thread_up;
static M2ContextCb m2_context_cb;
static void *m2_context_arg;
static uint16_t m2_ctx;
static int32_t m2_signaling_ctx;
static void *m2_thread(void *unused) {
    (void)unused;
    runtime_thread_attach_host("NpMatching2");
    for (;;) {
        pthread_mutex_lock(&m2_lock);
        while (!m2_count) pthread_cond_wait(&m2_wake,&m2_lock);
        const M2Event e=m2_queue[m2_head];
        m2_head=(m2_head+1)%32; --m2_count;
        const M2ContextCb context_cb=m2_context_cb; void *context_arg=m2_context_arg;
        pthread_mutex_unlock(&m2_lock);
        struct timespec nap={0,30*1000*1000}; nanosleep(&nap,NULL);
        if (e.cb) {
            printf("Runtime: NpMatching2 request %u answered: event 0x%x, error 0x%x\n",(unsigned)e.req,(unsigned)e.event,(unsigned)e.error);
            e.cb(e.ctx,e.req,e.event,e.error,e.data,e.arg);
        } else if (context_cb) {
            printf("Runtime: NpMatching2 context %u event 0x%x\n",(unsigned)e.ctx,(unsigned)e.event);
            context_cb(e.ctx,e.event,0,e.error,context_arg);
        }
    }
    return NULL;
}
/* Queue one callback: cb null means the context callback. */
static void m2_post(M2Event e) {
    pthread_mutex_lock(&m2_lock);
    if (!m2_thread_up) {
        pthread_t thread;
        if (!pthread_create(&thread,NULL,m2_thread,NULL)) { pthread_detach(thread); m2_thread_up=1; }
    }
    if (m2_count<32) { m2_queue[(m2_head+m2_count)%32]=e; ++m2_count; }
    else puts("Runtime: NpMatching2 event queue full, event dropped");
    pthread_cond_signal(&m2_wake);
    pthread_mutex_unlock(&m2_lock);
}
static ABI int32_t m2_create_context_a(const uint8_t *param,uint16_t *ctx) {
    if (!param || !ctx) return NP_INVALID_ARGUMENT;
    pthread_mutex_lock(&m2_lock);
    *ctx=m2_ctx=1;
    pthread_mutex_unlock(&m2_lock);
    puts("Runtime: NpMatching2 context 1 created (local)");
    return 0;
}
static ABI int32_t m2_register_context_cb(uint16_t ctx,M2ContextCb cb,void *arg) {
    (void)ctx;
    pthread_mutex_lock(&m2_lock);
    m2_context_cb=cb; m2_context_arg=arg;
    pthread_mutex_unlock(&m2_lock);
    return 0;
}
/* Lobby, room and signaling callbacks: stored by the library until an event arrives. No event of
 * those kinds is produced yet, so the registration only has to succeed. */
static ABI int32_t m2_register_cb(uint16_t ctx,void *cb,void *arg) { (void)ctx; (void)cb; (void)arg; return 0; }
static ABI int32_t m2_context_start(uint16_t ctx,uint64_t timeout) {
    (void)timeout;
    m2_post((M2Event){.ctx=ctx,.event=0x6f02});
    return 0;
}
static ABI int32_t m2_context_stop(uint16_t ctx) {
    m2_post((M2Event){.ctx=ctx,.event=0x6f03});
    return 0;
}
static ABI int32_t m2_destroy_context(uint16_t ctx) {
    (void)ctx;
    pthread_mutex_lock(&m2_lock);
    m2_ctx=0; m2_context_cb=NULL;
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
 *   count at +0xc, population at +0x14): one world with one lobby.
 * GetLobbyInfoList -> event 0x6, {.., u32 count at +8, rows * at +0x10}, 0x50-byte rows (u64 lobbyId
 *   at +0x10, u16 members at +0x1a): the one lobby, empty.
 * JoinLobby -> event 0x201, {P *}: P+0x10 u64 lobbyId, P+0x18 u16 memberIds *, P+0x20 u64 count,
 *   P+0x28 u16 own member id.
 * GetLobbyMemberDataInternalList -> event 0x7207, {first record *, u64 count}; record: next * at +0,
 *   online id at +0x20, u16 member id at +0x34: the local player alone.
 * LeaveLobby -> event 0x202, no data. */
typedef struct { M2RequestCb cb; void *arg; uint32_t timeout, pad; } M2Opt;
static M2Opt m2_default_opt;
static uint32_t m2_next_request=1;
static uint8_t m2_world_row[0x28], m2_lobby_row[0x50], m2_join_info[0x30], m2_member[0x40];
static uint16_t m2_member_ids[1]={1};
static struct { void *rows; uint32_t count; } m2_world_list;
static struct { uint64_t pad; uint32_t count, pad2; void *rows; } m2_lobby_list;
static struct { void *info; } m2_join;
static struct { void *first; uint64_t count; } m2_member_list;
static ABI int32_t m2_set_default_opt(uint16_t ctx,const M2Opt *opt) {
    (void)ctx;
    pthread_mutex_lock(&m2_lock);
    if (opt) m2_default_opt=*opt; else memset(&m2_default_opt,0,sizeof m2_default_opt);
    pthread_mutex_unlock(&m2_lock);
    return 0;
}
static int32_t m2_answer(const char *what,uint16_t ctx,const M2Opt *opt,uint32_t *id,uint16_t event,void *data) {
    pthread_mutex_lock(&m2_lock);
    const M2Opt use=opt ? *opt : m2_default_opt;
    const uint32_t request=m2_next_request++;
    pthread_mutex_unlock(&m2_lock);
    if (id) *id=request;
    printf("Runtime: NpMatching2 %s: request %u (local answer follows)\n",what,(unsigned)request);
    if (use.cb) m2_post((M2Event){.cb=use.cb,.arg=use.arg,.data=data,.req=request,.event=event,.ctx=ctx});
    return 0;
}
static ABI int32_t m2_world_info_list(uint16_t ctx,const void *request,const M2Opt *opt,uint32_t *id) {
    if (!request) return NP_INVALID_ARGUMENT;
    const uint32_t world=1, lobbies=1;
    memset(m2_world_row,0,sizeof m2_world_row);
    memcpy(m2_world_row+8,&world,4); memcpy(m2_world_row+0xc,&lobbies,4);
    m2_world_list.rows=m2_world_row; m2_world_list.count=1;
    return m2_answer("GetWorldInfoList",ctx,opt,id,0x2,&m2_world_list);
}
static ABI int32_t m2_lobby_info_list(uint16_t ctx,const void *request,const M2Opt *opt,uint32_t *id) {
    if (!request) return NP_INVALID_ARGUMENT;
    const uint64_t lobby=1;
    memset(m2_lobby_row,0,sizeof m2_lobby_row);
    memcpy(m2_lobby_row+0x10,&lobby,8);
    m2_lobby_list.rows=m2_lobby_row; m2_lobby_list.count=1;
    return m2_answer("GetLobbyInfoList",ctx,opt,id,0x6,&m2_lobby_list);
}
static ABI int32_t m2_join_lobby(uint16_t ctx,const uint64_t *request,const M2Opt *opt,uint32_t *id) {
    if (!request) return NP_INVALID_ARGUMENT;
    const uint64_t lobby=*request, count=1; void *ids=m2_member_ids;
    memset(m2_join_info,0,sizeof m2_join_info);
    memcpy(m2_join_info+0x10,&lobby,8); memcpy(m2_join_info+0x18,&ids,8);
    memcpy(m2_join_info+0x20,&count,8); memcpy(m2_join_info+0x28,m2_member_ids,2);
    m2_join.info=m2_join_info;
    return m2_answer("JoinLobby",ctx,opt,id,0x201,&m2_join);
}
static ABI int32_t m2_lobby_members(uint16_t ctx,const void *request,const M2Opt *opt,uint32_t *id) {
    if (!request) return NP_INVALID_ARGUMENT;
    memset(m2_member,0,sizeof m2_member);
    snprintf((char *)m2_member+0x20,17,"%s",online_id);
    memcpy(m2_member+0x34,m2_member_ids,2);
    m2_member_list.first=m2_member; m2_member_list.count=1;
    return m2_answer("GetLobbyMemberDataInternalList",ctx,opt,id,0x7207,&m2_member_list);
}
static ABI int32_t m2_leave_lobby(uint16_t ctx,const void *request,const M2Opt *opt,uint32_t *id) {
    (void)request;
    return m2_answer("LeaveLobby",ctx,opt,id,0x202,NULL);
}
static const RuntimeExport matching_exports[]={
    {"sceNpMatching2CreateContextA",m2_create_context_a},
    {"sceNpMatching2RegisterContextCallback",m2_register_context_cb},
    {"sceNpMatching2RegisterLobbyEventCallback",m2_register_cb},
    {"sceNpMatching2RegisterLobbyMessageCallback",m2_register_cb},
    {"sceNpMatching2RegisterRoomEventCallback",m2_register_cb},
    {"sceNpMatching2RegisterRoomMessageCallback",m2_register_cb},
    {"sceNpMatching2RegisterSignalingCallback",m2_register_cb},
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
uintptr_t runtime_golf_resolve(const char *name) {
    /* name is the import key (NID#library): the tables resolve it, a prefix test here cannot. */
    if (replay_auth) {
        uintptr_t address=RUNTIME_LOOKUP(auth_exports,name);
        if (address) return address;
    }
    if (local_account && getenv("GOLF_DISCOVERY_PORT")) {
        uintptr_t address=RUNTIME_LOOKUP(matching_exports,name);
        if (address) return address;
    }
    return local_account ? RUNTIME_LOOKUP(exports,name) : 0;
}
