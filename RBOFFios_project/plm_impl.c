/* pl_mpeg (MIT, (c) Dominic Szablewski) - MPEG-1 video + MP2 audio decoder, used for the game's .mpg movies.
 * The implementation lives in this one translation unit; everything else just includes pl_mpeg.h for the declarations. */
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
