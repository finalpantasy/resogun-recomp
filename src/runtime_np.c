/* PSN libraries for the offline console (runtime_services.c holds NpManager, the older NpScore,
 * Matching2 and WebApi calls, and the trophies). The one user is signed out, so (shadPS4 np_*):
 *   - creating a title, user or matching context fails with NP SIGNED_OUT;
 *   - a call on a context or request that can therefore not exist fails with that library's
 *     INVALID_ID; a request object the old API created fails its transfer with SIGNED_OUT;
 *   - objects that need no sign-in (WebApi handles and push filters, InGameMessage contexts,
 *     NpAuth requests) are created, and their server calls fail with SIGNED_OUT.
 * Dialogs about PSN or the system (sign-in, invitation, error, social screen) draw nothing: an
 * opened one finishes at once, cancelled by the user. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define COMMUNITY_INVALID_ARGUMENT ((int32_t)0x80550704)
#define COMMUNITY_INVALID_ID ((int32_t)0x8055070E)
#define AUTH_INVALID_ARGUMENT ((int32_t)0x80550301)
#define MATCHING2_INVALID_ARGUMENT ((int32_t)0x80550C0A)
#define MATCHING2_INVALID_CONTEXT_ID ((int32_t)0x80550C0B)
#define WEBAPI_INVALID_ARGUMENT ((int32_t)0x80552902)
#define WEBAPI_INVALID_LIB_CONTEXT_ID ((int32_t)0x80552903)
#define INVALID_SERVICE_LABEL 0xFFFFFFFFu
#define USER_INVALID (-1)

static _Atomic int32_t next_object=1;
static int32_t new_object(void) { return atomic_fetch_add(&next_object,1); }
static ABI int32_t ok(void) { return 0; }
static ABI int32_t signed_out(void) { return NP_SIGNED_OUT; }

/* ---- NpTus / NpTss (shadPS4 np_tus.cpp) ----
 * sceNpTusCreateNpTitleCtxA(label, user) and sceNpTssCreateNpTitleCtxA need the user's NpId:
 * SIGNED_OUT. With no title context, sceNpTusCreateRequest(ctx), every Get/Set/Add/TryAndSet/
 * Delete call on a request, sceNpTusAbortRequest, sceNpTusDeleteRequest and
 * sceNpTusDeleteNpTitleCtx find no such id: COMMUNITY INVALID_ID. */
static ABI int32_t community_no_id(void) { return COMMUNITY_INVALID_ID; }

/* ---- NpScore A variants (shadPS4 np_score.cpp) ----
 * sceNpScoreCreateNpTitleCtxA(label, user): SIGNED_OUT first, then the label is checked. The
 * ranking and game-data calls take a request from sceNpScoreCreateRequest and fail SIGNED_OUT,
 * as the older calls in runtime_services.c do. */
static ABI int32_t score_ctx_a(uint32_t label,int32_t user) { (void)label; (void)user; return NP_SIGNED_OUT; }

/* ---- NpMatching2 (shadPS4 np_matching2.cpp) ----
 * sceNpMatching2CreateContextA(param{userId, serviceLabel, size=0x10}, ctxId): INVALID_ARGUMENT
 * for a bad param, else SIGNED_OUT. Every call that names a context then fails with
 * INVALID_CONTEXT_ID. sceNpMatching2GetMemoryInfo / GetSslMemoryInfo: OK, as shadPS4. */
static ABI int32_t matching2_context_a(const uint8_t *param,uint16_t *ctx) {
    if (!param || !ctx) return MATCHING2_INVALID_ARGUMENT;
    uint64_t size; memcpy(&size,param+8,8);
    return size!=0x10 ? MATCHING2_INVALID_ARGUMENT : NP_SIGNED_OUT;
}
static ABI int32_t matching2_no_context(void) { return MATCHING2_INVALID_CONTEXT_ID; }

/* ---- NpWebApi (shadPS4 np_web_api.cpp) ----
 * sceNpWebApiCreateContextA(libCtx, user): a user context needs PSN: SIGNED_OUT (as
 * sceNpWebApiCreateContext). Handles and extended push-event filters belong to the library
 * context and are created; requests and push callbacks need a user context: SIGNED_OUT. */
static ABI int32_t webapi_context_a(int32_t lib,int32_t user) {
    if (lib>=0x8000) return WEBAPI_INVALID_LIB_CONTEXT_ID;
    return user==USER_INVALID ? WEBAPI_INVALID_ARGUMENT : NP_SIGNED_OUT;
}
static ABI int32_t webapi_handle(int32_t lib) { return lib>=0x8000 ? WEBAPI_INVALID_LIB_CONTEXT_ID : new_object(); }
/* sceNpWebApiCreateExtdPushEventFilter(lib, handle, service name, label, params, count). */
static ABI int32_t webapi_filter(int32_t lib,int32_t handle,const char *service,uint32_t label,const void *params,uint64_t count) {
    (void)handle;
    if ((service && label==INVALID_SERVICE_LABEL) || !params || !count) return WEBAPI_INVALID_ARGUMENT;
    return lib>=0x8000 ? WEBAPI_INVALID_LIB_CONTEXT_ID : new_object();
}
/* sceNpWebApiRegisterExtdPushEventCallbackA(userCtx, filter, callback, arg). */
static ABI int32_t webapi_callback_a(int32_t ctx,int32_t filter,void *callback,void *arg) {
    (void)ctx; (void)filter; (void)arg; return callback ? NP_SIGNED_OUT : WEBAPI_INVALID_ARGUMENT;
}

/* ---- NpUtility (shadPS4 np_utility.cpp) ----
 * sceNpWordFilterCreateTitleCtxA(user): SIGNED_OUT; the request and sanitize calls then name a
 * context that does not exist: COMMUNITY INVALID_ID. The bandwidth test measures the PSN
 * connection: sceNpBandwidthTestInitStart / GetStatus / Shutdown fail SIGNED_OUT, Abort is OK. */

/* ---- NpManager InGameMessage ----
 * sceNpInGameMessageInitialize(pool size, option) gives a library context and
 * sceNpInGameMessageCreateHandle(ctx) a handle; PrepareA and SendDataA send to other players:
 * SIGNED_OUT. Delete, Terminate and the pool statistics are OK. */
static ABI int32_t object(void) { return new_object(); }

/* ---- NpAuth (shadPS4 np_auth.cpp) ----
 * sceNpAuthCreateRequest: a synchronous request id (0x10000001 up). sceNpAuthGetAuthorizationCodeA
 * (req, param, code, issuer) and sceNpAuthGetIdTokenA(req, param, token) need param and output,
 * then fail SIGNED_OUT. sceNpAuthAbortRequest: OK. */
static _Atomic int32_t next_auth=0x10000001;
static ABI int32_t auth_request(void) { return atomic_fetch_add(&next_auth,1); }
static ABI int32_t auth_code_a(int32_t req,const void *param,void *code,int32_t *issuer) {
    (void)req; (void)issuer; return !param || !code ? AUTH_INVALID_ARGUMENT : NP_SIGNED_OUT;
}

/* ---- NpCommerce: sceNpCommerceShowPsStoreIcon(pos) / HidePsStoreIcon: OK, no icon is drawn
 * (the Store is unreachable offline). ---- */

/* ---- Dialogs: status 0 none, 1 initialized, 2 running, 3 finished ---- */
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)
#define DIALOG_ALREADY_INITIALIZED ((int32_t)0x80B80004)
#define DIALOG_NOT_FINISHED ((int32_t)0x80B80005)
#define DIALOG_ARG_NULL ((int32_t)0x80B8000D)
#define RESULT_USER_CANCELED 1
enum { SIGNIN, INVITATION, ERROR_DIALOG, SOCIAL_SCREEN, FRIEND_LIST, PROFILE, GAME_CUSTOM, DIALOGS };
static const char *const dialog_names[DIALOGS]={"SigninDialog","InvitationDialog","ErrorDialog","SocialScreenDialog","NpFriendListDialog","NpProfileDialog","GameCustomDataDialog"};
static _Atomic int dialog_status[DIALOGS];
static int32_t dialog_init(int d) {
    int none=0;
    return atomic_compare_exchange_strong(&dialog_status[d],&none,1) ? 0 : DIALOG_ALREADY_INITIALIZED;
}
static int32_t dialog_open(int d,const char *detail) {
    if (!atomic_load(&dialog_status[d])) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s opened%s; finished at once (offline, no dialog UI)\n",dialog_names[d],detail);
    atomic_store(&dialog_status[d],3); return 0;
}
static int32_t dialog_result(int d,void *result) {
    if (atomic_load(&dialog_status[d])!=3) return DIALOG_NOT_FINISHED;
    return result ? 0 : DIALOG_ARG_NULL;
}
static int32_t dialog_term(int d) {
    if (!atomic_load(&dialog_status[d])) return DIALOG_NOT_INITIALIZED;
    atomic_store(&dialog_status[d],0); return 0;
}
/* sceInvitationDialog*: nothing can be sent. OrbisInvitationDialogResultA: callbackArg @0 (kept),
 * errorCode @8, result @12, sentUsers @16, reserved[32]. */
static ABI int32_t invitation_init(void) { return dialog_init(INVITATION); }
static ABI int32_t invitation_open(const void *param) { (void)param; return dialog_open(INVITATION,""); }
static ABI int32_t invitation_status(void) { return atomic_load(&dialog_status[INVITATION]); }
static ABI int32_t invitation_result(uint8_t *result) {
    int32_t r=dialog_result(INVITATION,result);
    if (r) return r;
    int32_t codes[2]={NP_SIGNED_OUT,RESULT_USER_CANCELED};
    memcpy(result+8,codes,8); memset(result+16,0,40);
    return 0;
}
static ABI int32_t invitation_term(void) { return dialog_term(INVITATION); }
/* sceSigninDialog*: there is no PSN to sign in to; the player cancels. OrbisSigninDialogResult
 * starts with int32 result. Everybody's Golf polls UpdateStatus from 0xa5ab55 every frame. */
static ABI int32_t signin_init(void) { return dialog_init(SIGNIN); }
static ABI int32_t signin_open(const void *param) { if (!param) return DIALOG_ARG_NULL; return dialog_open(SIGNIN,""); }
static ABI int32_t signin_status(void) { return atomic_load(&dialog_status[SIGNIN]); }
static ABI int32_t signin_result(int32_t *result) {
    int32_t r=dialog_result(SIGNIN,result);
    if (r) return r;
    *result=RESULT_USER_CANCELED;
    return 0;
}
static ABI int32_t signin_term(void) { return dialog_term(SIGNIN); }
static ABI int32_t signin_close(void) { return atomic_load(&dialog_status[SIGNIN]) ? 0 : DIALOG_NOT_INITIALIZED; }
/* sceErrorDialog*: param {int32 size, int32 errorCode, ...}; the code goes to the log. */
static ABI int32_t error_init(void) { return dialog_init(ERROR_DIALOG); }
static ABI int32_t error_open(const int32_t *param) {
    if (!param) return DIALOG_ARG_NULL;
    char detail[48]; snprintf(detail,sizeof(detail)," (error 0x%08x)",(uint32_t)param[1]);
    return dialog_open(ERROR_DIALOG,detail);
}
static ABI int32_t error_status(void) { return atomic_load(&dialog_status[ERROR_DIALOG]); }
static ABI int32_t error_term(void) { return dialog_term(ERROR_DIALOG); }
/* sceInvitationDialogClose / sceErrorDialogClose: as sceSocialScreenDialogClose. */
static ABI int32_t invitation_close(void) { return atomic_load(&dialog_status[INVITATION]) ? 0 : DIALOG_NOT_INITIALIZED; }
static ABI int32_t error_close(void) { return atomic_load(&dialog_status[ERROR_DIALOG]) ? 0 : DIALOG_NOT_INITIALIZED; }
/* sceNpFriendListDialog*: no friends offline; the player leaves it. The result's first field is
 * the result (USER_CANCELED). */
static ABI int32_t friends_init(void) { return dialog_init(FRIEND_LIST); }
static ABI int32_t friends_open(const void *param) { (void)param; return dialog_open(FRIEND_LIST,""); }
static ABI int32_t friends_status(void) { return atomic_load(&dialog_status[FRIEND_LIST]); }
static ABI int32_t friends_result(int32_t *result) {
    int32_t r=dialog_result(FRIEND_LIST,result);
    if (!r) *result=RESULT_USER_CANCELED;
    return r;
}
static ABI int32_t friends_close(void) { return atomic_load(&dialog_status[FRIEND_LIST]) ? 0 : DIALOG_NOT_INITIALIZED; }
static ABI int32_t friends_term(void) { return dialog_term(FRIEND_LIST); }


/* ---- New error codes (np_error.h) ---- */
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NP_CALLBACK_ALREADY_REGISTERED ((int32_t)0x80550008)
#define NP_CALLBACK_NOT_REGISTERED ((int32_t)0x80550009)
#define UTIL_NOT_MATCH ((int32_t)0x80550609)
#define AUTH_INVALID_SIZE ((int32_t)0x80550302)
#define AUTH_INVALID_ID ((int32_t)0x80550307)
#define COMMUNITY_INSUFFICIENT_ARGUMENT ((int32_t)0x8055070C)
#define WEBAPI_REQUEST_NOT_FOUND ((int32_t)0x80552906)
#define SIGNALING_NOT_INITIALIZED ((int32_t)0x80552701)
#define SIGNALING_ALREADY_INITIALIZED ((int32_t)0x80552702)
#define SIGNALING_CTX_NOT_FOUND ((int32_t)0x80552705)
#define SIGNALING_INVALID_ARGUMENT ((int32_t)0x80552715)
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define RUDP_NOT_INITIALIZED ((int32_t)0x80770001) /* R: PS3 header; PS4 values NOT_VERIFIED */
#define RUDP_ALREADY_INITIALIZED ((int32_t)0x80770002)
#define RUDP_INVALID_CONTEXT_ID ((int32_t)0x80770003)
#define RUDP_INVALID_ARGUMENT ((int32_t)0x80770004)
#define RUDP_NOT_BOUND ((int32_t)0x80770013)
#define RUDP_INVALID_SOCKET ((int32_t)0x80770019)
#define RUDP_TOO_MANY_CONTEXTS ((int32_t)0x80770020)

/* ---- NpTus (P np_tus.cpp, S) ----
 * sceNpTusCreateNpTitleCtx(serviceLabel, NpId *self): P checks npId (INSUFFICIENT_ARGUMENT) and
 * the label (INVALID_ARGUMENT), then creates the context; here the context needs the user's
 * NpId: SIGNED_OUT, as sceNpTusCreateNpTitleCtxA. Every request call (Get/Set/AddAndGet/
 * TryAndSet/DeleteMultiSlot, plain, A, VUser, AVUser, ForCrossSave and Friends forms, and
 * sceNpTssGetData) takes the reqId of sceNpTusCreateRequest, which cannot exist: community
 * INVALID_ID (community_no_id, as the *Async forms in the existing table). Real argument lists:
 *   (int reqId, <target>, s32 slotId | s32 *slotIds, ..., void *option)  [P signatures]. */
static ABI int32_t tus_ctx(uint32_t label,const void *np_id) {
    if (!np_id) return COMMUNITY_INSUFFICIENT_ARGUMENT;
    return label==INVALID_SERVICE_LABEL ? COMMUNITY_INVALID_ARGUMENT : NP_SIGNED_OUT;
}

/* ---- NpScore (P np_score.cpp, S) ----
 * sceNpScoreCreateNpTitleCtx(label, NpId *self): same checks and result as tus_ctx (P order:
 * label INVALID_ARGUMENT before the null NpId INSUFFICIENT_ARGUMENT).
 * sceNpScoreGet(Friends)Ranking*, GetGameData*, GetBoardInfo, RecordScore, RecordGameData
 * (sync, non-A, ForCrossSave, PcId forms): the request fails its transfer: SIGNED_OUT (signed_out),
 * as the *Async forms already do. Argument lists: (s32 reqId, boardId, ..., void *option).
 * sceNpScoreSetPlayerCharacterId(ctxId, pcId): pcId < 0 INVALID_ARGUMENT, else the ctx/req id
 * cannot exist: INVALID_ID (P). */
static ABI int32_t score_ctx(uint32_t label,const void *np_id) {
    if (label==INVALID_SERVICE_LABEL) return COMMUNITY_INVALID_ARGUMENT;
    return np_id ? NP_SIGNED_OUT : COMMUNITY_INSUFFICIENT_ARGUMENT;
}
static ABI int32_t score_pc_id(int32_t ctx,int32_t pc_id) {
    (void)ctx; return pc_id<0 ? COMMUNITY_INVALID_ARGUMENT : COMMUNITY_INVALID_ID;
}

/* ---- NpUtility (P np_utility.cpp, S; C: eboot calls CreateTitleCtxA at 0xa8b263 with the user
 * id from a global (0x2ccbdb4), stores the id only if >= 0, and guards every Delete with id >= 0,
 * so a failed create is harmless) ----
 * sceNpLookupCreateTitleCtx(NpId *self) (null: INSUFFICIENT_
 * ARGUMENT as P), sceNpWordFilterCreateTitleCtx(NpId *self): SIGNED_OUT as WordFilterCreateTitleCtxA
 * (CreateTitleCtxA(user) is in runtime_np.c already).
 * sceNpLookupCreateRequest(titleCtx), sceNpLookupDeleteTitleCtx(titleCtx), sceNpLookupDeleteRequest
 * (req), sceNpLookupNpId(req, onlineId, npId, option), sceNpLookupPollAsync(req, *result),
 * sceNpWordFilterCreateRequest(titleCtx), sceNpWordFilterSanitizeComment(req, comment, out,
 * option): the id cannot exist: INVALID_ID. sceNpBandwidthTestInitStart / GetStatus /
 * Shutdown: SIGNED_OUT (runtime_np.c header); P returns OK from logging stubs, not copied.
 * sceNpBandwidthTestAbort: OK (nothing running). Real arguments of InitStart/GetStatus
 * NOT_VERIFIED (P has none). */
static ABI int32_t utility_ctx(const void *np_id) { return np_id ? NP_SIGNED_OUT : COMMUNITY_INSUFFICIENT_ARGUMENT; }

/* ---- NpManager, library side (P np_manager.cpp) ----
 * sceNpInGameMessagePrepare(ctx, handle, ...) / SendData(ctx, handle, ...): like the A forms,
 * SIGNED_OUT. sceNpInGameMessageGetMemoryPoolStatistics(ctx, stat *): statistics of a pool the
 * library holds: OK (as the existing Terminate; struct layout NOT_VERIFIED, nothing written).
 * sceNpRegisterNpReachabilityStateCallback(cb, arg) (P): null callback INVALID_ARGUMENT, a second
 * registration CALLBACK_ALREADY_REGISTERED, else OK (state is signed out, no event follows).
 * C: eboot 0xb9af0b ignores the result. ForToolkit: sceNpRegisterStateCallbackForToolkit(cb, arg)
 * OK (P), sceNpUnregisterStateCallbackForToolkit() CALLBACK_NOT_REGISTERED unless registered
 * (P), sceNpCheckCallbackForLib() OK, nothing pending. */
static _Atomic int reachability_cb, toolkit_cb;
static ABI int32_t reachability_register(void *cb,void *arg) {
    (void)arg;
    if (!cb) return NP_INVALID_ARGUMENT;
    int none=0;
    return atomic_compare_exchange_strong(&reachability_cb,&none,1) ? 0 : NP_CALLBACK_ALREADY_REGISTERED;
}
static ABI int32_t toolkit_register(void *cb,void *arg) { (void)cb; (void)arg; atomic_store(&toolkit_cb,1); return 0; }
static ABI int32_t toolkit_unregister(void) {
    int one=1;
    return atomic_compare_exchange_strong(&toolkit_cb,&one,0) ? 0 : NP_CALLBACK_NOT_REGISTERED;
}

/* ---- NpCommon: sceNpCmpNpId(NpId *a, NpId *b) (P np_common.cpp): null INVALID_ARGUMENT; NpId is
 * handle{data[16], term, dummy[3]}, opt[8], reserved[8]: the data (strncmp 16), opt and reserved
 * must match, else UTIL NOT_MATCH. ---- */
static ABI int32_t np_cmp_id(const uint8_t *a,const uint8_t *b) {
    if (!a || !b) return NP_INVALID_ARGUMENT;
    if (strncmp((const char *)a,(const char *)b,16)) return UTIL_NOT_MATCH;
    return memcmp(a+20,b+20,16) ? UTIL_NOT_MATCH : 0;
}

/* ---- NpAuth (P np_auth.cpp, S; C: eboot 0xa60ca9 creates an async request with
 * {size=0x18, mask, priority=700}, tests < 0, then calls GetAuthorizationCodeA(req, param{size
 * 0x20, user..}, code, issuer) and goes on only if that is >= 0; on failure it deletes the
 * request. Poll (0xb97f4c): 0 finished or < 0 deletes it, > 0 keeps polling.) ----
 * sceNpAuthCreateAsyncRequest(param{u64 size=0x18, mask, s32 priority}): null INVALID_ARGUMENT,
 * wrong size INVALID_SIZE, else an id (shared with the sync ids). sceNpAuthGetAuthorizationCode
 * (req, param, code, issuer) is the existing auth_code_a. sceNpAuthGetIdTokenA(req, param, token):
 * pointers checked, then SIGNED_OUT at once, returned directly even on an async id (as the
 * existing auth_code_a; the game's fail path never polls). sceNpAuthPollAsync(req, *result): no
 * operation is ever left running, so the async request has none to report: INVALID_ID (P
 * returns it for an async request whose operation never started). */
static ABI int32_t auth_async_request(const uint64_t *param) {
    if (!param) return AUTH_INVALID_ARGUMENT;
    return param[0]!=24 ? AUTH_INVALID_SIZE : auth_request();
}
static ABI int32_t auth_token_a(int32_t req,const void *param,void *token) {
    (void)req; return !param || !token ? AUTH_INVALID_ARGUMENT : NP_SIGNED_OUT;
}
static ABI int32_t auth_poll(int32_t req,int32_t *result) {
    (void)req; return result ? AUTH_INVALID_ID : AUTH_INVALID_ARGUMENT;
}

/* ---- NpWebApi (P np_web_api.cpp / np_web_api_internal.cpp, S) ----
 * sceNpWebApiCreateContext(lib, OrbisNpOnlineId *): lib >= 0x8000 INVALID_LIB_CONTEXT_ID, null
 * INVALID_ARGUMENT, else a user context needs PSN: SIGNED_OUT (as ...ContextA).
 * sceNpWebApiCreatePushEventFilter(lib, params, count): null/0 INVALID_ARGUMENT, bad lib, else
 * created (a library-level object). sceNpWebApiCreateServicePushEventFilter(lib, handle, service,
 * label, params, count): also needs the service name and a label != INVALID. DeletePush/
 * DeleteServicePush filter: OK as DeleteExtd. Register[Service]PushEventCallback(userCtx, filter,
 * cb, arg) is webapi_callback_a; Unregister[Service]PushEventCallback(userCtx, id): OK as
 * UnregisterExtd (P: USER_CONTEXT_NOT_FOUND, kept consistent with the Extd sibling).
 * Request header calls take a request id from CreateRequest, which fails: REQUEST_NOT_FOUND (P
 * finds no request) after the null checks. GetMemoryPoolStats(lib, stats{u64 pool, max, cur; s32}):
 * a library pool with nothing in use; the pool size is NOT_VERIFIED, zero is written. */
static ABI int32_t webapi_context(int32_t lib,const void *online_id) {
    if (lib>=0x8000) return WEBAPI_INVALID_LIB_CONTEXT_ID;
    return online_id ? NP_SIGNED_OUT : WEBAPI_INVALID_ARGUMENT;
}
static ABI int32_t webapi_plain_filter(int32_t lib,const void *params,uint64_t count) {
    if (!params || !count) return WEBAPI_INVALID_ARGUMENT;
    return lib>=0x8000 ? WEBAPI_INVALID_LIB_CONTEXT_ID : new_object();
}
static ABI int32_t webapi_service_filter(int32_t lib,int32_t handle,const char *service,uint32_t label,const void *params,uint64_t count) {
    (void)handle;
    if (!service || !params || !count || label==INVALID_SERVICE_LABEL) return WEBAPI_INVALID_ARGUMENT;
    return lib>=0x8000 ? WEBAPI_INVALID_LIB_CONTEXT_ID : new_object();
}
static ABI int32_t webapi_add_header(int64_t req,const char *name,const char *value) {
    (void)req; return !name || !value ? WEBAPI_INVALID_ARGUMENT : WEBAPI_REQUEST_NOT_FOUND;
}
static ABI int32_t webapi_header_length(int64_t req,const char *name,uint64_t *length) {
    (void)req; return !name || !length ? WEBAPI_INVALID_ARGUMENT : WEBAPI_REQUEST_NOT_FOUND;
}
static ABI int32_t webapi_header_value(int64_t req,const char *name,char *value,uint64_t size) {
    (void)req; return !name || !value || !size ? WEBAPI_INVALID_ARGUMENT : WEBAPI_REQUEST_NOT_FOUND;
}
static ABI int32_t webapi_pool_stats(int32_t lib,uint64_t *stats) {
    if (!stats) return WEBAPI_INVALID_ARGUMENT;
    if (lib>=0x8000) return WEBAPI_INVALID_LIB_CONTEXT_ID;
    memset(stats,0,24); return 0;
}

/* ---- NpMatching2 (P np_matching2.cpp, S) ----
 * sceNpMatching2CreateContext(param{NpId *, commId *, passphrase *, u32 label, u64 size=0x28},
 * ctxId *): null/size/ctxId INVALID_ARGUMENT, else SIGNED_OUT (A form: size 0x10 at +8).
 * Every call that names a context (ctxId first, u16): CreateJoinRoom, JoinRoom, KickoutRoomMember,
 * SendRoomMessage, SendRoomChatMessage, SetUserInfo, GetUserInfoList[A], GetRoomDataInternal,
 * GetRoomMemberDataInternal, SetRoomMemberDataInternal, JoinLobby, LeaveLobby, GetLobbyInfoList,
 * GetLobbyMemberDataInternalList, Register Lobby{Event,Message}Callback, SetSignalingOptParam,
 * SignalingGet/Cancel PeerNetInfo[Result], SignalingGetConnectionInfo[A],
 * GetRoomMemberIdListLocal, GetRoomPasswordLocal, GetSignalingOptParamLocal:
 * INVALID_CONTEXT_ID (matching2_no_context). Argument lists: see P (ctx, request, optParam,
 * *requestId) for the request calls; the Local and PeerNetInfo lists are NOT_VERIFIED (P has none).
 * sceNpMatching2GetMemoryInfo(info *) / GetSslMemoryInfo(info *): library memory statistics: OK as
 * P, nothing written (struct layout NOT_VERIFIED).
 * C: eboot 0xa7e1e1 creates the context (CreateContextA) right after Signaling and Rudp init and
 * jumps to cleanup on any non-zero result, so every call above is unreachable offline. */
static ABI int32_t matching2_context(const uint8_t *param,uint16_t *ctx) {
    if (!param || !ctx) return MATCHING2_INVALID_ARGUMENT;
    uint64_t size; memcpy(&size,param+32,8);
    return size!=0x28 ? MATCHING2_INVALID_ARGUMENT : NP_SIGNED_OUT;
}

/* ---- NpSignaling (P np_signaling.cpp; C: eboot 0xa7e0c2 Initialize(0x20000, 0x100, 0x3f,
 * 0x4000) must return 0 or the setup is dropped; CreateContextA (0xa7e4f0) non-zero also drops) ----
 * sceNpSignalingInitialize(memSize, priority, affinity, stackSize): the library starts local
 * threads and needs no PSN: OK; again ALREADY_INITIALIZED. sceNpSignalingTerminate(): OK, else
 * NOT_INITIALIZED. sceNpSignalingCreateContextA(user, callback, arg, ctxId *): NOT_INITIALIZED,
 * then user -1 or null ctxId INVALID_ARGUMENT, then the user's NpId: SIGNED_OUT (P order).
 * Every other call (GetConnectionStatus, Activate/Deactivate/TerminateConnection, GetConnection
 * FromPeerAddressA: ctxId first) names a context that cannot exist: NOT_INITIALIZED first, then
 * CTX_NOT_FOUND. DeleteContext(ctx): CTX_NOT_FOUND (P erases silently and returns OK; an id never
 * issued is not OK here).
 * Argument lists: P. */
static _Atomic int signaling_up;
static ABI int32_t signaling_init(int64_t mem,int32_t priority,int32_t affinity,int64_t stack) {
    (void)mem; (void)priority; (void)affinity; (void)stack;
    int none=0;
    return atomic_compare_exchange_strong(&signaling_up,&none,1) ? 0 : SIGNALING_ALREADY_INITIALIZED;
}
static ABI int32_t signaling_term(void) {
    int one=1;
    return atomic_compare_exchange_strong(&signaling_up,&one,0) ? 0 : SIGNALING_NOT_INITIALIZED;
}
static ABI int32_t signaling_context_a(int32_t user,void *cb,void *arg,int32_t *ctx) {
    (void)cb; (void)arg;
    if (!atomic_load(&signaling_up)) return SIGNALING_NOT_INITIALIZED;
    return user==USER_INVALID || !ctx ? SIGNALING_INVALID_ARGUMENT : NP_SIGNED_OUT;
}
static ABI int32_t signaling_no_context(void) { return atomic_load(&signaling_up) ? SIGNALING_CTX_NOT_FOUND : SIGNALING_NOT_INITIALIZED; }

/* ---- Rudp (R cellRudp.h error codes; arguments from the PS3/PS4 SDK API, C: eboot 0xa7e1a3
 * Init(pool, size) after SignalingInitialize, result tested < 0). P only has logging stubs
 * (OK), not copied. All contracts NOT_VERIFIED for PS4; local-only state follows the
 * Signaling model: no socket can exist (sceNetSocket fails), so nothing binds.
 * sceRudpInit(memPool, int memSize): null/<=0 INVALID_ARGUMENT, again ALREADY_INITIALIZED, else OK.
 * sceRudpEnd(): OK, else NOT_INITIALIZED. sceRudpEnableInternalIOThread(stack, priority) and
 * sceRudpSetEventHandler(handler, arg): NOT_INITIALIZED, else OK (local). sceRudpCreateContext(
 * handler, arg, int *ctxId): NOT_INITIALIZED, null ctxId INVALID_ARGUMENT, else a local id (16
 * at most, TOO_MANY_CONTEXTS). sceRudpBind(ctx, sockId, vport, muxMode): INVALID_CONTEXT_ID, else
 * INVALID_SOCKET. sceRudpTerminate(ctx): frees it, else INVALID_CONTEXT_ID. Initiate(ctx, addr,
 * len, vport), Write(ctx, data, len, flags), Read(ctx, buf, len, flags, ...), SetOption(ctx,
 * option, value, len), GetContextStatus(ctx, status *), GetSizeReadable(ctx),
 * GetNumberOfPacketsToRead(ctx): INVALID_CONTEXT_ID, else NOT_BOUND. */
#define RUDP_CONTEXTS 16
static _Atomic int rudp_up;
static _Atomic int32_t rudp_ctx[RUDP_CONTEXTS]; /* 0 free, else id */
static _Atomic int32_t rudp_next=1;
static ABI int32_t rudp_init(const void *pool,int32_t size) {
    if (!pool || size<=0) return RUDP_INVALID_ARGUMENT;
    int none=0;
    return atomic_compare_exchange_strong(&rudp_up,&none,1) ? 0 : RUDP_ALREADY_INITIALIZED;
}
static ABI int32_t rudp_end(void) {
    int one=1;
    if (!atomic_compare_exchange_strong(&rudp_up,&one,0)) return RUDP_NOT_INITIALIZED;
    for (int i=0;i<RUDP_CONTEXTS;i++) atomic_store(&rudp_ctx[i],0);
    return 0;
}
static ABI int32_t rudp_local(void) { return atomic_load(&rudp_up) ? 0 : RUDP_NOT_INITIALIZED; }
static ABI int32_t rudp_create(void *handler,void *arg,int32_t *ctx) {
    (void)handler; (void)arg;
    if (!atomic_load(&rudp_up)) return RUDP_NOT_INITIALIZED;
    if (!ctx) return RUDP_INVALID_ARGUMENT;
    for (int i=0;i<RUDP_CONTEXTS;i++) {
        int32_t none=0, id=atomic_fetch_add(&rudp_next,1);
        if (atomic_compare_exchange_strong(&rudp_ctx[i],&none,id)) { *ctx=id; return 0; }
    }
    return RUDP_TOO_MANY_CONTEXTS;
}
static int rudp_known(int32_t id) {
    for (int i=0;i<RUDP_CONTEXTS;i++) if (id && atomic_load(&rudp_ctx[i])==id) return 1;
    return 0;
}
static ABI int32_t rudp_bind(int32_t ctx,int32_t sock,uint8_t vport,uint8_t mux) {
    (void)sock; (void)vport; (void)mux;
    if (!atomic_load(&rudp_up)) return RUDP_NOT_INITIALIZED;
    return rudp_known(ctx) ? RUDP_INVALID_SOCKET : RUDP_INVALID_CONTEXT_ID;
}
static ABI int32_t rudp_terminate(int32_t ctx) {
    if (!atomic_load(&rudp_up)) return RUDP_NOT_INITIALIZED;
    for (int i=0;i<RUDP_CONTEXTS;i++) {
        int32_t id=ctx;
        if (id && atomic_compare_exchange_strong(&rudp_ctx[i],&id,0)) return 0;
    }
    return RUDP_INVALID_CONTEXT_ID;
}
static ABI int32_t rudp_unbound(int32_t ctx) {
    if (!atomic_load(&rudp_up)) return RUDP_NOT_INITIALIZED;
    return rudp_known(ctx) ? RUDP_NOT_BOUND : RUDP_INVALID_CONTEXT_ID;
}

/* ---- NetCtlForNpToolkit (P netctl.cpp; same offline state as sceNetCtl*) ----
 * RegisterCallbackForNpToolkit(cb, arg, int *cid): null cb or cid INVALID_ADDR, else an id; no
 * state change ever follows. Unregister(cid) and CheckCallback(): OK. */
static ABI int32_t netctl_toolkit_register(void *cb,void *arg,int32_t *cid) {
    (void)arg;
    if (!cb || !cid) return NET_CTL_INVALID_ADDR;
    *cid=new_object(); return 0;
}

/* ---- Dialogs. Replace the enum and name table at the top of the dialog block with (SigninDialog
 * is already in runtime_np.c, added while this draft was written, so it is not repeated):
 *   enum { SIGNIN, INVITATION, ERROR_DIALOG, SOCIAL_SCREEN, FRIEND_LIST, PROFILE, GAME_CUSTOM, DIALOGS };
 *   ... dialog_names[DIALOGS]={"SigninDialog","InvitationDialog","ErrorDialog","SocialScreenDialog",
 *                              "NpFriendListDialog","NpProfileDialog","GameCustomDataDialog"};
 * ----
 * sceNpProfileDialog* (P np_profile_dialog.cpp): Initialize, OpenA(param), UpdateStatus, Close,
 * Terminate follow the friend-list dialog; GetResult(result{s32 result, s32 userAction, void *userData, reserved[32]}):
 * result SIGNED_OUT, userAction USER_CANCELED, userData kept (as invitation_result). C: eboot
 * 0xef6aca..0xef6be0: status 3 then GetResult (result only tested < 0), 0 clears its flag.
 * sceGameCustomDataDialog* (P game_custom_data_dialog.cpp): Initialize, Open(A)(param),
 * UpdateStatus, Terminate (no Close in the import list); GetResult(A)(result{callbackArg,
 * errorCode, result, sentUsers *, reserved[32]}) exactly as invitation_result (SIGNED_OUT,
 * USER_CANCELED, no users). sceInvitationDialogOpen / GetResult (non-A) are invitation_open /
 * invitation_result: the sentOnlineIds pointer at +16 is zeroed like sentUsers. */
static ABI int32_t profile_init(void) { return dialog_init(PROFILE); }
static ABI int32_t profile_open(const void *param) { (void)param; return dialog_open(PROFILE,""); }
static ABI int32_t profile_status(void) { return atomic_load(&dialog_status[PROFILE]); }
static ABI int32_t profile_result(uint8_t *result) {
    int32_t r=dialog_result(PROFILE,result);
    if (!r) { int32_t codes[2]={NP_SIGNED_OUT,RESULT_USER_CANCELED}; memcpy(result,codes,8); }
    return r;
}
static ABI int32_t profile_close(void) { return atomic_load(&dialog_status[PROFILE]) ? 0 : DIALOG_NOT_INITIALIZED; }
static ABI int32_t profile_term(void) { return dialog_term(PROFILE); }
static ABI int32_t custom_init(void) { return dialog_init(GAME_CUSTOM); }
static ABI int32_t custom_open(const void *param) { (void)param; return dialog_open(GAME_CUSTOM,""); }
static ABI int32_t custom_status(void) { return atomic_load(&dialog_status[GAME_CUSTOM]); }
static ABI int32_t custom_result(uint8_t *result) {
    int32_t r=dialog_result(GAME_CUSTOM,result);
    if (r) return r;
    int32_t codes[2]={NP_SIGNED_OUT,RESULT_USER_CANCELED};
    memcpy(result+8,codes,8); memset(result+16,0,40);
    return 0;
}
static ABI int32_t custom_term(void) { return dialog_term(GAME_CUSTOM); }

static const RuntimeExport exports[]={
    {"sceNpTusCreateNpTitleCtxA",signed_out}, {"sceNpTssCreateNpTitleCtxA",signed_out},
    {"sceNpTusCreateRequest",community_no_id}, {"sceNpTusDeleteRequest",community_no_id},
    {"sceNpTusAbortRequest",community_no_id}, {"sceNpTusDeleteNpTitleCtx",community_no_id},

    /* The asynchronous forms name the same requests that cannot exist, and so does polling one. */
    {"sceNpTssGetDataAsync",community_no_id}, {"sceNpTusPollAsync",community_no_id},
    {"sceNpTusAddAndGetVariableAAsync",community_no_id}, {"sceNpTusAddAndGetVariableAVUserAsync",community_no_id},
    {"sceNpTusGetDataAAsync",community_no_id}, {"sceNpTusGetDataAVUserAsync",community_no_id},
    {"sceNpTusGetMultiSlotVariableAAsync",community_no_id}, {"sceNpTusGetMultiSlotVariableAVUserAsync",community_no_id},
    {"sceNpTusSetDataAAsync",community_no_id}, {"sceNpTusSetDataAVUserAsync",community_no_id},
    {"sceNpScoreCreateNpTitleCtxA",score_ctx_a},

    /* Asynchronous ranking and record calls fail as the synchronous ones; nothing is in flight to poll. */
    {"sceNpScoreGetBoardInfoAsync",signed_out}, {"sceNpScoreGetFriendsRankingAAsync",signed_out},
    {"sceNpScoreGetGameDataByAccountIdAsync",signed_out}, {"sceNpScoreGetRankingByAccountIdAsync",signed_out},
    {"sceNpScoreGetRankingByRangeAAsync",signed_out}, {"sceNpScoreRecordGameDataAsync",signed_out},
    {"sceNpScoreRecordScoreAsync",signed_out}, {"sceNpScorePollAsync",community_no_id},
    {"sceNpMatching2CreateContextA",matching2_context_a},
    {"sceNpMatching2CreateJoinRoomA",matching2_no_context}, {"sceNpMatching2JoinRoomA",matching2_no_context},

    {"sceNpMatching2GetRoomDataExternalList",matching2_no_context},

    {"sceNpMatching2GetRoomMemberDataExternalList",matching2_no_context},

    {"sceNpMatching2RegisterRoomMessageCallback",matching2_no_context},

    {"sceNpWebApiCreateContextA",webapi_context_a}, {"sceNpWebApiCreateHandle",webapi_handle},
    {"sceNpWebApiDeleteHandle",ok}, {"sceNpWebApiSendRequest2",signed_out},

    {"sceNpWebApiCreateExtdPushEventFilter",webapi_filter}, {"sceNpWebApiDeleteExtdPushEventFilter",ok},
    {"sceNpWebApiRegisterExtdPushEventCallbackA",webapi_callback_a},
    {"sceNpWebApiUnregisterExtdPushEventCallback",ok},
    {"sceNpWordFilterCreateTitleCtxA",signed_out}, {"sceNpLookupCreateTitleCtxA",signed_out}, /* (user): needs the user's NpId, as the other title contexts */
    {"sceNpWordFilterDeleteTitleCtx",community_no_id},
    {"sceNpWordFilterDeleteRequest",community_no_id},
    {"sceNpWordFilterAbortRequest",community_no_id},
    {"sceNpWordFilterCensorComment",community_no_id}, {"sceNpWordFilterCreateAsyncRequest",community_no_id},
    {"sceNpWordFilterPollAsync",community_no_id},

    {"sceNpInGameMessageInitialize",object}, {"sceNpInGameMessageTerminate",ok},
    {"sceNpInGameMessageCreateHandle",object}, {"sceNpInGameMessageDeleteHandle",ok},
    {"sceNpInGameMessageAbortHandle",ok},
    {"sceNpInGameMessagePrepareA",signed_out}, {"sceNpInGameMessageSendDataA",signed_out},

    {"sceNpAuthCreateRequest",auth_request}, {"sceNpAuthGetAuthorizationCodeA",auth_code_a},
    {"sceNpAuthAbortRequest",ok},

    {"sceInvitationDialogInitialize",invitation_init}, {"sceInvitationDialogOpenA",invitation_open},
    {"sceInvitationDialogUpdateStatus",invitation_status}, {"sceInvitationDialogGetResultA",invitation_result},
    {"sceInvitationDialogTerminate",invitation_term}, {"sceInvitationDialogClose",invitation_close},
    {"sceSigninDialogInitialize",signin_init}, {"sceSigninDialogOpen",signin_open},
    {"sceSigninDialogUpdateStatus",signin_status}, {"sceSigninDialogGetResult",signin_result},
    {"sceSigninDialogTerminate",signin_term}, {"sceSigninDialogClose",signin_close},
    {"sceErrorDialogInitialize",error_init}, {"sceErrorDialogOpen",error_open},
    {"sceErrorDialogUpdateStatus",error_status}, {"sceErrorDialogTerminate",error_term},
    {"sceErrorDialogClose",error_close},
    {"sceNpFriendListDialogInitialize",friends_init}, {"sceNpFriendListDialogOpenA",friends_open},
    {"sceNpFriendListDialogUpdateStatus",friends_status}, {"sceNpFriendListDialogGetResultA",friends_result},
    {"sceNpFriendListDialogClose",friends_close}, {"sceNpFriendListDialogTerminate",friends_term},
    {"sceNpMatching2GetLobbyMemberDataInternalList",matching2_no_context},
    {"sceNpMatching2RegisterLobbyEventCallback",matching2_no_context},
    {"sceNpMatching2GetRoomMemberDataInternal",matching2_no_context},
    {"sceNpMatching2GetSslMemoryInfo",ok},
    {"sceNpMatching2KickoutRoomMember",matching2_no_context},
    {"sceNpMatching2LeaveLobby",matching2_no_context},
    {"sceNpMatching2JoinRoom",matching2_no_context},
    {"sceNpMatching2SignalingGetPeerNetInfoResult",matching2_no_context},
    {"sceNpMatching2RegisterLobbyMessageCallback",matching2_no_context},
    {"sceNpMatching2SetSignalingOptParam",matching2_no_context},
    {"sceNpMatching2SignalingCancelPeerNetInfo",matching2_no_context},
    {"sceNpMatching2GetUserInfoListA",matching2_no_context},
    {"sceNpMatching2SetRoomMemberDataInternal",matching2_no_context},
    {"sceNpMatching2SendRoomMessage",matching2_no_context},
    {"sceNpMatching2GetRoomDataInternal",matching2_no_context},
    {"sceNpMatching2GetRoomMemberIdListLocal",matching2_no_context},
    {"sceNpMatching2CreateContext",matching2_context},
    {"sceNpMatching2GetSignalingOptParamLocal",matching2_no_context},
    {"sceNpMatching2GetMemoryInfo",ok},
    {"sceNpMatching2SetUserInfo",matching2_no_context},
    {"sceNpMatching2JoinLobby",matching2_no_context},
    {"sceNpMatching2SignalingGetConnectionInfoA",matching2_no_context},
    {"sceNpMatching2SendRoomChatMessage",matching2_no_context},
    {"sceNpMatching2GetUserInfoList",matching2_no_context},
    {"sceNpMatching2SignalingGetConnectionInfo",matching2_no_context},
    {"sceNpMatching2GetRoomPasswordLocal",matching2_no_context},
    {"sceNpMatching2GetLobbyInfoList",matching2_no_context},
    {"sceNpMatching2CreateJoinRoom",matching2_no_context},
    {"sceNpScoreGetRankingByAccountIdForCrossSave",signed_out},
    {"sceNpScoreGetFriendsRankingAsync",signed_out},
    {"sceNpScoreGetFriendsRanking",signed_out},
    {"sceNpScoreGetRankingByNpId",signed_out},
    {"sceNpScoreGetFriendsRankingForCrossSave",signed_out},
    {"sceNpScoreGetGameDataAsync",signed_out},
    {"sceNpScoreGetRankingByAccountId",signed_out},
    {"sceNpScoreGetRankingByRange",signed_out},
    {"sceNpScoreCreateNpTitleCtx",score_ctx},
    {"sceNpScoreGetGameDataByAccountId",signed_out},
    {"sceNpScoreGetBoardInfo",signed_out},
    {"sceNpScoreGetRankingByRangeA",signed_out},
    {"sceNpScoreGetRankingByNpIdAsync",signed_out},
    {"sceNpScoreRecordGameData",signed_out},
    {"sceNpScoreSetPlayerCharacterId",score_pc_id},
    {"sceNpScoreGetFriendsRankingA",signed_out},
    {"sceNpScoreGetRankingByRangeForCrossSave",signed_out},
    {"sceNpScoreGetRankingByAccountIdPcIdForCrossSave",signed_out},
    {"sceNpScoreGetRankingByRangeAsync",signed_out},
    {"sceNpScoreGetRankingByAccountIdPcId",signed_out},
    {"sceNpScoreGetGameData",signed_out},
    {"sceNpScoreRecordScore",signed_out},
    {"sceNpTusSetDataVUser",community_no_id},
    {"sceNpTusGetDataForCrossSaveVUser",community_no_id},
    {"sceNpTssGetData",community_no_id},
    {"sceNpTusDeleteMultiSlotData",community_no_id},
    {"sceNpTusTryAndSetVariableA",community_no_id},
    {"sceNpTusSetMultiSlotVariableVUser",community_no_id},
    {"sceNpTusGetDataForCrossSave",community_no_id},
    {"sceNpTusSetData",community_no_id},
    {"sceNpTusAddAndGetVariableForCrossSaveVUser",community_no_id},
    {"sceNpTusGetMultiSlotVariableAVUser",community_no_id},
    {"sceNpTusCreateNpTitleCtx",tus_ctx},
    {"sceNpTusGetFriendsVariableA",community_no_id},
    {"sceNpTusGetMultiUserVariableForCrossSaveVUser",community_no_id},
    {"sceNpTusAddAndGetVariableForCrossSave",community_no_id},
    {"sceNpTusGetMultiSlotVariable",community_no_id},
    {"sceNpTusGetMultiSlotVariableA",community_no_id},
    {"sceNpTusGetDataVUser",community_no_id},
    {"sceNpTusGetMultiUserVariableA",community_no_id},
    {"sceNpTusTryAndSetVariableForCrossSave",community_no_id},
    {"sceNpTusAddAndGetVariableAVUser",community_no_id},
    {"sceNpTusGetMultiUserVariableForCrossSave",community_no_id},
    {"sceNpTusGetFriendsDataStatusForCrossSave",community_no_id},
    {"sceNpTusSetDataA",community_no_id},
    {"sceNpTusGetData",community_no_id},
    {"sceNpTusSetMultiSlotVariable",community_no_id},
    {"sceNpTusAddAndGetVariable",community_no_id},
    {"sceNpTusGetMultiUserDataStatusForCrossSave",community_no_id},
    {"sceNpTusSetMultiSlotVariableA",community_no_id},
    {"sceNpTusGetMultiUserDataStatusAVUser",community_no_id},
    {"sceNpTusGetMultiUserVariableAVUser",community_no_id},
    {"sceNpTusTryAndSetVariableForCrossSaveVUser",community_no_id},
    {"sceNpTusDeleteMultiSlotDataA",community_no_id},
    {"sceNpTusGetDataAVUser",community_no_id},
    {"sceNpTusSetDataAVUser",community_no_id},
    {"sceNpTusGetMultiUserDataStatusA",community_no_id},
    {"sceNpTusGetMultiSlotVariableForCrossSave",community_no_id},
    {"sceNpTusGetFriendsVariableForCrossSave",community_no_id},
    {"sceNpTusTryAndSetVariableAVUser",community_no_id},
    {"sceNpTusGetMultiSlotVariableVUser",community_no_id},
    {"sceNpTusAddAndGetVariableVUser",community_no_id},
    {"sceNpTusGetMultiUserDataStatusForCrossSaveVUser",community_no_id},
    {"sceNpTusAddAndGetVariableA",community_no_id},
    {"sceNpTusGetMultiSlotVariableForCrossSaveVUser",community_no_id},
    {"sceNpTusDeleteMultiSlotDataVUser",community_no_id},
    {"sceNpTusGetDataA",community_no_id},
    {"sceNpTusGetFriendsDataStatusA",community_no_id},
    {"sceNpLookupCreateTitleCtx",utility_ctx},
    {"sceNpWordFilterCreateTitleCtx",utility_ctx},
    {"sceNpBandwidthTestInitStart",signed_out},
    {"sceNpBandwidthTestGetStatus",signed_out},
    {"sceNpBandwidthTestShutdown",signed_out},
    {"sceNpLookupCreateRequest",community_no_id},
    {"sceNpLookupDeleteTitleCtx",community_no_id},
    {"sceNpLookupDeleteRequest",community_no_id},
    {"sceNpLookupNpId",community_no_id},
    {"sceNpLookupPollAsync",community_no_id},
    {"sceNpWordFilterCreateRequest",community_no_id},
    {"sceNpWordFilterSanitizeComment",community_no_id},
    {"sceNpBandwidthTestAbort",ok},
    {"sceNpInGameMessageGetMemoryPoolStatistics",ok},
    {"sceNpInGameMessagePrepare",signed_out},
    {"sceNpInGameMessageSendData",signed_out},
    {"sceNpRegisterNpReachabilityStateCallback",reachability_register},
    {"sceNpRegisterStateCallbackForToolkit",toolkit_register},
    {"sceNpUnregisterStateCallbackForToolkit",toolkit_unregister},
    {"sceNpCheckCallbackForLib",ok},
    {"sceNpCmpNpId",np_cmp_id},
    {"sceNpAuthCreateAsyncRequest",auth_async_request},
    {"sceNpAuthGetAuthorizationCode",auth_code_a},
    {"sceNpAuthGetIdTokenA",auth_token_a},
    {"sceNpAuthPollAsync",auth_poll},
    {"sceNpCommerceShowPsStoreIcon",ok},
    {"sceNpCommerceHidePsStoreIcon",ok},
    {"sceNpWebApiCreateContext",webapi_context},
    {"sceNpWebApiCreatePushEventFilter",webapi_plain_filter},
    {"sceNpWebApiCreateServicePushEventFilter",webapi_service_filter},
    {"sceNpWebApiDeletePushEventFilter",ok},
    {"sceNpWebApiDeleteServicePushEventFilter",ok},
    {"sceNpWebApiUnregisterPushEventCallback",ok},
    {"sceNpWebApiUnregisterServicePushEventCallback",ok},
    {"sceNpWebApiRegisterPushEventCallback",webapi_callback_a},
    {"sceNpWebApiRegisterServicePushEventCallback",webapi_callback_a},
    {"sceNpWebApiAddHttpRequestHeader",webapi_add_header},
    {"sceNpWebApiGetHttpResponseHeaderValueLength",webapi_header_length},
    {"sceNpWebApiGetHttpResponseHeaderValue",webapi_header_value},
    {"sceNpWebApiGetMemoryPoolStats",webapi_pool_stats},
    {"sceNpSignalingInitialize",signaling_init},
    {"sceNpSignalingTerminate",signaling_term},
    {"sceNpSignalingCreateContextA",signaling_context_a},
    {"sceNpSignalingDeactivateConnection",signaling_no_context},
    {"sceNpSignalingGetConnectionFromPeerAddressA",signaling_no_context},
    {"sceNpSignalingActivateConnectionA",signaling_no_context},
    {"sceNpSignalingTerminateConnection",signaling_no_context},
    {"sceNpSignalingGetConnectionStatus",signaling_no_context},
    {"sceNpSignalingDeleteContext",signaling_no_context},
    {"sceRudpInit",rudp_init},
    {"sceRudpEnd",rudp_end},
    {"sceRudpEnableInternalIOThread",rudp_local},
    {"sceRudpSetEventHandler",rudp_local},
    {"sceRudpCreateContext",rudp_create},
    {"sceRudpBind",rudp_bind},
    {"sceRudpTerminate",rudp_terminate},
    {"sceRudpWrite",rudp_unbound},
    {"sceRudpRead",rudp_unbound},
    {"sceRudpSetOption",rudp_unbound},
    {"sceRudpGetContextStatus",rudp_unbound},
    {"sceRudpGetSizeReadable",rudp_unbound},
    {"sceRudpGetNumberOfPacketsToRead",rudp_unbound},
    {"sceRudpInitiate",rudp_unbound},
    {"sceNetCtlRegisterCallbackForNpToolkit",netctl_toolkit_register},
    {"sceNetCtlUnregisterCallbackForNpToolkit",ok},
    {"sceNetCtlCheckCallbackForNpToolkit",ok},
    {"sceNpProfileDialogInitialize",profile_init},
    {"sceNpProfileDialogOpenA",profile_open},
    {"sceNpProfileDialogUpdateStatus",profile_status},
    {"sceNpProfileDialogGetResult",profile_result},
    {"sceNpProfileDialogClose",profile_close},
    {"sceNpProfileDialogTerminate",profile_term},
    {"sceGameCustomDataDialogInitialize",custom_init},
    {"sceGameCustomDataDialogOpen",custom_open},
    {"sceGameCustomDataDialogOpenA",custom_open},
    {"sceGameCustomDataDialogUpdateStatus",custom_status},
    {"sceGameCustomDataDialogGetResult",custom_result},
    {"sceGameCustomDataDialogGetResultA",custom_result},
    {"sceGameCustomDataDialogTerminate",custom_term},
    {"sceInvitationDialogOpen",invitation_open},
    {"sceInvitationDialogGetResult",invitation_result},
};
uintptr_t runtime_np_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
