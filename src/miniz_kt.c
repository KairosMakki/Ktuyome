#undef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 0
#include "ktuyome.h"
#ifndef NDEBUG
#define NDEBUG
#endif
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#include "vendor/miniz/miniz.c"