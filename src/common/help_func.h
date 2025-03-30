#pragma once
#include <vector>
#include <string>

#if (WIN32)
#include <Windows.h>

#define Path_Sep_S "\\"
#define Path_Sep '\\'
#define LibPrefix ""
#define ShareLibExt ".dll"
#define LOADLIB(path) LoadLibraryEx(path,NULL,LOAD_WITH_ALTERED_SEARCH_PATH)
#define GetProc(handle,funcName) GetProcAddress((HMODULE)handle, funcName)
#define UNLOADLIB(h) FreeLibrary((HMODULE)h)
#else
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/time.h>

#define Path_Sep_S "/"
#define Path_Sep '/'
#define LibPrefix "lib"
#define ShareLibExt ".so"
#define LOADLIB(path) dlopen(path, RTLD_LAZY)
#define GetProc(handle,funcName) dlsym(handle, funcName)
#define UNLOADLIB(handle) dlclose(handle)
#endif

#include "common.h"

namespace Quanta
{
	bool dir(std::string search_path,
		std::vector<std::string>& subfolders,
		std::vector<std::string>& files);
	bool file_search(std::string folder,
		std::string fileName,
		std::vector<std::string>& outFiles,
		bool findAll);
	bool file_search_filter(std::string folder,
		std::string filter,
		std::vector<std::string>& outFiles);
	std::vector<std::string> SplitStr(const std::string& str, char delim);
	bool SearchDll(std::string& dllMainName, std::string& searchPath,
		std::string& findFileName);
	long long getCurMilliTimeStamp();
	long long getCurMicroTimeStamp();
	std::string UIDToString(UID uid);
	UID UIDFromString(std::string id);
	unsigned long long byteStringToNumber(const char* strBytes, int size);
	std::string tostring(unsigned long long x);
	std::string tostring(long x);
	std::string getFileExtension(const std::string& fileName);
	long long getCurTimeStamp();
	unsigned long GetPID();
	unsigned long GetThreadID();
	bool isAbsolutePath(const std::string& path);
}