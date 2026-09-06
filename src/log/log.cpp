#include "log.h"
#include "port.h"
#include "help_func.h"

Quanta::Log Quanta::log;

Quanta::Log::Log()
{
}

Quanta::Log::~Log()
{

}

Quanta::Log& Quanta::Log::SetCurInfo(const char* fileName,
	const int line, const int level)
{
	m_lock.Lock();
	m_level = level;
	if (m_level <= m_dumpLevel)
	{
		std::string strFileName(fileName);
		auto pos = strFileName.rfind(Path_Sep_S);
		if (pos != std::string::npos)
		{
			strFileName = strFileName.substr(pos + 1);
		}
		unsigned long pid = GetPID();
		unsigned long tid = GetThreadID();
		int64_t ts = getCurTimeStamp();

		const int buf_Len = 1000;
		char szFilter[buf_Len];
		SPRINTF(szFilter, buf_Len, "[%d-%d-%llu,%s:%d] ", pid, tid, ts, strFileName.c_str(), line);
		std::clog << szFilter;
	}
	return *this;
}
