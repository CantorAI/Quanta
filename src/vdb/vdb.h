#pragma once
#include "quanta_runtime.h"

namespace Quanta
{
	class HnswVdb;
	class VectorDatabase;
	class Vdb
	{
		VectorDatabase* m_vdb = nullptr;
		HnswVdb* m_index = nullptr;
		std::mutex mutex_;
	public:
		BEGIN_PACKAGE(Vdb)
			APISET().AddVarFunc("Init", &Vdb::Init);
			APISET().AddFunc<1>("Save", &Vdb::Save);
			APISET().AddFunc<1>("Load", &Vdb::Load);
			APISET().AddVarFunc("AddVectors", &Vdb::AddVectors);
			APISET().AddFunc<2>("Lookup", &Vdb::Lookup);
		END_PACKAGE

	public:
		Vdb() = default;
		X::Value Init(const X::ARGS& params, const X::KWARGS& kwParams);
		virtual ~Vdb();
		bool Save(const std::string& fileName);
		bool Load(const std::string& fileName);
		X::Value Lookup(const X::Value& vec, int topK);
		X::Value AddVectors(const X::ARGS& params, const X::KWARGS& kwParams);
	};
}
