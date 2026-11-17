#ifndef _PXQUAKE_H
#define _PXQUAKE_H
#include <AEEStdDef.h>
#include <remote.h>
#include <string.h>
#include <stdlib.h>


#ifndef __QAIC_HEADER
#define __QAIC_HEADER(ff) ff
#endif //__QAIC_HEADER

#ifndef __QAIC_HEADER_EXPORT
#define __QAIC_HEADER_EXPORT
#endif // __QAIC_HEADER_EXPORT

#ifndef __QAIC_HEADER_ATTRIBUTE
#define __QAIC_HEADER_ATTRIBUTE
#endif // __QAIC_HEADER_ATTRIBUTE

#ifndef __QAIC_IMPL
#define __QAIC_IMPL(ff) ff
#endif //__QAIC_IMPL

#ifndef __QAIC_IMPL_EXPORT
#define __QAIC_IMPL_EXPORT
#endif // __QAIC_IMPL_EXPORT

#ifndef __QAIC_IMPL_ATTRIBUTE
#define __QAIC_IMPL_ATTRIBUTE
#endif // __QAIC_IMPL_ATTRIBUTE
#ifndef _QAIC_ENV_H
#define _QAIC_ENV_H

#include <stdio.h>
#ifdef _WIN32
#include "qtest_stdlib.h"
#else
#define MALLOC malloc
#define FREE free
#endif

#ifdef __GNUC__
#ifdef __clang__
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#else
#pragma GCC diagnostic ignored "-Wpragmas"
#endif
#pragma GCC diagnostic ignored "-Wuninitialized"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#ifndef _ATTRIBUTE_UNUSED

#ifdef _WIN32
#define _ATTRIBUTE_UNUSED
#else
#define _ATTRIBUTE_UNUSED __attribute__ ((unused))
#endif

#endif // _ATTRIBUTE_UNUSED

#ifndef _ATTRIBUTE_VISIBILITY

#ifdef _WIN32
#define _ATTRIBUTE_VISIBILITY
#else
#define _ATTRIBUTE_VISIBILITY __attribute__ ((visibility("default")))
#endif

#endif // _ATTRIBUTE_VISIBILITY

#ifndef __QAIC_REMOTE
#define __QAIC_REMOTE(ff) ff
#endif //__QAIC_REMOTE

#ifndef __QAIC_HEADER
#define __QAIC_HEADER(ff) ff
#endif //__QAIC_HEADER

#ifndef __QAIC_HEADER_EXPORT
#define __QAIC_HEADER_EXPORT
#endif // __QAIC_HEADER_EXPORT

#ifndef __QAIC_HEADER_ATTRIBUTE
#define __QAIC_HEADER_ATTRIBUTE
#endif // __QAIC_HEADER_ATTRIBUTE

#ifndef __QAIC_IMPL
#define __QAIC_IMPL(ff) ff
#endif //__QAIC_IMPL

#ifndef __QAIC_IMPL_EXPORT
#define __QAIC_IMPL_EXPORT
#endif // __QAIC_IMPL_EXPORT

#ifndef __QAIC_IMPL_ATTRIBUTE
#define __QAIC_IMPL_ATTRIBUTE
#endif // __QAIC_IMPL_ATTRIBUTE

#ifndef __QAIC_STUB
#define __QAIC_STUB(ff) ff
#endif //__QAIC_STUB

#ifndef __QAIC_STUB_EXPORT
#define __QAIC_STUB_EXPORT
#endif // __QAIC_STUB_EXPORT

#ifndef __QAIC_STUB_ATTRIBUTE
#define __QAIC_STUB_ATTRIBUTE
#endif // __QAIC_STUB_ATTRIBUTE

#ifndef __QAIC_SKEL
#define __QAIC_SKEL(ff) ff
#endif //__QAIC_SKEL__

#ifndef __QAIC_SKEL_EXPORT
#define __QAIC_SKEL_EXPORT
#endif // __QAIC_SKEL_EXPORT

#ifndef __QAIC_SKEL_ATTRIBUTE
#define __QAIC_SKEL_ATTRIBUTE
#endif // __QAIC_SKEL_ATTRIBUTE

#ifdef __QAIC_DEBUG__
   #ifndef __QAIC_DBG_PRINTF__
   #include <stdio.h>
   #define __QAIC_DBG_PRINTF__( ee ) do { printf ee ; } while(0)
   #endif
#else
   #define __QAIC_DBG_PRINTF__( ee ) (void)0
#endif


#define _OFFSET(src, sof)  ((void*)(((char*)(src)) + (sof)))

#define _COPY(dst, dof, src, sof, sz)  \
   do {\
         struct __copy { \
            char ar[sz]; \
         };\
         *(struct __copy*)_OFFSET(dst, dof) = *(struct __copy*)_OFFSET(src, sof);\
   } while (0)

#define _COPYIF(dst, dof, src, sof, sz)  \
   do {\
      if(_OFFSET(dst, dof) != _OFFSET(src, sof)) {\
         _COPY(dst, dof, src, sof, sz); \
      } \
   } while (0)

_ATTRIBUTE_UNUSED
static __inline void _qaic_memmove(void* dst, void* src, int size) {
   int i = 0;
   for(i = 0; i < size; ++i) {
      ((char*)dst)[i] = ((char*)src)[i];
   }
}

#define _MEMMOVEIF(dst, src, sz)  \
   do {\
      if(dst != src) {\
         _qaic_memmove(dst, src, sz);\
      } \
   } while (0)


#define _ASSIGN(dst, src, sof)  \
   do {\
      dst = OFFSET(src, sof); \
   } while (0)

#define _STD_STRLEN_IF(str) (str == 0 ? 0 : strlen(str))

#include "AEEStdErr.h"

#ifdef _WIN32
#define _QAIC_FARF(level, msg, ...) (void)0
#else
#define _QAIC_FARF(level, msg, ...) \
   do {\
      if(0 == (HAP_debug_v2) ) {\
         (void)0; \
      } else { \
         FARF(level, msg , ##__VA_ARGS__); \
      } \
   }while(0)
#endif //_WIN32 for _QAIC_FARF

#define _TRY(ee, func) \
   do { \
      if (AEE_SUCCESS != ((ee) = func)) {\
         __QAIC_DBG_PRINTF__((__FILE__ ":%d:error:%d:%s\n", __LINE__, (int)(ee),#func));\
         goto ee##bail;\
      } \
   } while (0)

#define _TRY_FARF(ee, func) \
   do { \
      if (AEE_SUCCESS != ((ee) = func)) {\
         goto ee##farf##bail;\
      } \
   } while (0)

#define _QAIC_CATCH(exception) exception##bail: if (exception != AEE_SUCCESS)

#define _CATCH_FARF(exception) exception##farf##bail: if (exception != AEE_SUCCESS)

#define _QAIC_ASSERT(nErr, ff) _TRY(nErr, 0 == (ff) ? AEE_EBADPARM : AEE_SUCCESS)

#ifdef __QAIC_DEBUG__
#define _QAIC_ALLOCATE(nErr, pal, size, alignment, pv) _TRY(nErr, _allocator_alloc(pal, __FILE_LINE__, size, alignment, (void**)&pv));\
                                                  _QAIC_ASSERT(nErr,pv || !(size))
#else
#define _QAIC_ALLOCATE(nErr, pal, size, alignment, pv) _TRY(nErr, _allocator_alloc(pal, 0, size, alignment, (void**)&pv));\
                                                  _QAIC_ASSERT(nErr,pv || !(size))
#endif


#endif // _QAIC_ENV_H

#ifdef __cplusplus
extern "C" {
#endif
/// pxquake - Quake 1 span drawing on the Snapdragon X NPU (Hexagon), phase 2 (2026-10-26).
/// Its own small FastRPC module (libpxquake_skel.so), NOT part of the pulsehmx skel.
/// Command stream contract: pxquake_imp.c (top comment) and bridge/pxquake/pc_dsp.c (the writer).
/// Regenerate the glue with gen_glue.sh after changing this file.
/**
    * Opens the handle in the specified domain.  If this is the first
    * handle, this creates the session.  Typically this means opening
    * the device, aka open("/dev/adsprpc-smd"), then calling ioctl
    * device APIs to create a PD on the DSP to execute our code in,
    * then asking that PD to dlopen the .so and dlsym the skel function.
    *
    * @param uri, <interface>_URI"&_dom=aDSP"
    *    <interface>_URI is a QAIC generated uri, or
    *    "file:///<sofilename>?<interface>_skel_handle_invoke&_modver=1.0"
    *    If the _dom parameter is not present, _dom=DEFAULT is assumed
    *    but not forwarded.
    *    Reserved uri keys:
    *      [0]: first unamed argument is the skel invoke function
    *      _dom: execution domain name, _dom=mDSP/aDSP/DEFAULT
    *      _modver: module version, _modver=1.0
    *      _*: any other key name starting with an _ is reserved
    *    Unknown uri keys/values are forwarded as is.
    * @param h, resulting handle
    * @retval, 0 on success
    */
__QAIC_HEADER_EXPORT int __QAIC_HEADER(pxquake_open)(const char* uri, remote_handle64* h) __QAIC_HEADER_ATTRIBUTE;
/** 
    * Closes a handle.  If this is the last handle to close, the session
    * is closed as well, releasing all the allocated resources.

    * @param h, the handle to close
    * @retval, 0 on success, should always succeed
    */
__QAIC_HEADER_EXPORT int __QAIC_HEADER(pxquake_close)(remote_handle64 h) __QAIC_HEADER_ATTRIBUTE;
/// truly empty round trip (call-cost measurement)
__QAIC_HEADER_EXPORT int __QAIC_HEADER(pxquake_nop)(remote_handle64 _h) __QAIC_HEADER_ATTRIBUTE;
/// round trip with one shared input buffer (frame-sized), DSP reads the first and last byte: zero-copy handover cost.
/// status[0] = 0x51 marker, [1] = buf[0], [2] = buf[len-1], [3] = len
__QAIC_HEADER_EXPORT int __QAIC_HEADER(pxquake_touch)(remote_handle64 _h, const unsigned char* buf, int bufLen, int* status, int statusLen) __QAIC_HEADER_ATTRIBUTE;
/// draw one frame's spans: cmds = the command stream (see pxquake_imp.c), cache = the whole surface cache,
/// fb = the 8-bit frame buffer, zb = the 16-bit z buffer. Only the pixels inside the spans are written.
/// status: [0] 0x5A done, [1] spans, [2] pixels, [3] pcycles, [4] us, [5] out-of-bounds texels skipped,
///         [6] bad-op position (-1 = none), [7] surfaces, [8] out-of-bounds dest skipped
__QAIC_HEADER_EXPORT int __QAIC_HEADER(pxquake_spans)(remote_handle64 _h, const int* cmds, int cmdsLen, const unsigned char* cache, int cacheLen, unsigned char* fb, int fbLen, short* zb, int zbLen, int* status, int statusLen) __QAIC_HEADER_ATTRIBUTE;
/// give the clock votes back (called by the host before the session is closed). status[0] = 364
__QAIC_HEADER_EXPORT int __QAIC_HEADER(pxquake_release)(remote_handle64 _h, int* status, int statusLen) __QAIC_HEADER_ATTRIBUTE;
/// phase 5: start the persistent frame worker on the DSP: imports the host's dspqueue (id = lo | hi << 32) and
/// starts a DSP thread that waits on it (dspqueue_read with a timeout - no busy polling). status[0] = 0x71 ok.
__QAIC_HEADER_EXPORT int __QAIC_HEADER(pxquake_q_start)(remote_handle64 _h, int id_lo, int id_hi, int* status, int statusLen) __QAIC_HEADER_ATTRIBUTE;
/// phase 5: stop it: the thread is told to stop (flag + the host's stop packet), joined, the queue closed.
__QAIC_HEADER_EXPORT int __QAIC_HEADER(pxquake_q_stop)(remote_handle64 _h, int* status, int statusLen) __QAIC_HEADER_ATTRIBUTE;
/// phase 5 (dspqueue_write is dead on this runtime): the persistent worker over a MAILBOX in shared memory.
/// fds = every host buffer, mapped by the host with fastrpc_mmap(FASTRPC_MAP_FD); fds[0] = the mailbox page.
__QAIC_HEADER_EXPORT int __QAIC_HEADER(pxquake_mb_start)(remote_handle64 _h, const int* fds, int fdsLen, const int* sizes, int sizesLen, int* status, int statusLen) __QAIC_HEADER_ATTRIBUTE;
/// stop flag, thread joined, HAP_mmap_put of every fd.
__QAIC_HEADER_EXPORT int __QAIC_HEADER(pxquake_mb_stop)(remote_handle64 _h, int* status, int statusLen) __QAIC_HEADER_ATTRIBUTE;
#ifndef pxquake_URI
#define pxquake_URI "file:///libpxquake_skel.so?pxquake_skel_handle_invoke&_modver=1.0"
#endif /*pxquake_URI*/
#ifdef __cplusplus
}
#endif
#endif //_PXQUAKE_H
