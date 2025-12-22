#include "vdb.h"
#include "VectorDatabase.h"
#include "HnswVdb.h"

namespace Quanta
{
	Vdb::Vdb(X::ARGS& params, X::KWARGS& kwParams)
	{
		if (params.size() == 0 && kwParams.size())
		{
			//need to call load and init vdb
			return;
		}
		X::Value retValue;
		Init(nullptr, nullptr, params, kwParams, retValue);
	}

	bool Vdb::Init(X::XRuntime* rt, X::XObj* pContext,
		X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue)
	{
		// 1) defaults
		std::string spaceName = "l2";
		int         dimension = 1024;
		size_t      maxElements = 1000000;
		int         M = 16;
		int         efConstruction = 200;
		int         efSearch = 50;

		// 2) positional override for dimension
		if (params.size() >= 1) {
			dimension = params[0].ToInt();
		}

		// 3) keyword overrides
		if (auto it = kwParams.find("dimension"); it) {
			dimension = it->val.ToInt();
		}
		m_vdb = new VectorDatabase(dimension);

		m_vdb->AddParameter("dimension", dimension);

		if (auto it = kwParams.find("space"); it) {
			spaceName = it->val.ToString();
			m_vdb->AddParameter("space", spaceName);
		}
		if (auto it = kwParams.find("max_elements"); it) {
			maxElements = static_cast<size_t>(it->val.ToInt());
			m_vdb->AddParameter("max_elements", maxElements);
		}
		if (auto it = kwParams.find("M"); it) {
			M = it->val.ToInt();
			m_vdb->AddParameter("M", M);
		}
		if (auto it = kwParams.find("ef_construction"); it) {
			efConstruction = it->val.ToInt();
			m_vdb->AddParameter("ef_construction", efConstruction);
		}
		if (auto it = kwParams.find("ef_search"); it) {
			efSearch = it->val.ToInt();
			m_vdb->AddParameter("ef_search", efSearch);
		}

		// 4) construct the index with all parameters
		m_index = new HnswVdb(
			spaceName,
			dimension,
			maxElements,
			M,
			efConstruction,
			efSearch
		);
		retValue = X::Value(true);
		return true;
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
		}
		if (m_index)
		{
			m_index->Save(fileName);
		}
		return true;
	}
	bool Vdb::Load(const std::string& fileName)
	{
		if (m_vdb)
		{
			delete m_vdb;
		}
		m_vdb = new VectorDatabase(0);
		m_vdb->Load(fileName);
		if (m_index)
		{
			delete m_index;
		}
		// get parameters from vdb
		std::string space_name = m_vdb->GetParam("space", "l2").ToString();
		int dimension = m_vdb->GetParam("dimension", 1024).ToInt();
		size_t max_elements = static_cast<size_t>(m_vdb->GetParam("max_elements", 10000).ToInt());
		int M = m_vdb->GetParam("M", 16).ToInt();
		int ef_construction = m_vdb->GetParam("ef_construction", 200).ToInt();
		int ef_search = m_vdb->GetParam("ef_search", 50).ToInt();
		// create the index
		m_index = new HnswVdb(
			space_name,
			dimension,
			max_elements,
			M,
			ef_construction,
			ef_search
		);
		m_index->Load(fileName);
		return true;
	}
	X::Value Vdb::Lookup(X::Value& vec, int topK)
	{
		if (m_vdb && vec.IsTensor())
		{
			X::Tensor vecT0(vec);
			X::Value vecValCont = vecT0->ToType(X::TensorDataType::FLOAT);
			X::Tensor vecTensor(vecValCont);

			std::vector<float> vecData(vecTensor->GetCount());
			memcpy(vecData.data(), vecTensor->GetData(), vecTensor->GetCount() * sizeof(float));
			auto results = m_index->Lookup(vecData, topK);
			X::List list;
			for (const auto& result : results)
			{
				X::List item;
				unsigned long long id = m_vdb->GetIdByIndex(result.first);
				item += id;
				item += result.second;
				item += m_vdb->GetTextById(id);
				list->AddItem(item);
			}
			return list;
		}
		return X::Value();
	}
	void Vdb::AddVectors(X::XRuntime* rt, X::XObj* pContext,
		X::ARGS& params, X::KWARGS& kwParams,
		X::Value& retValue)
	{
		// sanity checks
		if (!m_vdb || !m_index || params.size() < 2) {
			retValue = X::Value(false);
			return;
		}

		// --- 1) parse keywords ---
		int num_threads = -1;
		if (auto it = kwParams.find("num_threads")) {
			num_threads = it->val.ToInt();
		}
		X::Value chunksVal;
		if (auto it = kwParams.find("chunks")) {
			chunksVal = it->val;
		}

		// --- 2) get the flat float array & dims ---
		X::Value vecVal = params[1];
		if (!vecVal.IsTensor()) {
			retValue = X::Value(false);
			return;
		}
		X::Tensor vecT0(vecVal);
		X::Value vecValCont = vecT0->ToType(X::TensorDataType::FLOAT);
		X::Tensor vecT(vecValCont);

		long long totalCount = vecT->GetCount();
		int D = m_vdb->GetDimension();          // your VectorDatabase dim
		if (totalCount ==0 || D <= 0 || totalCount % D != 0) {
			retValue = X::Value(false);
			return;
		}
		size_t n = totalCount / D;
		const float* rawPtr = reinterpret_cast<const float*>(vecT->GetData());

		// --- 3) build external IDs vector ---
		std::vector<unsigned long long> extIds(n);
		X::Value idsVal = params[0];
		if (idsVal.IsList()) {
			X::List list(idsVal);
			if (list->Size() != n) {
				retValue = X::Value(false);
				return;
			}
			size_t i = 0;
			for (auto& it : *list) {
				extIds[i++] = it.ToLongLong();
			}
		}
		else if (idsVal.IsLong()) {
			unsigned long long start = idsVal.ToLongLong();
			for (size_t i = 0; i < n; ++i) {
				extIds[i] = start + i;
			}
		}
		else {
			retValue = X::Value(false);
			return;
		}

		// --- 4) collect chunk texts if any ---
		std::vector<std::string> chunkTexts;
		if (chunksVal.IsList()) {
			X::List list(chunksVal);
			if (list->Size() == n) {
				for (auto& it : *list)
				{
					if (it.IsDict())
					{
						X::Value text = it["chunk"];
						chunkTexts.push_back(text.ToString());
					}
					else
					{
						chunkTexts.push_back(it.ToString());
					}
				}
			}
		}
		else if (chunksVal.IsDict()) {
			X::Value text = chunksVal["chunk"];
			chunkTexts.assign(n, text.ToString());
		}
		else if (chunksVal.IsString()) {
			chunkTexts.assign(n, chunksVal.ToString());
		}

		// --- 5) register in the VectorDatabase ¡ú get internal indices ---
		std::vector<unsigned long long> recIdx;
		if (chunkTexts.empty()) {
			recIdx = m_vdb->AddLabels(extIds, std::vector<std::string>(n));
		}
		else {
			recIdx = m_vdb->AddLabels(extIds, chunkTexts);
		}

		// --- 6) insert into HNSW with the rawPtr (no copy) ---
		m_index->AddVectors(recIdx, rawPtr, totalCount, num_threads);

		unsigned long long last = extIds[n-1];
		retValue = X::Value(last);
	}


}