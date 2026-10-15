/* Montage: forced into libMXF's sources under MinGW, whose GCC has no structured exception handling (__try /
   __except, used by mxf_win32_mmap.c). There the copies run unguarded: an I/O error in a mapped view ends the program
   rather than the read. */
#pragma once
#ifndef __try
#define __try if (1)
#define __except(filter) else if (0)
#endif
