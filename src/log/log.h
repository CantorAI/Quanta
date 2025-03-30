#pragma once

#include "Locker.h"
#include "value.h"
#include <sstream> 
#include <string>

namespace Quanta
{
	class Log
	{
		Locker m_lock;
		X::Value m_realLogger;
	public:
		Log();
		~Log();

		void Init(X::Value& logger)
		{
			m_realLogger = logger;
		}
		template<typename T>
		inline Log& operator<<(const T& v)
		{
			if (m_level <= m_dumpLevel)
			{
				std::ostringstream oss;
				oss << v;
				std::string message = oss.str();
				m_realLogger(message);
			}
			return (Log&)*this;
		}
		inline void operator<<(Locker* l)
		{
			if (m_level <= m_dumpLevel)
			{
				m_realLogger('\n');
			}
			l->Unlock();
		}

		Log& SetCurInfo(const char* fileName, const int line, const int level);
		inline Locker* LineEnd()
		{
			return &m_lock;
		}
		inline Locker* End()
		{
			return &m_lock;
		}
		inline void LineBegin()
		{
			m_lock.Lock();
		}

		inline void SetDumpLevel(int l)
		{
			m_dumpLevel = l;
		}
		inline void SetLevel(int l)
		{
			m_level = l;
		}
	private:

		int m_level = 0;
		int m_dumpLevel = 999999; //All level will dump out
	};
	extern Log log;
	#define InitLog(logger) Galaxy::log.Init(logger)
	#define SetLogSizeLimit(l) Galaxy::log.SetFileSizeLimit(l)
	#define SetLogLevel(l) Galaxy::log.SetDumpLevel(l)
	#define LOGV(level) Galaxy::log.SetCurInfo(__FILE__,__LINE__,level)
	#define LOG LOGV(0)
	#define LOG1 LOGV(1)
	#define LOG2 LOGV(2)
	#define LOG3 LOGV(3)
	#define LOG4 LOGV(4)
	#define LOG5 LOGV(5)
	#define LOG6 LOGV(6)
	#define LOG7 LOGV(7)
	#define LOG8 LOGV(8)
	#define LOG9 LOGV(9)
	#define LINE_END Galaxy::log.LineEnd()
	#define LOG_END Galaxy::log.End()

	// ANSI color codes for console
	#define LOG_RED "\033[31m"   // Red color
	#define LOG_GREEN "\033[32m" // Green color
	#define LOG_YELLOW "\033[33m" // Yellow color
	#define LOG_BLUE "\033[34m"  // Blue color
	#define LOG_RESET "\033[0m"  // Reset to default
}