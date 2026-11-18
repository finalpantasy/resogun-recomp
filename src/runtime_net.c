/* Networking for an offline console (the cable is unplugged, as in runtime_services.c): the
 * libSceHttp normally reports HTTP NETWORK. Golf's discovery and selected title routes
 * can replay over opt-in IPv4 loopback. The Second Screen server has no client. */
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include "runtime.h"
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

/* ---- libSceHttp (shadPS4 http.cpp), offline ---- */
#define HTTP_NETWORK ((int32_t)0x80431063)
#define HTTP_UNKNOWN_SCHEME ((int32_t)0x80431061)
#define HTTP_INVALID_VALUE ((int32_t)0x804311fe)
static _Atomic int32_t http_next=0x10000;

/* Only these guest URLs can replay over the literal loopback socket. */
#define DISCOVERY_PATH "/nmg-us/ss.info"
#define SERVER_TIME_PATH "/nmg-us/replay-probe/gameapi_getservertime"
#define EULA_PATH "/nmg-us/replay-probe/gameapi_geteula"
#define NOTICE_PATH "/nmg-us/replay-probe/gameapi_getinfolist"
#define DAILY_SCORE_PATH "/nmg-us/replay-probe/gameapi_getscorelist"
#define BOARD_LIST_PATH "/nmg-us/replay-probe/gameapi_getboardlist"
#define PROPERTY_INFO_PATH "/nmg-us/replay-probe/gameapi_getgamepropertyinfo"
#define TELEMETRY_PATH "/nmg-us/replay-probe/analytics/19d916d7-6a01-4371-9e6c-c91b2ff386bb/telemetry"
#define LOGIN_PREFIX "/nmg-us/replay-probe/gameapi_login?account=5932163534510096385&n="
enum { DISCOVERY_HEADER_MAX=4096 };
typedef struct {
    const char *url, *path;
    unsigned request_min, request_max, response_min, response_max, response_alignment;
    const char *content_type; /* NULL uses the title multipart format. */
} ReplayRoute;
static const ReplayRoute replay_routes[]={
    {"https://ssps4.scej-network.jp:443" DISCOVERY_PATH, DISCOVERY_PATH, 0, 0, 8, 0xa2800, 8, NULL},
    {"https://127.0.0.1:8765" SERVER_TIME_PATH, SERVER_TIME_PATH, 102, 102, 28, 28, 4, NULL},
    {"https://127.0.0.1:8765" EULA_PATH, EULA_PATH, 110, 110, 64, 60+0x8000, 4, NULL},
    {"https://127.0.0.1:8765" NOTICE_PATH, NOTICE_PATH, 394, 1394, 36, 36, 4, NULL},
    {"https://127.0.0.1:8765" DAILY_SCORE_PATH, DAILY_SCORE_PATH, 166, 258, 52, 52, 4, NULL},
    {"https://127.0.0.1:8765" BOARD_LIST_PATH, BOARD_LIST_PATH, 138, 230, 40, 40, 4, NULL},
    {"https://127.0.0.1:8765" LOGIN_PREFIX, NULL, 194, 246, 24, 24, 4, NULL},
    {"https://127.0.0.1:8765" PROPERTY_INFO_PATH, PROPERTY_INFO_PATH, 118, 118, 24, 24, 4, NULL},
    {"http://127.0.0.1:8765" TELEMETRY_PATH, TELEMETRY_PATH, 1, 8192, 0, 0, 1, "application/json"},
};
static pthread_mutex_t discovery_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {
    int32_t id;
    unsigned port;
    const ReplayRoute *route;
    int status, read;
    size_t length, request_size;
    char login_path[sizeof LOGIN_PREFIX+sizeof "-2147483648&s=1"];
    unsigned char *body;
} discovery;

static unsigned discovery_port(void) {
    const char *text=getenv("GOLF_DISCOVERY_PORT");
    if (!text || !*text || strlen(text)>5 || text[strspn(text,"0123456789")]) return 0;
    unsigned long port=strtoul(text,NULL,10);
    return port && port<=65535 ? (unsigned)port : 0;
}

#ifdef _WIN32
typedef SOCKET DiscoverySocket;
#define DISCOVERY_INVALID INVALID_SOCKET
#define discovery_close closesocket
#define DISCOVERY_SEND_FLAGS 0
#else
typedef int DiscoverySocket;
#define DISCOVERY_INVALID (-1)
#define discovery_close close
#define DISCOVERY_SEND_FLAGS MSG_NOSIGNAL
#endif

static int discovery_send(DiscoverySocket sock,const void *data,int size) {
    const char *bytes=data;
    int sent=0;
    while (sent<size) {
        int n=(int)send(sock,bytes+sent,size-sent,DISCOVERY_SEND_FLAGS);
        if (n<=0) return 0;
        sent+=n;
    }
    return 1;
}

static int discovery_exchange(const void *data,int size) {
    static const char request_format[]=
        "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%u\r\n"
        "User-Agent: PS4Application nolib/0.0 (U-UP9000-CUSA04687_00-EVERYBODYSGOLF07/2017022700/1170)\r\n"
        "%sConnection: close\r\n\r\n";
    const ReplayRoute *route=discovery.route;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2),&wsa)) return 0;
    DWORD timeout=3000;
#else
    struct timeval timeout={.tv_sec=3};
#endif
    int ok=0;
    char *response=NULL;
    DiscoverySocket sock=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (sock==DISCOVERY_INVALID) goto done;
    if (setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,(const char *)&timeout,sizeof timeout) ||
        setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,(const char *)&timeout,sizeof timeout)) goto done;
    struct sockaddr_in target={0};
    target.sin_family=AF_INET;
    target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    target.sin_port=htons((uint16_t)discovery.port);
    if (connect(sock,(const struct sockaddr *)&target,sizeof target)) goto done;

    char request[384], headers[96]="";
    if (size) {
        const char *type=route->content_type ? route->content_type : "multipart/form-data; boundary=xYzZY";
        int n=snprintf(headers,sizeof headers,"Content-Type: %s\r\nContent-Length: %d\r\n",type,size);
        if (n<0 || (size_t)n>=sizeof headers) goto done;
    }
    const char *method=size ? "POST" : "GET";
    const char *path=route->path ? route->path : discovery.login_path;
    int request_size=snprintf(request,sizeof request,request_format,method,path,discovery.port,headers);
    if (request_size<0 || (size_t)request_size>=sizeof request ||
        !discovery_send(sock,request,request_size) || !discovery_send(sock,data,size)) goto done;

    const size_t max_body=route->response_max;
    const size_t capacity=DISCOVERY_HEADER_MAX+max_body;
    response=malloc(capacity+1);
    if (!response) goto done;
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC,&start)) goto done;
    size_t received=0, header_size=0, length=0;
    int status=0;
    do {
        if (received==capacity || clock_gettime(CLOCK_MONOTONIC,&now) ||
            now.tv_sec-start.tv_sec>=4) goto done;
        int n=(int)recv(sock,response+received,(int)(capacity-received),0);
        if (n<=0) goto done;
        received+=(size_t)n;
        if (!header_size) {
            response[received]=0;
            char *end=strstr(response,"\r\n\r\n");
            if (!end) {
                if (received>=DISCOVERY_HEADER_MAX) goto done;
                continue;
            }
            header_size=(size_t)(end+4-response);
            if (header_size>DISCOVERY_HEADER_MAX ||
                sscanf(response,"HTTP/1.%*1[01] %3d",&status)!=1 ||
                status<100 || status>599 ||
                (response[12]!=' ' && response[12]!='\r')) goto done;
            *end=0;
            char *field=strstr(response,"\r\nContent-Length: ");
            if (!field || strstr(response,"\r\nTransfer-Encoding:") ||
                strstr(field+2,"\r\nContent-Length:")) goto done;
            field+=sizeof("\r\nContent-Length: ")-1;
            size_t digits=strspn(field,"0123456789");
            if (!digits || digits>6 || (field[digits] && field[digits]!='\r')) goto done;
            length=(size_t)strtoul(field,NULL,10);
            if (length>max_body || (status!=200 && length)) goto done;
            if (status==200 && (length<route->response_min || length%route->response_alignment)) goto done;
        }
    } while (!header_size || received<header_size+length);
    if (received!=header_size+length) goto done;
    if (length) memmove(response,response+header_size,length);
    discovery.body=(unsigned char *)response;
    response=NULL;
    discovery.status=status;
    discovery.length=length;
    ok=1;
done:
    free(response);
    if (sock!=DISCOVERY_INVALID) discovery_close(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return ok;
}

static ABI int32_t http_send(int32_t id,const void *data,uint64_t length) {
    pthread_mutex_lock(&discovery_lock);
    int ok=0;
    if (discovery.id && id==discovery.id && length==discovery.request_size && (!length || data)) {
        free(discovery.body);
        discovery.body=NULL;
        discovery.status=discovery.read=0;
        discovery.length=0;
        ok=discovery_exchange(data,(int)length);
        printf("Runtime: http replay request %d sent to 127.0.0.1:%u: %s, status %d, %llu body bytes\n",id,discovery.port,
               ok ? "answered" : "no valid answer",discovery.status,(unsigned long long)discovery.length);
    }
    pthread_mutex_unlock(&discovery_lock);
    return ok ? 0 : HTTP_NETWORK;
}

static ABI int32_t http_status(int32_t id,int *status) {
    if (!status) return HTTP_INVALID_VALUE;
    pthread_mutex_lock(&discovery_lock);
    int ok=id==discovery.id && discovery.status;
    if (ok) *status=discovery.status;
    pthread_mutex_unlock(&discovery_lock);
    return ok ? 0 : HTTP_NETWORK;
}

static ABI int32_t http_length(int32_t id,int *result,uint64_t *length) {
    if (!result || !length) return HTTP_INVALID_VALUE;
    pthread_mutex_lock(&discovery_lock);
    int ok=id==discovery.id && discovery.status;
    if (ok) { *result=0; *length=discovery.length; }
    pthread_mutex_unlock(&discovery_lock);
    return ok ? 0 : HTTP_NETWORK;
}

static ABI int32_t http_read(int32_t id,void *data,uint64_t capacity) {
    if (!data) return HTTP_INVALID_VALUE;
    pthread_mutex_lock(&discovery_lock);
    int32_t result=HTTP_NETWORK;
    if (id==discovery.id && discovery.status==200 && discovery.length<=capacity) {
        result=0;
        if (!discovery.read) {
            memcpy(data,discovery.body,discovery.length);
            result=(int32_t)discovery.length;
            discovery.read=1;
        }
    }
    pthread_mutex_unlock(&discovery_lock);
    return result;
}

static ABI int32_t http_delete_request(int32_t id) {
    pthread_mutex_lock(&discovery_lock);
    if (discovery.id && id==discovery.id) {
        free(discovery.body);
        memset(&discovery,0,sizeof discovery);
    }
    pthread_mutex_unlock(&discovery_lock);
    return 0;
}

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
 * URL is required. An explicitly configured discovery replay can use the returned id. */
/* The only variable target is the primary local account and a signed 32-bit counter. */
static int replay_url_matches(const ReplayRoute *route,const char *url) {
    if (route->path) return !strcmp(url,route->url);
    const size_t prefix=strlen(route->url);
    if (strncmp(url,route->url,prefix)) return 0;
    const char *counter=url+prefix;
    const char *digits=counter+(*counter=='-');
    const size_t size=strspn(digits,"0123456789");
    if (!size || size>10 || (*digits=='0' && (size!=1 || digits!=counter)) ||
        strcmp(digits+size,"&s=1")) return 0;
    const long long value=strtoll(counter,NULL,10);
    return value>=INT32_MIN && value<=INT32_MAX;
}
static ABI int32_t http_request_url(int32_t conn,int32_t method,const char *url,uint64_t length) {
    (void)conn;
    if (!url) return HTTP_INVALID_VALUE;
    const int32_t id=atomic_fetch_add(&http_next,1);
    const ReplayRoute *replay=NULL;
    for (size_t i=0;i<sizeof replay_routes/sizeof *replay_routes;++i) {
        const ReplayRoute *route=&replay_routes[i];
        if (method==(route->request_min!=0) && length>=route->request_min &&
            length<=route->request_max && (route->content_type || !((length-route->request_min)%4)) &&
            replay_url_matches(route,url)) {
            replay=route;
            break;
        }
    }
    if (replay) {
        const unsigned port=discovery_port();
        if (port) {
            pthread_mutex_lock(&discovery_lock);
            if (!discovery.id) {
                discovery.id=id;
                discovery.port=port;
                discovery.route=replay;
                discovery.request_size=(size_t)length;
                if (!replay->path)
                    strcpy(discovery.login_path,url+sizeof("https://127.0.0.1:8765")-1);
                pthread_mutex_unlock(&discovery_lock);
                printf("Runtime: http replay request %d opened: %s\n",id,url);
                return id;
            }
            pthread_mutex_unlock(&discovery_lock);
        }
    }
    /* The game retries a refused request many times a second: one line per run of the same URL. */
    static _Atomic uint32_t last_url;
    uint32_t hash=2166136261u;
    for (const char *c=url;*c;++c) hash=(hash^(unsigned char)*c)*16777619u;
    if (atomic_exchange(&last_url,hash)==hash) return id;
    printf("Runtime: http request method %d, %llu body bytes: %s (not sent: %s)\n",method,(unsigned long long)length,url,
           !replay ? "no replay route" : discovery_port() ? "another replay is open" : "GOLF_DISCOVERY_PORT is not set");
    return id;
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
    {"sceHttpSendRequest",http_send}, {"sceHttpGetStatusCode",http_status},
    {"sceHttpGetResponseContentLength",http_length}, {"sceHttpReadData",http_read},
    {"sceHttpDeleteRequest",http_delete_request},
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
