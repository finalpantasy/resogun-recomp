/* The per-profile trophy record: new unlocks, repeats, and a damaged file. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/trophy_record.h"
#include <assert.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p,0755)
#endif

#define DIR_ "out/test_trophy_record"
#define FILE_ DIR_ "/trophies.txt"

static void write_file(const char *text) {
    FILE *f=fopen(FILE_,"wb"); assert(f); fputs(text,f); fclose(f);
}
int main(void) {
    MKDIR("out"); MKDIR(DIR_);
    TrophyLog log;
    remove(FILE_);

    /* Missing file: empty, nothing bad. */
    assert(trophy_log_load(&log,FILE_)==0 && log.n==0);

    /* A new unlock is written; the same id again is skipped and writes nothing. */
    assert(trophy_log_add(&log,FILE_,3,'?',1700000000)==1 && log.n==1);
    assert(trophy_log_add(&log,FILE_,3,'G',1700000099)==0 && log.n==1);
    assert(trophy_log_add(&log,FILE_,0,'P',1700000100)==1 && log.n==2);
    assert(trophy_log_add(&log,FILE_,-1,'?',1)==-1 && trophy_log_add(&log,FILE_,TROPHY_REC_MAX,'?',1)==-1);

    /* Reload: both entries, first date kept. */
    TrophyLog again;
    assert(trophy_log_load(&again,FILE_)==0 && again.n==2);
    assert(again.e[0].id==3 && again.e[0].grade=='?' && again.e[0].when==1700000000);
    assert(again.e[1].id==0 && again.e[1].grade=='P' && again.e[1].when==1700000100);

    /* A damaged file: garbage, a repeated id, a bad grade, an id out of range, a cut-off last line
     * with no newline. The good lines survive, the bad ones are counted. */
    write_file("EGTROPHY 1\n5 B 1700000005\nnot a line\n5 G 1700000006\n7 X 1700000007\n999 S 1\n9 S 17000");
    assert(trophy_log_load(&log,FILE_)==4 && log.n==2);   /* 5 and the cut-off 9 parse; 4 lines bad */
    assert(log.e[0].id==5 && log.e[0].grade=='B' && log.e[1].id==9);

    /* Appending after a cut-off line starts on a new line and keeps the earlier lines. */
    assert(trophy_log_add(&log,FILE_,11,'S',1700000011)==1);
    assert(trophy_log_load(&again,FILE_)==4 && again.n==3 && trophy_log_has(&again,11) && trophy_log_has(&again,5));

    /* Binary noise and an empty file load as empty. */
    write_file("");
    assert(trophy_log_load(&log,FILE_)==0 && log.n==0);
    write_file("\x01\x02\xff\xfe\x00\x03 \n\n\n");
    log.n=0; trophy_log_load(&log,FILE_); assert(log.n==0);
    assert(trophy_log_add(&log,FILE_,2,'?',5)==1 && trophy_log_load(&again,FILE_)>=0 && trophy_log_has(&again,2));
    remove(FILE_);
    puts("trophy record tests passed");
    return 0;
}
