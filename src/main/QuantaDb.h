// quantadb.h
#pragma once
#include "singleton.h"
#include <string>
#include "value.h"

namespace Quanta
{
    class QuantaDb :
        public Singleton<QuantaDb>
    {
    private:
        X::Value m_sqlite;
        X::Value m_db;
        X::Value m_statment;
        X::Value m_status_ROW;  // SQLROW
        bool CheckTableExist(std::string tableName);
        void BuildTables();
    public:
        X::Value& Sqlite() { return m_sqlite; }
        X::Value& Db() { return m_db; }
        X::Value& Statment() { return m_statment; }
        void Start(std::string& exePath);
        void Close();

        // File system scanning related methods
        bool AddFile(std::string filePath, long long fileSize, std::string nodeId, std::string metadata);
        X::Value QueryFilesByNodeId(std::string nodeId);
        X::Value QueryFilesByPath(std::string pathPattern);
        bool RemoveFile(std::string filePath, std::string nodeId);
        bool UpdateFileMetadata(std::string filePath, std::string nodeId, std::string metadata);
    };
}