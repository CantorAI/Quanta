#include "vdb.h"
#include "HnswVdb.h"

namespace Quanta
{
	Vdb::Vdb(int dimension, long long maxElements)
	{
		m_vdb = new HnswVdb(dimension, maxElements);
	}
	Vdb::~Vdb()
	{
		if (m_vdb)
		{
			delete m_vdb;
			m_vdb = nullptr;
		}
	}
	bool Vdb::Save(const std::string& fileName)
	{
		if (m_vdb)
		{
			m_vdb->Save(fileName);
			return true;
		}
		return false;
	}
	bool Vdb::Load(const std::string& fileName)
	{
		if (m_vdb)
		{
			m_vdb->Load(fileName);
			return true;
		}
		return false;
	}
	X::Value Vdb::Lookup(X::Value& vec, int topK)
	{
		if (m_vdb && vec.IsTensor())
		{
			X::Tensor vecTensor(vec);
			std::vector<float> vecData(vecTensor->GetCount());
			memcpy(vecData.data(), vecTensor->GetData(), vecTensor->GetCount() * sizeof(float));
			auto results = m_vdb->Lookup(vecData, topK);
			X::List list;
			for (const auto& result : results)
			{
				X::List item;
				item += result.first;
				item += result.second;
				list->AddItem(item);
			}
			return list;
		}
		return X::Value();
	}
	void Vdb::AddVector(unsigned long long id, X::Value& vec)
	{
		if (m_vdb && vec.IsTensor())
		{
			X::Tensor vecTensor(vec);
			std::vector<float> vecData(vecTensor->GetCount());
			memcpy(vecData.data(), vecTensor->GetData(), vecTensor->GetCount() * sizeof(float));
			m_vdb->AddVector(id, vecData);
		}
	}
}