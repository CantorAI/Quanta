#include "scene_tracker.h"
#include "partitioned_vdb.h"
#include <algorithm>

namespace Quanta
{
    // ============================================================================
    // Constructor (parent-aware, called by AddVarClass)
    // ============================================================================
    SceneTracker::SceneTracker(PartitionedVdb* parent,
        X::ARGS& params, X::KWARGS& kwParams)
        : vdb_(parent)
    {
        if (vdb_)
        {
            dimension_ = vdb_->GetDimension();
        }

        // kwargs: threshold
        if (auto it = kwParams.find("threshold"); it)
        {
            threshold_ = static_cast<float>(it->val.ToDouble());
        }
    }

    // ============================================================================
    // NormalizeVec
    // ============================================================================
    float SceneTracker::NormalizeVec(std::vector<float>& vec)
    {
        float norm = 0.0f;
        for (auto v : vec)
        {
            norm += v * v;
        }
        if (norm <= 0.0f) return 0.0f;
        norm = std::sqrt(norm);
        for (auto& v : vec)
        {
            v /= norm;
        }
        return norm;
    }

    // ============================================================================
    // Append
    // ============================================================================
    void SceneTracker::Append(X::XRuntime* rt, X::XObj* pContext,
        X::ARGS& params, X::KWARGS& kwParams, X::Value& retValue)
    {
        if (!vdb_ || params.size() < 1)
        {
            retValue = X::Value(false);
            return;
        }

        unsigned long long imageId =
            static_cast<unsigned long long>(params[0].ToLongLong());

        // Parse kwargs
        long long timestampMs = 0;
        std::string partitionTag = "default";
        float score = 0.0f;

        if (auto it = kwParams.find("timestamp"); it)
        {
            timestampMs = it->val.ToLongLong();
        }
        if (auto it = kwParams.find("partitionTag"); it)
        {
            partitionTag = it->val.ToString();
        }
        if (auto it = kwParams.find("score"); it)
        {
            score = static_cast<float>(it->val.ToDouble());
        }

        // Step 1: Resolve partition key
        std::string tsPartition = vdb_->TimestampToPartitionName(timestampMs);
        int customIndex = vdb_->GetOrCreateCustomIndex(partitionTag);
        std::string fullKey = tsPartition + "_" + std::to_string(customIndex);

        // Step 2: Fetch vector from VDB
        std::vector<float> vec = vdb_->FetchVectorForItem(
            imageId, fullKey, false);

        if (vec.empty())
        {
            retValue = X::Value(false);
            return;
        }

        if (dimension_ == 0)
        {
            dimension_ = static_cast<int>(vec.size());
        }

        NormalizeVec(vec);

        // Step 3: First frame — initialize
        if (state_.scene_id < 0)
        {
            state_.scene_id = 0;
            state_.centroid = vec;
            state_.frame_count = 1;
            state_.start_ts = timestampMs;
            state_.end_ts = timestampMs;
            state_.best_image_id = imageId;
            state_.best_score = score;

            retValue = BuildResult(false, 1.0f);
            return;
        }

        // Step 4: Cosine similarity (dot product for unit vectors)
        float similarity = 0.0f;
        if (state_.centroid.size() == vec.size())
        {
            for (size_t d = 0; d < vec.size(); ++d)
            {
                similarity += vec[d] * state_.centroid[d];
            }
        }

        // Step 5: Same scene
        if (similarity >= threshold_)
        {
            size_t dim = vec.size();
            int count = state_.frame_count;
            for (size_t d = 0; d < dim; ++d)
            {
                state_.centroid[d] = state_.centroid[d] * count + vec[d];
            }
            NormalizeVec(state_.centroid);

            state_.frame_count++;
            state_.end_ts = timestampMs;

            if (score > state_.best_score)
            {
                state_.best_score = score;
                state_.best_image_id = imageId;
            }

            retValue = BuildResult(false, similarity);
            return;
        }

        // Step 6: New scene
        SceneState completed = state_;

        state_.scene_id++;
        state_.centroid = vec;
        state_.frame_count = 1;
        state_.start_ts = timestampMs;
        state_.end_ts = timestampMs;
        state_.best_image_id = imageId;
        state_.best_score = score;

        retValue = BuildResult(true, similarity, &completed);
    }

    // ============================================================================
    // Reset
    // ============================================================================
    void SceneTracker::Reset()
    {
        state_ = SceneState{};
    }

    // ============================================================================
    // GetState
    // ============================================================================
    X::Value SceneTracker::GetState()
    {
        X::Dict dict;
        dict->Set("scene_id", state_.scene_id);
        dict->Set("frame_count", state_.frame_count);
        dict->Set("start_ts", state_.start_ts);
        dict->Set("end_ts", state_.end_ts);
        dict->Set("best_image_id",
            static_cast<long long>(state_.best_image_id));
        dict->Set("best_score",
            static_cast<double>(state_.best_score));
        return dict;
    }

    // ============================================================================
    // BuildResult
    // ============================================================================
    X::Value SceneTracker::BuildResult(bool isNewScene, float similarity,
        const SceneState* completedScene)
    {
        X::Dict result;
        result->Set("scene_id", state_.scene_id);
        result->Set("is_new_scene", isNewScene);
        result->Set("similarity", static_cast<double>(similarity));
        result->Set("scene_frame_count", state_.frame_count);
        result->Set("scene_start_ts", state_.start_ts);
        result->Set("scene_end_ts", state_.end_ts);
        result->Set("best_image_id",
            static_cast<long long>(state_.best_image_id));
        result->Set("best_score",
            static_cast<double>(state_.best_score));

        if (completedScene)
        {
            X::Dict completed;
            completed->Set("scene_id", completedScene->scene_id);
            completed->Set("best_image_id",
                static_cast<long long>(completedScene->best_image_id));
            completed->Set("best_score",
                static_cast<double>(completedScene->best_score));
            completed->Set("frame_count", completedScene->frame_count);
            completed->Set("start_ts", completedScene->start_ts);
            completed->Set("end_ts", completedScene->end_ts);
            result->Set("completed_scene", completed);
        }

        return result;
    }
}
