#ifndef WIN_MSVC_H
#define WIN_MSVC_H

#if defined(_MSC_VER)

#ifndef _SSIZE_T
#define _SSIZE_T
#define ssize_t long
#endif

#endif

#endif /* WIN_MSVC_H */
