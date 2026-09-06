#include "vdb.h"
#include "VectorDatabase.h"
#include "HnswVdb.h"
#include "vector_input.h"

namespace Quanta {
X::Value Vdb::Init(const X::ARGS& args, const X::KWARGS& kwargs) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto integer = [&](const char* name, long long fallback) {
        auto value = Keyword(kwargs, name);
        return value ? value->ToLongLong() : fallback;
    };
    int dimension = static_cast<int>(integer("dimension", args.empty() ? 1024 : args[0].ToLongLong()));
    long long capacity = integer("max_elements", 1000000);
    int m = static_cast<int>(integer("M", 16));
    int construction = static_cast<int>(integer("ef_construction", 200));
    int search = static_cast<int>(integer("ef_search", 50));
    auto metric = Keyword(kwargs, "space");
    std::string space = metric ? metric->ToString() : "l2";
    if (dimension <= 0 || capacity <= 0 || m < 2 || construction <= 0 || search <= 0)
        throw X::Error("Invalid vector database dimensions or index parameters");
    auto database = std::make_unique<VectorDatabase>(dimension);
    database->AddParameter("dimension", dimension);
    database->AddParameter("space", space);
    database->AddParameter("max_elements", capacity);
    database->AddParameter("M", m);
    database->AddParameter("ef_construction", construction);
    database->AddParameter("ef_search", search);
    auto index = std::make_unique<HnswVdb>(space, dimension, static_cast<size_t>(capacity), m, construction, search);
    delete m_index;
    delete m_vdb;
    m_index = index.release();
    m_vdb = database.release();
    return X::Value(true);
}

Vdb::~Vdb() { delete m_index; delete m_vdb; }

bool Vdb::Save(const std::string& filename) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!m_vdb || !m_index) return false;
    if (!m_vdb->Save(filename)) return false;
    m_index->Save(filename);
    return true;
}

bool Vdb::Load(const std::string& filename) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto database = std::make_unique<VectorDatabase>(0);
    if (!database->Load(filename)) return false;
    auto index = std::make_unique<HnswVdb>(
        database->GetParam("space", std::string("l2")), database->GetDimension(),
        database->GetParam<size_t>("max_elements", 10000), database->GetParam("M", 16),
        database->GetParam("ef_construction", 200), database->GetParam("ef_search", 50));
    index->Load(filename);
    delete m_index;
    delete m_vdb;
    m_index = index.release();
    m_vdb = database.release();
    return true;
}

X::Value Vdb::Lookup(const X::Value& vector, int topK) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!m_index) throw X::Error("Vector database is not initialized");
    FloatInput input(vector);
    auto results = m_index->Lookup(input.data(), input.size(), topK);
    auto output = X::Value::List(Host());
    for (const auto& result : results) {
        auto item = X::Value::List(Host());
        auto id = m_vdb->GetIdByIndex(result.first);
        item.Append(id);
        item.Append(result.second);
        item.Append(m_vdb->GetTextById(id));
        output.Append(item);
    }
    return output;
}

X::Value Vdb::AddVectors(const X::ARGS& args, const X::KWARGS& kwargs) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!m_index || args.size() < 2) throw X::Error("AddVectors requires IDs and vectors");
    FloatInput input(args[1]);
    const auto dimension = static_cast<size_t>(m_vdb->GetDimension());
    if (!input.size() || input.size() % dimension) throw X::Error("Vector dimensions do not match the database");
    const size_t count = input.size() / dimension;
    if (count > m_index->GetMaxElements() - m_index->GetCurrentCount())
        throw X::Error("Vector database capacity exceeded");
    std::vector<unsigned long long> ids(count);
    if (IsSequence(args[0])) {
        if (args[0].Size() != count) throw X::Error("ID count must match vector count");
        for (size_t i = 0; i < count; ++i) ids[i] = args[0].Get(i).ToLongLong();
    } else if (args[0].IsInt64()) {
        const auto start = args[0].ToLongLong();
        if (start < 0 || count - 1 > static_cast<uint64_t>(INT64_MAX - start))
            throw X::Error("Vector IDs exceed the supported range");
        for (size_t i = 0; i < count; ++i) ids[i] = start + i;
    } else throw X::Error("IDs must be an integer or a list");
    std::vector<std::string> texts(count);
    if (auto chunks = Keyword(kwargs, "chunks")) {
        if (IsSequence(*chunks) && chunks->Size() != count) throw X::Error("Chunk count must match vector count");
        for (size_t i = 0; i < count; ++i) {
            auto text = IsSequence(*chunks) ? chunks->Get(i) : *chunks;
            texts[i] = IsMapping(text) ? text["chunk"].ToString() : text.ToString();
        }
    }
    auto threads = Keyword(kwargs, "num_threads");
    auto indices = m_vdb->AddLabels(ids, texts);
    m_index->AddVectors(indices, input.data(), input.size(), threads ? static_cast<int>(threads->ToLongLong()) : -1);
    return X::Value(ids.back());
}
}
