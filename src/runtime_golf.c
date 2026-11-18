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

#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define WEBAPI_OUT_OF_MEMORY ((int32_t)0x80552901)
#define WEBAPI_INVALID_ARGUMENT ((int32_t)0x80552902)
#define WEBAPI_INVALID_LIB_CONTEXT_ID ((int32_t)0x80552903)
#define WEBAPI_REQUEST_NOT_FOUND ((int32_t)0x80552907)
#define USER_INVALID (-1)

static int local_account, replay_tickets;
static char online_id[17]="Golfer";
void runtime_golf_set_title(const char *title_id) {
    const char *mode=getenv("GOLF_NP"), *name=getenv("GOLF_ONLINE_ID");
    local_account=title_id && !strncmp(title_id,"CUSA04687",9) && !(mode && !strcmp(mode,"offline"));
    const char *grants=getenv("GOLF_TICKET_GRANTS");
    replay_tickets=grants && !strcmp(grants,"1");
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
    if (lib>=0x8000) return WEBAPI_INVALID_LIB_CONTEXT_ID;
    if (user==USER_INVALID) return WEBAPI_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    const int32_t id=next_context++;
    pthread_mutex_unlock(&lock);
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

static const RuntimeExport exports[]={
    {"sceNpGetState",np_state}, {"sceNpGetOnlineId",np_online_id}, {"sceNpGetNpId",np_id},
    {"sceNpGetAccountCountryA",np_country},
    {"sceNpWebApiCreateContextA",webapi_context}, {"sceNpWebApiDeleteContext",ok},
    {"sceNpWebApiCreateRequest",webapi_create}, {"sceNpWebApiSendRequest2",webapi_send2},
    {"sceNpWebApiSendRequest",webapi_send}, {"sceNpWebApiGetHttpStatusCode",webapi_status},
    {"sceNpWebApiReadData",webapi_read}, {"sceNpWebApiDeleteRequest",webapi_delete},
    {"sceNpWebApiAbortRequest",ok},
};
uintptr_t runtime_golf_resolve(const char *name) { return local_account ? RUNTIME_LOOKUP(exports,name) : 0; }
