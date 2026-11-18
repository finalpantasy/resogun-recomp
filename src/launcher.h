#ifndef GOLF_LAUNCHER_H
#define GOLF_LAUNCHER_H

enum { LAUNCH_RUN, LAUNCH_ERROR, LAUNCH_DONE };

/* Configure portable state and translate user-facing options into the loader's argv. */
int launcher_start(int *argc, char ***argv);
const char *launcher_save_dir(void);

#endif
