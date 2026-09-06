#include "QuantaDb.h"
#include "quanta_runtime.h"
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
            std::filesystem::create_directories(dbFolder);
        }
        std::string dbName = dbFolder + Path_Sep_S + "quantastore.db";
        auto sqlite = Import(Host(), "xlang_sqlite3", "sqlite");
        m_db = Invoke(sqlite["Database"], dbName);
        m_statment = m_db["statement"];
        m_sqlite = sqlite;
        m_status_ROW = m_sqlite["ROW"];
        BuildTables();
    }

    void QuantaDb::Close()
    {
        m_statment = X::Value();
        if (m_db.IsObject()) Invoke(m_db["close"]);
        m_db = X::Value();
        m_sqlite = X::Value();
    }

    bool QuantaDb::CheckTableExist(std::string tableName)
    {
        bool bHave = false;
        X::Value stat = Invoke(m_statment, "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = ?");
        Invoke(stat["bind"], 1, tableName);
        if (Invoke(stat["step"]) == m_status_ROW)
        {
            int cnt = static_cast<int>(Invoke(stat["get"], 0).ToLongLong());
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
            Invoke(ExecSQL, "CREATE TABLE \"DfsFiles\" (\
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
            Invoke(ExecSQL, "CREATE TABLE \"ScanHistory\" (\
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
        X::Value stat = Invoke(m_statment, "INSERT OR REPLACE INTO DfsFiles (FilePath, NodeId, FileSize, ModifiedTime, CreatedTime, Metadata, LastScannedTime) VALUES (?, ?, ?, ?, ?, ?, ?)");
        Invoke(stat["bind"], 1, filePath);
        Invoke(stat["bind"], 2, nodeId);
        Invoke(stat["bind"], 3, fileSize);

        // Get current time for modification and creation if not provided
        long long currentTime = getCurMilliTimeStamp();

        Invoke(stat["bind"], 4, currentTime);  // ModifiedTime - would be better to get from file system
        Invoke(stat["bind"], 5, currentTime);  // CreatedTime - would be better to get from file system
        Invoke(stat["bind"], 6, metadata);
        Invoke(stat["bind"], 7, currentTime);  // LastScannedTime

        bool result = Invoke(stat["step"]) != m_status_ROW;
        return result;
    }

    X::Value QuantaDb::QueryFilesByNodeId(std::string nodeId)
    {
        X::Value results;
        X::Value stat = Invoke(m_statment, "SELECT FilePath, FileSize, ModifiedTime, CreatedTime, Metadata, LastScannedTime FROM DfsFiles WHERE NodeId = ? ORDER BY FilePath");
        Invoke(stat["bind"], 1, nodeId);

        auto fileList = X::Value::List(Host());
        while (Invoke(stat["step"]) == m_status_ROW)
        {
            auto fileInfo = X::Value::Dict(Host());
            fileInfo.SetItem("FilePath", Invoke(stat["get"], 0));
            fileInfo.SetItem("FileSize", Invoke(stat["get"], 1));
            fileInfo.SetItem("ModifiedTime", Invoke(stat["get"], 2));
            fileInfo.SetItem("CreatedTime", Invoke(stat["get"], 3));
            fileInfo.SetItem("Metadata", Invoke(stat["get"], 4));
            fileInfo.SetItem("LastScannedTime", Invoke(stat["get"], 5));

            fileList.Append(fileInfo);
        }

        results = fileList;
        return results;
    }

    void QuantaDb::EnumFiles(std::function<void(std::string& filePath)> cb)
    {
        X::Value results;
        X::Value stat = Invoke(m_statment, "SELECT FilePath FROM DfsFiles");

        auto fileList = X::Value::List(Host());
        while (Invoke(stat["step"]) == m_status_ROW)
        {
			std::string filePath = Invoke(stat["get"], 0).ToString();
			cb(filePath);
        }
    }

    X::Value QuantaDb::QueryFilesByPath(std::string pathPattern)
    {
        X::Value results;
        X::Value stat = Invoke(m_statment, "SELECT FilePath, NodeId, FileSize, ModifiedTime, CreatedTime, Metadata, LastScannedTime FROM DfsFiles WHERE FilePath LIKE ? ORDER BY NodeId, FilePath");
        Invoke(stat["bind"], 1, "%" + pathPattern + "%");

        auto fileList = X::Value::List(Host());
        while (Invoke(stat["step"]) == m_status_ROW)
        {
            auto fileInfo = X::Value::Dict(Host());
            fileInfo.SetItem("FilePath", Invoke(stat["get"], 0));
            fileInfo.SetItem("NodeId", Invoke(stat["get"], 1));
            fileInfo.SetItem("FileSize", Invoke(stat["get"], 2));
            fileInfo.SetItem("ModifiedTime", Invoke(stat["get"], 3));
            fileInfo.SetItem("CreatedTime", Invoke(stat["get"], 4));
            fileInfo.SetItem("Metadata", Invoke(stat["get"], 5));
            fileInfo.SetItem("LastScannedTime", Invoke(stat["get"], 6));

            fileList.Append(fileInfo);
        }

        results = fileList;
        return results;
    }

    bool QuantaDb::RemoveFile(std::string filePath, std::string nodeId)
    {
        X::Value stat = Invoke(m_statment, "DELETE FROM DfsFiles WHERE FilePath = ? AND NodeId = ?");
        Invoke(stat["bind"], 1, filePath);
        Invoke(stat["bind"], 2, nodeId);

        bool result = Invoke(stat["step"]) != m_status_ROW;
        return result;
    }

    bool QuantaDb::UpdateFileMetadata(std::string filePath, std::string nodeId, std::string metadata)
    {
        X::Value stat = Invoke(m_statment, "UPDATE DfsFiles SET Metadata = ?, LastScannedTime = ? WHERE FilePath = ? AND NodeId = ?");
        Invoke(stat["bind"], 1, metadata);
        Invoke(stat["bind"], 2, getCurMilliTimeStamp());
        Invoke(stat["bind"], 3, filePath);
        Invoke(stat["bind"], 4, nodeId);

        bool result = Invoke(stat["step"]) != m_status_ROW;
        return result;
    }
}
