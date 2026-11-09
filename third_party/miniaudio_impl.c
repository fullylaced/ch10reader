/* Single translation unit holding the miniaudio implementation. Kept in C so
 * the (large) header compiles with the C compiler on every platform. */
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#include "miniaudio.h"
