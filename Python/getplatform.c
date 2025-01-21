
#include "Python.h"

#ifndef PLATFORM
#if defined(__psp__)
#define PLATFORM "psp"
#elif defined(__wii__)
#define PLATFORM "wii"
#elif defined(__vita__)
#define PLATFORM "vita"
#elif defined(__3DS__) || defined(_3DS)
#define PLATFORM "3ds"
#else
#define PLATFORM "unknown"
#endif
#endif

const char *
Py_GetPlatform(void)
{
	return PLATFORM;
}
