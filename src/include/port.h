#pragma once

#define Quanta_API_Name "quanta"

#if (WIN32)
#include <Windows.h>
#define Path_Sep_S "\\"
#define Path_Sep '\\'
#define LibPrefix ""
#define ShareLibExt ".dll"
#define LOADLIB(path) LoadLibraryEx(path,NULL,LOAD_WITH_ALTERED_SEARCH_PATH)
#define GetProc(handle,funcName) GetProcAddress((HMODULE)handle, funcName)
#define UNLOADLIB(h) FreeLibrary((HMODULE)h)

#define SPRINTF sprintf_s
#define SCANF sscanf_s
#define MS_SLEEP(t) Sleep(t)
#define US_SLEEP(t) Sleep(t/1000)
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif // strcasecmp
#else
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <strings.h>

#define Path_Sep_S "/"
#define Path_Sep '/'
#define LibPrefix "lib"
#define ShareLibExt ".so"
#define LOADLIB(path) dlopen(path, RTLD_LAZY)
#define GetProc(handle,funcName) dlsym(handle, funcName)
#define UNLOADLIB(handle) dlclose(handle)

#define SPRINTF snprintf
#define SCANF sscanf
#define MS_SLEEP(t)  usleep((t)*1000)
#define US_SLEEP(t) usleep(t)
#endif

#if (WIN32)
#define SPRINTF sprintf_s
#else
#define SPRINTF snprintf
#endif

#define MAKE_I64(w,h,i64) i64=w;i64<<=32;i64&=0xFFFFFFFF00000000L;i64|=h;
#define Extract_From_I64(w,h,i64) (w) = (i64>>32);(h) = i64&0xFFFFFFFFL;

#define XLANG_TAG "xlang"