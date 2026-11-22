/* The per-profile record of earned trophies: <user dir>/trophies.txt, one "id grade unixtime"
 * line each, behind a "EGTROPHY 1" header. Header-only so the runtime, its test and the launcher
 * share one reader. Anything unreadable (bad line, id out of range, repeated id) is skipped,
 * never fatal; a new unlock is appended, so earlier lines survive a crash. Grade is P (platinum),
 * G, S, B or ? when the package's grades cannot be read (TROPCONF.ESFM is encrypted). */
#ifndef TROPHY_RECORD_H
#define TROPHY_RECORD_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TROPHY_REC_MAX 128
typedef struct { int id; char grade; long long when; } TrophyRec;
typedef struct { TrophyRec e[TROPHY_REC_MAX]; int n; } TrophyLog;

static inline int trophy_log_has(const TrophyLog *log,int id) {
    for (int i=0;i<log->n;i++) if (log->e[i].id==id) return 1;
    return 0;
}
/* Reads an open file into *log; returns the number of lines skipped as bad. */
static inline int trophy_log_read(TrophyLog *log,FILE *f) {
    int bad=0;
    log->n=0;
    char line[160];
    while (fgets(line,sizeof(line),f)) {
        if (!strncmp(line,"EGTROPHY",8)) continue;
        int id; char grade[8]; long long when;
        if (line[0]=='\n' || line[0]=='\r') continue;
        if (sscanf(line,"%d %1[PGSB?] %lld",&id,grade,&when)!=3 || id<0 || id>=TROPHY_REC_MAX || when<0 ||
            trophy_log_has(log,id) || log->n>=TROPHY_REC_MAX) { bad++; continue; }
        log->e[log->n].id=id; log->e[log->n].grade=grade[0]; log->e[log->n].when=when; log->n++;
    }
    return bad;
}
/* Reads the file into *log (empty when missing); returns the number of lines skipped as bad. */
static inline int trophy_log_load(TrophyLog *log,const char *path) {
    FILE *f=fopen(path,"rb");
    log->n=0;
    if (!f) return 0;
    int bad=trophy_log_read(log,f);
    fclose(f);
    return bad;
}
/* Records an unlock: 1 added, 0 already recorded (nothing written), -1 write failed. */
static inline int trophy_log_add(TrophyLog *log,const char *path,int id,char grade,long long when) {
    if (id<0 || id>=TROPHY_REC_MAX || log->n>=TROPHY_REC_MAX) return -1;
    if (trophy_log_has(log,id)) return 0;
    int fresh=1, needs_newline=0;
    FILE *r=fopen(path,"rb");
    if (r) {
        if (fseek(r,0,SEEK_END)==0 && ftell(r)>0) {
            fresh=0;
            fseek(r,-1,SEEK_END);
            needs_newline=fgetc(r)!='\n';
        }
        fclose(r);
    }
    FILE *f=fopen(path,"ab");
    if (!f) return -1;
    if (needs_newline) fputc('\n',f);
    if (fresh) fputs("EGTROPHY 1\n",f);
    int ok=fprintf(f,"%d %c %lld\n",id,grade,when)>0;
    if (fclose(f)!=0) ok=0;
    if (!ok) return -1;
    log->e[log->n].id=id; log->e[log->n].grade=grade; log->e[log->n].when=when; log->n++;
    return 1;
}
#endif
