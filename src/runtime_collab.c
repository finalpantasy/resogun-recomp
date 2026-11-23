/* Everybody's Golf (CUSA04687): settings.ini collab_items=earn puts the tie-in outfits on sale in
 * the game's own shop instead of granting them (runtime_game.c grants them with collab_items=
 * unlocked). The shop reads its stock from the shop sheet data/param/serverFile/shop_price_sheet.txt
 * inside xbdata/param/serverFile/info.<locale>.xb (shop_id, floor_id, sort_id, ..., item_id, ...,
 * price, sales_num, weekday, hour and date window columns). Floor 16 is the coin tab that sold the
 * Atelier and BEAMS GOLF pieces for 500 coins from 2018-08-31 to 09-28; its rows without a date
 * window are on sale every day. The archive the game opens is swapped for a copy, written at the
 * first open of each start in the save folder, whose sheet sells every part runtime_game.c lists
 * on floor 16 with no window, at 3 to 5 times the tab's 300-500 coins so they take a while to
 * earn (the prices below): the existing Atelier rows lose their window, the others are added. A bought part is the game's own purchase and stays in the save. */
#include "runtime.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <zlib.h>
#include "gpu/resogun_gpu.h"

/* The tuning: the coin tab, and the prices (the tab's own gear costs 300 to 500 coins). */
#define COLLAB_FLOOR "16"
#define COLLAB_PRICE_PART 1500   /* caps, accessories, gloves, shoes, socks: 3x */
#define COLLAB_PRICE_OUTFIT 2000 /* outers and the T-shirt: 4x */
#define COLLAB_PRICE_TOP 2500    /* the full-body suits (Moogle, Jibanyan, Komasan) and the FF30 cart: 5x */
static const uint16_t collab_outfits[]={8953,8969,8970,8986,8987,8988,8989,8990,8991,9124};
static const uint16_t collab_top[]={8956,8958,8984,8985};

static int collab_price(unsigned id) {
    for (size_t i=0;i<sizeof(collab_top)/sizeof(collab_top[0]);++i) if (collab_top[i]==id) return COLLAB_PRICE_TOP;
    for (size_t i=0;i<sizeof(collab_outfits)/sizeof(collab_outfits[0]);++i) if (collab_outfits[i]==id) return COLLAB_PRICE_OUTFIT;
    return COLLAB_PRICE_PART;
}

static uint32_t name_hash(const char *s) {
    uint32_t h=0;
    for (; *s; ++s) {
        const char c=*s>='A' && *s<='Z' ? (char)(*s+32) : *s;
        h=31*h+(uint32_t)(int32_t)(signed char)c;
    }
    return h;
}

/* The shop sheet `text` (CRLF lines, 38 columns) with the parts on sale; NULL when it is not one. */
static char *patch_sheet(const char *text,size_t size,size_t *out_size,unsigned *opened,unsigned *added) {
    size_t n=0;
    const uint16_t *items=runtime_game_golf_collab_list(&n);
    unsigned char seen[64]={0};
    if (n>sizeof(seen) || size<10 || memcmp(text,"\xef\xbb\xbfshop_id",10)) return NULL;
    char *out=malloc(size+n*96+16);
    if (!out) return NULL;
    size_t o=0;
    int max_sort=0;
    *opened=*added=0;
    for (size_t at=0; at<size;) {
        const char *line=text+at, *end=memchr(line,'\n',size-at);
        const size_t len=end ? (size_t)(end-line)+1 : size-at;
        at+=len;
        char fields[40][24];
        int count=0, too_long=0;
        size_t k=0;
        for (int f=0; f<40; ++f) {
            size_t j=0;
            while (k<len && line[k]!='\t' && line[k]!='\r' && line[k]!='\n') {
                if (j<23) fields[f][j++]=line[k]; else too_long=1;
                ++k;
            }
            fields[f][j]=0;
            ++count;
            if (k>=len || line[k]!='\t') break;
            ++k;
        }
        int item=-1;
        if (line!=text && count==38 && !too_long) {
            const int sort=atoi(fields[2]);
            if (sort>max_sort) max_sort=sort;
            const int id=atoi(fields[7]);
            for (size_t i=0;i<n;++i) if (fields[7][0] && items[i]==id) item=(int)i;
        }
        if (item<0) { memcpy(out+o,line,len); o+=len; continue; }
        /* A collab part the sheet sells: on floor 16 at its price, with no weekday, hour or date window. */
        seen[item]=1;
        ++*opened;
        o+=(size_t)sprintf(out+o,"%s\t" COLLAB_FLOOR "\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%s",
                           fields[0],fields[2],fields[3],fields[4],fields[5],fields[6],fields[7],fields[8],fields[9],
                           fields[10],fields[11],fields[12],fields[13],collab_price(items[item]),fields[15]);
        for (int f=16; f<38; ++f) out[o++]='\t';
        out[o++]='\r'; out[o++]='\n';
    }
    if (o && out[o-1]!='\n') { out[o++]='\r'; out[o++]='\n'; }
    for (size_t i=0;i<n;++i) {
        if (seen[i]) continue;
        o+=(size_t)sprintf(out+o,"0\t" COLLAB_FLOOR "\t%d\t\t\t\t\t%u\t\t\t\t1\t\t\t%d\t1",++max_sort,(unsigned)items[i],
                           collab_price(items[i]));
        for (int f=16; f<38; ++f) out[o++]='\t';
        out[o++]='\r'; out[o++]='\n';
        ++*added;
    }
    *out_size=o;
    return out;
}

/* The archive `data` with its shop sheet patched, in `*out` (malloc); 0 on success. The archive:
 * 'FB\0\0', count, 4, 0, then count rows [name hash, 0, stream, csize, usize] sorted by hash; row
 * k's sizes are those of the k-th zlib stream, the streams follow the rows back to back, 4-aligned,
 * and a name's data is stream `stream` of its row. */
int runtime_collab_patch_archive(const unsigned char *data,size_t size,unsigned char **out,size_t *out_size,
                                 unsigned *opened,unsigned *added) {
    if (size<16 || memcmp(data,"FB\0\0",4)) return -1;
    uint32_t count;
    memcpy(&count,data+4,4);
    if (!count || count>4096 || 16+(size_t)count*20>size) return -1;
    uint32_t (*rows)[5]=malloc(count*sizeof(*rows));
    size_t *offsets=malloc(count*sizeof(size_t));
    unsigned char *text=NULL,*packed=NULL,*file=NULL;
    char *patched=NULL;
    int result=-1;
    if (!rows || !offsets) goto done;
    memcpy(rows,data+16,(size_t)count*20);
    size_t pos=16+(size_t)count*20;
    int64_t sheet=-1;
    const uint32_t want=name_hash("data/param/serverFile/shop_price_sheet.txt");
    for (uint32_t k=0;k<count;++k) {
        offsets[k]=pos;
        pos=(pos+rows[k][3]+3)&~(size_t)3;
        if (rows[k][0]==want) sheet=rows[k][2];
    }
    if (pos>size+3 || sheet<0 || sheet>=(int64_t)count) goto done;
    uLongf usize=rows[sheet][4];
    if (!(text=malloc(usize+1)) || uncompress(text,&usize,data+offsets[sheet],rows[sheet][3])!=Z_OK) goto done;
    size_t patched_size;
    if (!(patched=patch_sheet((const char *)text,usize,&patched_size,opened,added))) goto done;
    uLongf csize=compressBound(patched_size);
    if (!(packed=malloc(csize)) || compress2(packed,&csize,(const unsigned char *)patched,patched_size,9)!=Z_OK) goto done;
    if (!(file=malloc(size+csize+16))) goto done;
    rows[sheet][3]=(uint32_t)csize;
    rows[sheet][4]=(uint32_t)patched_size;
    memcpy(file,data,16);
    memcpy(file+16,rows,(size_t)count*20);
    size_t o=16+(size_t)count*20;
    for (uint32_t k=0;k<count;++k) {
        const int mine=k==(uint32_t)sheet;
        const size_t n=mine ? csize : rows[k][3];
        if (!mine && offsets[k]+n>size) goto done;
        memcpy(file+o,mine ? packed : data+offsets[k],n);
        o+=n;
        while (o&3) file[o++]=0;
    }
    *out=file;
    *out_size=o;
    file=NULL;
    result=0;
done:
    free(rows); free(offsets); free(text); free(patched); free(packed); free(file);
    return result;
}

/* Called by the file layer with each resolved game path: while collab_items=earn the shop archive
 * becomes the patched copy, made at the first open. */
void runtime_collab_redirect(const char *guest,char *host,size_t size) {
    const char *at=strstr(guest,"/param/serverFile/info");
    if (!at || strchr(at+18,'/')) return;
    const size_t len=strlen(at);
    if (strcmp(at+len-3,".xb")) return;
    static int mode=-1;
    if (mode<0) {
        const char *ent=getenv("GOLF_ENTITLEMENTS");
        mode=ent && !strcmp(ent,"original") ? 0 : resogun_collab_items();
    }
    if (mode!=2) return;
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    static char made[8][128];
    static int made_ok[8];
    const char *base=strrchr(at,'/')+1;
    char copy[1024];
    if ((size_t)snprintf(copy,sizeof(copy),"%s/collab-shop",runtime_file_user_dir())>=sizeof(copy)) return;
    pthread_mutex_lock(&lock);
    mkdir(copy,0755);
    int done=0;
    if ((size_t)snprintf(copy,sizeof(copy),"%s/collab-shop/%s",runtime_file_user_dir(),base)>=sizeof(copy) ||
        strlen(copy)>=size || strlen(base)>=sizeof(made[0])) {
        pthread_mutex_unlock(&lock);
        return;
    }
    int known=0;
    for (int i=0;i<8;++i) if (!strcmp(made[i],base)) { known=1; done=made_ok[i]; }
    if (!known) {
        FILE *in=fopen(host,"rb");
        unsigned char *data=NULL,*patched=NULL;
        size_t n=0,patched_size=0;
        unsigned opened=0,added=0;
        long end=-1;
        if (in && !fseek(in,0,SEEK_END) && (end=ftell(in))>0 && !fseek(in,0,SEEK_SET) && (n=(size_t)end) &&
            (data=malloc(n)) && fread(data,1,n,in)==n &&
            !runtime_collab_patch_archive(data,n,&patched,&patched_size,&opened,&added)) {
            char tmp[1100];
            snprintf(tmp,sizeof(tmp),"%s.tmp",copy);
            FILE *out=fopen(tmp,"wb");
            int ok=out && fwrite(patched,1,patched_size,out)==patched_size;
            if (out && fclose(out)) ok=0;
            remove(copy);
            if (ok && !rename(tmp,copy)) {
                done=1;
                printf("Collab items: collab_items=earn: the shop's coin tab sells %u tie-in parts for %d to %d"
                       " coins, every day (%u rows reopened, %u added; %s)\n",opened+added,COLLAB_PRICE_PART,COLLAB_PRICE_TOP,opened,added,base);
            } else remove(tmp);
        } else printf("Collab items: %s has no shop sheet to patch; the shop is unchanged\n",base);
        if (in) fclose(in);
        free(data);
        free(patched);
        for (int i=0;i<8;++i) if (!made[i][0]) { snprintf(made[i],sizeof(made[i]),"%s",base); made_ok[i]=done; break; }
    }
    pthread_mutex_unlock(&lock);
    if (done) memcpy(host,copy,strlen(copy)+1);
}
