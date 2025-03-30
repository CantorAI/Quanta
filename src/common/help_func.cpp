#include "help_func.h"
#include <sstream>
#include <vector>
#include "wildcard.h"
#include <chrono>
#include "port.h"
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace Quanta
{
	bool dir(std::string search_path,
		std::vector<std::string>& subfolders,
		std::vector<std::string>& files)
	{
		bool ret = false;
#if (WIN32)
		BOOL result = TRUE;
		WIN32_FIND_DATA ff;
		std::string search_pat = search_path + Path_Sep_S + "*.*";
		HANDLE findhandle = FindFirstFile(search_pat.c_str(), &ff);
		if (findhandle != INVALID_HANDLE_VALUE)
		{
			ret = true;
			BOOL res = TRUE;
			while (res)
			{
				// We only want directories
				if (ff.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				{
					std::string fileName(ff.cFileName);
					if (fileName != "." && fileName != "..")
					{
						subfolders.push_back(fileName);
					}
				}
				else
				{
					std::string fileName(ff.cFileName);
					files.push_back(fileName);
				}
				res = FindNextFile(findhandle, &ff);
			}
			FindClose(findhandle);
		}
#else
		DIR* dir;
		struct dirent* ent;
		if ((dir = opendir(search_path.c_str())) != NULL)
		{
			ret = true;
			while ((ent = readdir(dir)) != NULL)
			{
				if (ent->d_type == DT_DIR)
				{
					std::string fileName(ent->d_name);
					if (fileName != "." && fileName != "..")
					{
						subfolders.push_back(fileName);
					}
				}
				else if (ent->d_type == DT_REG)//A regular file
				{
					std::string fileName(ent->d_name);
					files.push_back(fileName);
				}
			}
			closedir(dir);
		}
#endif
		return ret;
	}
	bool file_search(std::string folder,
		std::string fileName,
		std::vector<std::string>& outFiles,
		bool findAll)
	{
		bool bFind = false;
		std::vector<std::string> subfolders;
		std::vector<std::string> files;
		bool bOK = dir(folder, subfolders, files);
		if (bOK)
		{
			for (auto& f : files)
			{
				if (f == fileName)
				{
					outFiles.push_back(folder + Path_Sep_S + f);
					bFind = true;
					break;
				}
			}
			for (auto& fd : subfolders)
			{
				bool bRet = file_search(folder + Path_Sep_S + fd, fileName, outFiles, findAll);
				if (bRet)
				{
					bFind = true;
					if (!findAll)
					{
						break;
					}
				}
			}
		}
		return bFind;
	}
	bool file_search_filter(std::string folder,
		std::string filter,
		std::vector<std::string>& outFiles)
	{
		bool bFind = false;
		std::vector<std::string> subfolders;
		std::vector<std::string> files;
		bool bOK = dir(folder, subfolders, files);
		if (bOK)
		{
			for (auto& f : files)
			{
				bool matched = wildcard(filter.c_str(), f.c_str());
				if (matched)
				{
					outFiles.push_back(folder + Path_Sep_S + f);
					bFind = true;
				}
			}
			for (auto& fd : subfolders)
			{
				bool bRet = file_search_filter(folder + Path_Sep_S + fd, filter, outFiles);
				if (bRet)
				{
					bFind = true;
				}
			}
		}
		return bFind;
	}
	std::vector<std::string> SplitStr(const std::string& str, char delim)
	{
		std::vector<std::string> list;
		std::string temp;
		std::stringstream ss(str);
		while (std::getline(ss, temp, delim))
		{
			list.push_back(temp);
		}
		return list;
	}
	bool SearchDll(std::string& dllMainName, std::string& searchPath,
		std::string& findFileName)
	{
		bool bHaveDll = false;
		std::vector<std::string> candiateFiles;
		bool bRet = file_search(searchPath,
			LibPrefix+dllMainName + ShareLibExt, candiateFiles,false);
		if (bRet && candiateFiles.size() > 0)
		{
			findFileName = candiateFiles[0];
			bHaveDll = true;
		}
		return bHaveDll;
	}
	long long getCurMicroTimeStamp()
	{
		auto current_time = std::chrono::high_resolution_clock::now();
		long long current_time_ll = std::chrono::time_point_cast<std::chrono::microseconds>(current_time).time_since_epoch().count();
		return current_time_ll;
	}
	//unit is 100 nanoseconds, max precision is 1/10 microseconds
// in system clock with high_resolution_clock
	long long getCurTimeStamp()
	{
		auto current_time = std::chrono::high_resolution_clock::now();
		long long current_time_ll = std::chrono::time_point_cast<std::chrono::nanoseconds>(current_time).time_since_epoch().count();
		return current_time_ll / 100;
	}
	long long getCurMilliTimeStamp()
	{
#if (WIN32)
		return (long long)GetTickCount64();
#else
		struct timeval tv;
		gettimeofday(&tv, NULL);

		return tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
	}

	unsigned long GetPID()
	{
		unsigned long processId = 0;
#if (WIN32)
		processId = GetCurrentProcessId();
#else
		processId = getpid();
#endif
		return processId;
	}
	unsigned long GetThreadID()
	{
		unsigned long tid = 0;
#if (WIN32)
		tid = ::GetCurrentThreadId();
#elif defined(__APPLE__)
		tid = (unsigned long)mach_thread_self();
		mach_port_deallocate(mach_task_self(), tid);
#else
#include <sys/types.h>
#include <unistd.h>
		tid = gettid();
#endif
		return tid;
	}

	std::string UIDToString(UID uid)
	{
		char id[32 + 1];
		id[32] = 0;
		snprintf(id, sizeof(id), "%016llx%016llx", uid.h, uid.l);
		return id;
	}
	void ReplaceAll(std::string& data, std::string toSearch, std::string replaceStr)
	{
		// Get the first occurrence
		size_t pos = data.find(toSearch);
		// Repeat till end is reached
		while (pos != std::string::npos)
		{
			// Replace this occurrence of Sub String
			data.replace(pos, toSearch.size(), replaceStr);
			// Get the next occurrence from the current position
			pos = data.find(toSearch, pos + replaceStr.size());
		}
	}

	UID UIDFromString(std::string id)
	{
		ReplaceAll(id, "-", "");
		UID uid;
		int retVal = sscanf(id.c_str(), "%016llx%016llx", &uid.h, &uid.l);
		if (retVal != 2)
		{
			uid.h = uid.l = 0;
		}
		return uid;
	}
	unsigned long long byteStringToNumber(const char* strBytes, int size)//like 'xcvb'
	{
		if (size > (int)sizeof(unsigned long long))
		{
			size = sizeof(unsigned long long);
		}
		unsigned long long t = 0;
		for (int i = 0; i < size; i++)
		{
			auto c = strBytes[i];
			if (c < 256)
			{
				t = (t << 8) | c;
			}
		}
		return t;
	}
	std::string tostring(unsigned long long x)
	{
		const int buf_len = 1000;
		char strBuf[buf_len];
		SPRINTF(strBuf, buf_len, "%llu", x);
		return strBuf;
	}
	std::string tostring(long x)
	{
		const int buf_len = 1000;
		char strBuf[buf_len];
		SPRINTF(strBuf, buf_len, "%ld", x);
		return strBuf;
	}
	std::string getFileExtension(const std::string& fileName) 
	{
		size_t dotPosition = fileName.find_last_of(".");
		if (dotPosition != std::string::npos) 
		{
			return fileName.substr(dotPosition + 1);
		}
		return "";
	}
	bool isAbsolutePath(const std::string& path) {
		if (path.empty()) return false;

		// Check for Unix/Linux absolute path
		if (path[0] == '/') {
			return true;
		}

		// Check for Windows absolute path (e.g., C:\Folder)
		if (path.length() > 2 && std::isalpha(path[0]) && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
			return true;
		}

		// Check for Windows network path (e.g., \\Server\Share)
		if (path.length() > 1 && path[0] == '\\' && path[1] == '\\') {
			return true;
		}

		return false;
	}
	bool isDir(const std::string& name)
	{
		bool yes = false;
		struct stat buffer;
		if (stat(name.c_str(), &buffer) == 0)
		{
			if (buffer.st_mode & S_IFDIR)
			{
				yes = true;
			}
		}
		return yes;
	}
#if (WIN32)
#include <Windows.h>
	void _mkdir(const char* dir)
	{
		CreateDirectory(dir, NULL);
	}
#else
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <limits.h>
#include <cstring>	
#include <spawn.h>
#include <sys/wait.h>
#include <sys/utsname.h>
	extern char** environ; // Required for posix_spawnp


	void _mkdir(const char* dir)
	{
		int state = access(dir, R_OK | W_OK);
		if (state != 0)
		{
			mkdir(dir, S_IRWXU);
		}
	}
#endif
}