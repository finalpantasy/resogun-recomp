#ifndef GOLF_PREPARE_H
#define GOLF_PREPARE_H

enum { GOLF_PATH = 1024, GOLF_SOURCE_KEY = 65 };

/* The destination must be a new directory owned by the caller. */
int prepare_image(const char *game, const char *destination, const char *key, int any_build);
int prepare_source_key(const char *game, char key[GOLF_SOURCE_KEY], int any_build);

#endif
