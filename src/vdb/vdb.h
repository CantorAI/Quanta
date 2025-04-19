#pragma once
#include "xpackage.h"

namespace Quanta
{
	class HnswVdb;
	class VectorDatabase;
	class Vdb
	{
		VectorDatabase* m_vdb = nullptr;
		HnswVdb* m_index = nullptr;
	public:
		BEGIN_PACKAGE(Vdb)
			APISET().AddVarFunc("Init", &Vdb::Init);
			APISET().AddFunc<1>("Save", &Vdb::Save);
			APISET().AddFunc<1>("Load", &Vdb::Load);
			APISET().AddVarFunc("AddVectors", &Vdb::AddVectors);
			APISET().AddFunc<2>("Lookup", &Vdb::Lookup);
		END_PACKAGE

	public:
		Vdb(X::ARGS& params, X::KWARGS& kwParams);
		bool Init(X::XRuntime* rt, X::XObj* pContext,
			X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue);
		virtual ~Vdb();
		bool Save(const std::string& fileName);
		bool Load(const std::string& fileName);
		X::Value Lookup(X::Value& vec, int topK);
		void AddVectors(X::XRuntime* rt, X::XObj* pContext,
			X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue);
	};
}