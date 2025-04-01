#pragma once
#include "xpackage.h"

namespace Quanta
{
	class HnswVdb;
	class Vdb
	{
		HnswVdb* m_vdb = nullptr;
	public:
		BEGIN_PACKAGE(Vdb)
			APISET().AddFunc<1>("Save", &Vdb::Save);
			APISET().AddFunc<1>("Load", &Vdb::Load);
			APISET().AddFunc<2>("AddVector", &Vdb::AddVector);
			APISET().AddFunc<2>("Lookup", &Vdb::Lookup);
		END_PACKAGE

	public:
		Vdb(int dimension, long long maxElements);
		virtual ~Vdb();
		bool Save(const std::string& fileName);
		bool Load(const std::string& fileName);
		X::Value Lookup(X::Value& vec, int topK);
		void AddVector(unsigned long long id, X::Value& vec);
	};
}