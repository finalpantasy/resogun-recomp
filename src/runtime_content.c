/* Offline AppContent provider for the configured base-game profile.
 * Add-ons are the folders under <user>/addcont/ and, when one is set, under an extra DLC folder
 * (none by default); no downloads, entitlement or license emulation. */
#include "runtime.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
#define NO_ENTITLEMENT ((int32_t)0x80d90007)
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries;
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); exit(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
/* sceSysmoduleLoadModule(id): Resogun ships no system module of its own to start (sce_module
 * holds only libc and Fios2, linked at boot). */
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); exit(21); }
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    require_loaded();
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    puts("Runtime: AppContent initialized; offline base-game profile");
    return 0;
}
static void require_initialized(void) {
    require_loaded();
    if (!initialized) { fputs("STOP: AppContent query before initialization\n",stderr); exit(21); }
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    require_initialized();
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
/* Installed add-ons: each folder <user>/addcont/<label>/ is one, then each <extra>/<label>/ whose
 * label <user>/addcont/ lacks; its name is the entitlement label (up to 16 characters). Resogun's
 * add-ons are ships and skins with no data of their own: a folder holding sce_sys is enough. */
#define LABEL_SIZE 17
#define MAX_ADDONS 64
typedef struct { char label[LABEL_SIZE]; char folder[700]; } Addon;
static Addon addons[MAX_ADDONS];
static unsigned addon_count, addons_scanned;
static int golf_title;
/* The ticket package's ProductLabel is longer than its EntitlementLabel. */
static const char *golf_entitlement_label(const char *label) {
    if (!golf_title) return label;
    if (!strcmp(label,"EN00010000000000")) return "EN0001";
    if (!strcmp(label,"EN00020000000000")) return "EN0002";
    return label;
}
static char extra_dir[600];
static int addon_order(const void *a,const void *b) { return strcmp(((const Addon *)a)->label,((const Addon *)b)->label); }
static int find_addon(const char *label) {
    for (unsigned i=0;i<addon_count;++i) if (!strncmp(label,addons[i].label,LABEL_SIZE-1)) return (int)i;
    return -1;
}
static int is_folder(const char *path) {
    DIR *d=opendir(path);
    if (!d) return 0;
    closedir(d); return 1;
}
/* An extra DLC folder holds the add-ons, or (as shadPS4's addcont folder does) a <title ID>/
 * folder that holds them. */
void runtime_content_extra_dir(const char *folder,const char *title_id) {
    if (!folder || !folder[0]) { extra_dir[0]=0; return; }
    char titled[600];
    if (title_id && title_id[0] && (size_t)snprintf(titled,sizeof(titled),"%s/%s",folder,title_id)<sizeof(titled) && is_folder(titled))
        snprintf(extra_dir,sizeof(extra_dir),"%s",titled);
    else snprintf(extra_dir,sizeof(extra_dir),"%s",folder);
    printf("Runtime: extra DLC folder %s%s\n",extra_dir,is_folder(extra_dir) ? "" : " (missing)");
}
static void scan_folder(const char *path) {
    DIR *d=opendir(path);
    if (!d) return;
    for (struct dirent *e; addon_count<MAX_ADDONS && (e=readdir(d));) {
        const char *label=golf_entitlement_label(e->d_name);
        size_t n=strlen(label);
        if (e->d_name[0]=='.' || n>=LABEL_SIZE || find_addon(label)>=0) continue;
        Addon *a=&addons[addon_count];
        if ((size_t)snprintf(a->folder,sizeof(a->folder),"%s/%s",path,e->d_name)>=sizeof(a->folder) || !is_folder(a->folder)) continue;
        memcpy(a->label,label,n+1);
        ++addon_count;
    }
    closedir(d);
}
/* Everybody's Golf (CUSA04687): its add-ons are entitlements with no files (carts, courses,
 * boosters, character boxes, outfits, tickets). The game's own table
 * (xbdata/param/entitlement/entitlement_param.xb: label, product label, item save-data id) lists
 * 31; 24 are in the supplied packages, the other 7 (16, 17, 20, 22, 24, 25, 26) have no
 * package here. All 31 are reported as installed by default; GOLF_ENTITLEMENTS=original reports only
 * the add-on folders, as for any other game. */
static const char *const golf_addons[]={
    "EN00000000000001","EN00000000000002","EN00000000000003","EN00000000000004",
    "EN00000000000005","EN00000000000006","EN00000000000007","EN00000000000008",
    "EN00000000000009","EN00000000000010","EN00000000000011","EN00000000000012",
    "EN00000000000013","EN00000000000014","EN00000000000015","EN00000000000016",
    "EN00000000000017","EN00000000000018","EN00000000000019","EN00000000000020",
    "EN00000000000021","EN00000000000022","EN00000000000023","EN00000000000024",
    "EN00000000000025","EN00000000000026","EN00000000000027","EN00000000000028",
    "EN00000000000029","EN0001","EN0002"
};
void runtime_content_set_title(const char *title_id) { golf_title=title_id && !strncmp(title_id,"CUSA04687",9); }
static void builtin_addons(void) {
    const char *mode=getenv("GOLF_ENTITLEMENTS");
    if (!golf_title || (mode && !strcmp(mode,"original"))) return;
    unsigned added=0;
    for (size_t i=0;i<sizeof(golf_addons)/sizeof(golf_addons[0]) && addon_count<MAX_ADDONS;++i) {
        if (find_addon(golf_addons[i])>=0) continue;
        Addon *a=&addons[addon_count++];
        memcpy(a->label,golf_addons[i],LABEL_SIZE); strcpy(a->folder,"built in");
        ++added;
    }
    printf("Runtime: %u built-in add-ons unlocked (GOLF_ENTITLEMENTS=original turns this off)\n",added);
}
static void scan_addons(void) {
    if (addons_scanned) return;
    addons_scanned=1;
    char path[600]; snprintf(path,sizeof(path),"%s/addcont",runtime_file_user_dir());
    scan_folder(path);
    if (extra_dir[0]) scan_folder(extra_dir);
    builtin_addons();
    qsort(addons,addon_count,sizeof(*addons),addon_order);
    for (unsigned i=0;i<addon_count;++i) printf("Runtime: add-on %s installed (%s)\n",addons[i].label,addons[i].folder);
}
/* sceAppContentGetAddcontInfo(service, label, info): the one add-on of `label` (17-byte
 * entitlement label) in the list's form, or DRM_NO_ENTITLEMENT when no folder has that label.
 * Resogun asks for each of its ships and skins this way. */
static ABI int32_t addon_info(uint32_t service,const char *label,void *info) {
    require_initialized();
    if (service) { fputs("STOP: nonzero AppContent service label unsupported\n",stderr); exit(21); }
    if (!label || !info) return PARAMETER;
    scan_addons();
    char want[17]; memcpy(want,label,16); want[16]=0;
    const char *canonical=golf_entitlement_label(want);
    ++queries;
    for (unsigned i=0;i<addon_count;++i) {
        if (strcmp(addons[i].label,canonical)) continue;
        unsigned char *out=info; uint32_t installed=4;
        memset(out,0,24); memcpy(out,addons[i].label,strlen(addons[i].label)); memcpy(out+20,&installed,4);
        return 0;
    }
    return NO_ENTITLEMENT;
}
/* sceAppContentGetAddcontInfoList(service, list, list size, hits): the installed add-ons, 24 bytes
 * each (label, status 4 = installed); with no list, only their number (shadPS4 app_content.cpp). */
static ABI int32_t addon_list(uint32_t service,unsigned char *list,uint32_t size,uint32_t *hits) {
    require_initialized();
    (void)service;
    if (!hits) return PARAMETER;
    scan_addons();
    ++queries;
    if (!list || !size) { *hits=addon_count; return 0; }
    uint32_t n=0;
    for (; n<addon_count && n<size; ++n) {
        unsigned char *out=list+(size_t)n*24; const uint32_t installed=4;
        memset(out,0,24); memcpy(out,addons[n].label,strlen(addons[n].label)); memcpy(out+20,&installed,4);
    }
    *hits=n;
    printf("Runtime: add-on list: %u of %u reported\n",n,addon_count);
    return 0;
}
/* sceAppContentTemporaryDataMount2(option, mount) (shadPS4 app_content.cpp): names /temp0, kept in
 * <user>/temp0; the folder is made and mounted on the first call (Resogun's boot makes none). */
static ABI int32_t temp_mount(uint32_t option,char *mount) {
    static int mounted;
    (void)option;
    if (!mount) return PARAMETER;
    if (!mounted) {
        char path[600]; snprintf(path,sizeof(path),"%s/temp0",runtime_file_user_dir());
        mkdir(path,0755);
        if (runtime_file_mount("/temp0",path)) return PARAMETER;
        mounted=1;
    }
    memset(mount,0,16); memcpy(mount,"/temp0",6); return 0;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (!strcmp(name,"buYbeLOGWmA#libSceAppContent")) return (uintptr_t)temp_mount;
    if (!strcmp(name,"g8cM39EUZ6o#libSceSysmodule")) return (uintptr_t)module_load;
    if (!strcmp(name,"fMP5NHUOaMk#libSceSysmodule")) return (uintptr_t)module_loaded;
    if (!strcmp(name,"eR2bZFAAU0Q#libSceSysmodule")) return (uintptr_t)module_unload;
    if (!strcmp(name,"R9lA82OraNs#libSceAppContent")) return (uintptr_t)content_init;
    if (!strcmp(name,"99b82IKXpH4#libSceAppContent")) return (uintptr_t)param_int;
    if (!strcmp(name,"m47juOmH0VE#libSceAppContent")) return (uintptr_t)addon_info;
    if (!strcmp(name,"xnd8BJzAxmk#libSceAppContent")) return (uintptr_t)addon_list;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, queries=%u\n",refs,initialized,queries);
}
