#include "quantadb.h"
#include "xpackage.h"
#include "port.h"
#include "QuantaHost.h"
#include "help_func.h"

namespace Quanta
{
    void QuantaDb::Start(std::string& exePath)
    {
        std::string dbFolder = exePath + Path_Sep_S + "QuantaDB";
        if (isDir(dbFolder.c_str()) == false)
        {
            _mkdir(dbFolder.c_str());
        }
        std::string dbName = dbFolder + Path_Sep_S + "quantastore.db";
        X::Package sqlite(QuantaHost::I().RT(), "sqlite", "xlang_sqlite");
        m_db = sqlite["Database"](dbName);
        m_statment = m_db["statement"];
        m_sqlite = sqlite;
        m_status_ROW = m_sqlite["ROW"];
        BuildTables();
    }

    void QuantaDb::Close()
    {
        // Close database connection if needed
    }

    bool QuantaDb::CheckTableExist(std::string tableName)
    {
        bool bHave = false;
        X::Value stat = m_statment("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = ?");
        stat["bind"](1, tableName);
        if (stat["step"]() == m_status_ROW)
        {
            int cnt = stat["get"](0);
            bHave = (cnt > 0);
        }
        return bHave;
    }

    void QuantaDb::BuildTables()
    {
        auto ExecSQL = m_db["exec"];

        // DfsFiles table for file system scanning
        if (!CheckTableExist("DfsFiles"))
        {
            ExecSQL("CREATE TABLE \"DfsFiles\" (\
                \"FilePath\" TEXT,\
                \"NodeId\" TEXT,\
                \"FileSize\" INTEGER,\
                \"ModifiedTime\" INTEGER,\
                \"CreatedTime\" INTEGER,\
                \"Metadata\" TEXT,\
                \"LastScannedTime\" INTEGER,\
                PRIMARY KEY(\"FilePath\", \"NodeId\")\
            )");
        }

        // Scan history table
        if (!CheckTableExist("ScanHistory"))
        {
            ExecSQL("CREATE TABLE \"ScanHistory\" (\
                \"NodeId\" TEXT,\
                \"StartTime\" INTEGER,\
                \"EndTime\" INTEGER,\
                \"TotalFiles\" INTEGER,\
                \"TotalSize\" INTEGER,\
                \"ScanPath\" TEXT,\
                PRIMARY KEY(\"NodeId\", \"StartTime\")\
            )");
        }
    }

    bool QuantaDb::AddFile(std::string filePath, long long fileSize, std::string nodeId, std::string metadata)
    {
        X::Value stat = m_statment("INSERT OR REPLACE INTO DfsFiles (FilePath, NodeId, FileSize, ModifiedTime, CreatedTime, Metadata, LastScannedTime) VALUES (?, ?, ?, ?, ?, ?, ?)");
        stat["bind"](1, filePath);
        stat["bind"](2, nodeId);
        stat["bind"](3, fileSize);

        // Get current time for modification and creation if not provided
        long long currentTime = getCurMilliTimeStamp();

        stat["bind"](4, currentTime);  // ModifiedTime - would be better to get from file system
        stat["bind"](5, currentTime);  // CreatedTime - would be better to get from file system
        stat["bind"](6, metadata);
        stat["bind"](7, currentTime);  // LastScannedTime

        bool result = stat["step"]() != m_status_ROW;
        return result;
    }

    X::Value QuantaDb::QueryFilesByNodeId(std::string nodeId)
    {
        X::Value results;
        X::Value stat = m_statment("SELECT FilePath, FileSize, ModifiedTime, CreatedTime, Metadata, LastScannedTime FROM DfsFiles WHERE NodeId = ? ORDER BY FilePath");
        stat["bind"](1, nodeId);

        X::List fileList;
        while (stat["step"]() == m_status_ROW)
        {
            X::Dict fileInfo;
            fileInfo->Set("FilePath", stat["get"](0));
            fileInfo->Set("FileSize", stat["get"](1));
            fileInfo->Set("ModifiedTime", stat["get"](2));
            fileInfo->Set("CreatedTime", stat["get"](3));
            fileInfo->Set("Metadata", stat["get"](4));
            fileInfo->Set("LastScannedTime", stat["get"](5));

            fileList->AddItem(fileInfo);
        }

        results = fileList;
        return results;
    }

    X::Value QuantaDb::QueryFilesByPath(std::string pathPattern)
    {
        X::Value results;
        X::Value stat = m_statment("SELECT FilePath, NodeId, FileSize, ModifiedTime, CreatedTime, Metadata, LastScannedTime FROM DfsFiles WHERE FilePath LIKE ? ORDER BY NodeId, FilePath");
        stat["bind"](1, "%" + pathPattern + "%");

        X::List fileList;
        while (stat["step"]() == m_status_ROW)
        {
            X::Dict fileInfo;
            fileInfo->Set("FilePath", stat["get"](0));
            fileInfo->Set("NodeId", stat["get"](1));
            fileInfo->Set("FileSize", stat["get"](2));
            fileInfo->Set("ModifiedTime", stat["get"](3));
            fileInfo->Set("CreatedTime", stat["get"](4));
            fileInfo->Set("Metadata", stat["get"](5));
            fileInfo->Set("LastScannedTime", stat["get"](6));

            fileList->AddItem(fileInfo);
        }

        results = fileList;
        return results;
    }

    bool QuantaDb::RemoveFile(std::string filePath, std::string nodeId)
    {
        X::Value stat = m_statment("DELETE FROM DfsFiles WHERE FilePath = ? AND NodeId = ?");
        stat["bind"](1, filePath);
        stat["bind"](2, nodeId);

        bool result = stat["step"]() != m_status_ROW;
        return result;
    }

    bool QuantaDb::UpdateFileMetadata(std::string filePath, std::string nodeId, std::string metadata)
    {
        X::Value stat = m_statment("UPDATE DfsFiles SET Metadata = ?, LastScannedTime = ? WHERE FilePath = ? AND NodeId = ?");
        stat["bind"](1, metadata);
        stat["bind"](2, getCurMilliTimeStamp());
        stat["bind"](3, filePath);
        stat["bind"](4, nodeId);

        bool result = stat["step"]() != m_status_ROW;
        return result;
    }
}