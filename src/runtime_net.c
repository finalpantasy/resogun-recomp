/* Networking for an offline console (the cable is unplugged, as in runtime_services.c): the
 * libSceHttp calls runtime_services.c does not have create objects, and no request ever gets a
 * response (HTTP NETWORK); the Second Screen companion server never sees a client. */
#include <stdio.h>
#include "runtime.h"
#include <ctype.h>
#include <stdatomic.h>
#include <string.h>

/* ---- libSceHttp (shadPS4 http.cpp), offline ---- */
#define HTTP_NETWORK ((int32_t)0x80431063)
#define HTTP_UNKNOWN_SCHEME ((int32_t)0x80431061)
#define HTTP_INVALID_VALUE ((int32_t)0x804311fe)
static _Atomic int32_t http_next=0x10000;
static int same(const char *a,const char *b) {
    while (*a && tolower((unsigned char)*a)==*b) ++a, ++b;
    return !*a && !*b;
}
/* sceHttpCreateConnection(template, server, scheme, port, keepalive): an id; the server is
 * required and the scheme is http or https. */
static ABI int32_t http_connection(int32_t tmpl,const char *server,const char *scheme,uint16_t port,int32_t keepalive) {
    (void)tmpl; (void)port; (void)keepalive;
    if (!server) return HTTP_INVALID_VALUE;
    if (!scheme || (!same(scheme,"http") && !same(scheme,"https"))) return HTTP_UNKNOWN_SCHEME;
    return atomic_fetch_add(&http_next,1);
}
/* sceHttpsEnableOption(id, ssl flags) (NID f42K37mm5RM; Golf passes its connection id and 0x18 and
 * ignores the result): accepted. The flags choose TLS certificate checks, and no handshake happens. */
static ABI int32_t https_option(int32_t id,uint32_t flags) { (void)id; (void)flags; return 0; }
/* sceHttpCreateConnectionWithURL(template, url, keepalive) (NID qgxDBjorUxs; Golf passes its template
 * id, a URL string and 1, and keeps a result >= 0 as the connection id): an id; the URL is required
 * and its scheme is http or https, as for sceHttpCreateConnection. Nothing connects. */
static ABI int32_t http_connection_url(int32_t tmpl,const char *url,int32_t keepalive) {
    (void)tmpl; (void)keepalive;
    if (!url) return HTTP_INVALID_VALUE;
    const char *colon=strstr(url,"://");
    size_t n=colon ? (size_t)(colon-url) : 0;
    char scheme[8];
    if (!n || n>=sizeof scheme) return HTTP_UNKNOWN_SCHEME;
    memcpy(scheme,url,n); scheme[n]=0;
    if (!same(scheme,"http") && !same(scheme,"https")) return HTTP_UNKNOWN_SCHEME;
    printf("Runtime: http connection to %s (nothing connects)%c",url,10);
    return atomic_fetch_add(&http_next,1);
}
/* sceHttpCreateRequest2(connection, method, path, content length): an id; the path is required. */
static ABI int32_t http_request2(int32_t conn,const char *method,const char *path,uint64_t length) {
    (void)conn; (void)method; (void)length;
    if (!path) return HTTP_INVALID_VALUE;
    return atomic_fetch_add(&http_next,1);
}
/* sceHttpCreateRequestWithURL(connection, method, url, content length) (NID Aeu5wVKkF9w; Golf at
 * 0xec9174 passes method 0..2 from a table and keeps a result >= 0 as the request id): an id; the
 * URL is required. The URL is logged: it is the game server's contract. Nothing is sent. */
static ABI int32_t http_request_url(int32_t conn,int32_t method,const char *url,uint64_t length) {
    (void)conn;
    if (!url) return HTTP_INVALID_VALUE;
    printf("Runtime: http request method %d, %llu body bytes: %s (not sent)\n",method,(unsigned long long)length,url);
    return atomic_fetch_add(&http_next,1);
}
/* sceHttpGetAllResponseHeaders(request, header, size): no request gets a response. */
static ABI int32_t http_headers(int32_t req,char **header,uint64_t *size) {
    (void)req;
    if (!header || !size) return HTTP_INVALID_VALUE;
    return HTTP_NETWORK;
}
/* Timeouts and aborting a request: accepted, as shadPS4 does; nothing is sent. */
static ABI int32_t http_ok(void) { return 0; }

static ABI int32_t net_init(void) { return 0; }
/* sceHttpsSetSslCallback(id, callback, user argument) (NID htyBOoWeS58; Golf passes its template id,
 * a function and its object, and tests the result for < 0): accepted. The callback reports a TLS
 * handshake, and no request is ever sent, so it is never called. */
static ABI int32_t https_ssl_callback(int32_t id,void *cb,void *arg) { (void)id; (void)cb; (void)arg; return 0; }

/* libSceCompanionHttpd: the HTTP server for the PS4 Second Screen companion app. It starts and
 * accepts handlers, but no companion app can reach it, so no event ever arrives: GetEvent reports
 * none (the game only tests for nonzero; it reacts to 0x10000001 connected / 0x10000002 gone). */
#define COMPANION_NO_EVENT ((int32_t)0x80E40008)
static ABI int32_t companion_no_event(void *event) { (void)event; return COMPANION_NO_EVENT; }

static const RuntimeExport exports[]={
    {"sceNetInit",net_init},
    {"sceHttpsSetSslCallback",https_ssl_callback},
    {"sceHttpCreateConnectionWithURL",http_connection_url},
    {"sceHttpCreateRequestWithURL",http_request_url},
    {"sceHttpsDisableOption",https_option}, {"sceHttpSetChunkedTransferEnabled",http_ok},
    {"sceHttpsEnableOption",https_option},
    {"sceCompanionHttpdInitialize2",http_ok}, {"sceCompanionHttpdTerminate",http_ok},
    {"sceCompanionHttpdStart",http_ok}, {"sceCompanionHttpdStop",http_ok},
    {"sceCompanionHttpdRegisterRequestCallback",http_ok}, {"sceCompanionHttpdUnregisterRequestCallback",http_ok},
    {"sceCompanionHttpdAddHeader",http_ok}, {"sceCompanionHttpdSetBody",http_ok},
    {"sceCompanionHttpdSetStatus",http_ok}, {"sceCompanionHttpdGetEvent",companion_no_event},

    {"sceHttpCreateConnection",http_connection}, {"sceHttpCreateRequest2",http_request2},
    {"sceHttpGetAllResponseHeaders",http_headers},
    {"sceHttpAbortRequest",http_ok},
    {"sceHttpSetSendTimeOut",http_ok}, {"sceHttpSetResolveTimeOut",http_ok},
    {"sceHttpSetRecvTimeOut",http_ok},
};
uintptr_t runtime_net_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
