#include "scene_tracker.h"
#include "partitioned_vdb.h"
#include "vector_input.h"
#include <algorithm>
#include <mutex>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace Quanta
{
    // ============================================================================
    // Constructor (parent-aware, called by AddVarClass)
    // ============================================================================
    SceneTracker::SceneTracker(X::Value parent,
        const X::ARGS& params, const X::KWARGS& kwParams)
        : vdb_(parent.NativeData<PartitionedVdb>()), owner_(std::move(parent))
    {
        if (vdb_)
        {
            dimension_ = vdb_->GetDimension();
        }

        if (auto it = Keyword(kwParams, "threshold"); it)
        {
            threshold_ = static_cast<float>((*it).ToDouble());
        }
        if (auto it = Keyword(kwParams, "method"); it)
        {
            method_ = (*it).ToString();
        }
        if (auto it = Keyword(kwParams, "window_size"); it)
        {
            window_size_ = static_cast<int>((*it).ToLongLong());
            if (window_size_ < 1) window_size_ = 1; // Safeguard
        }
        if (auto it = Keyword(kwParams, "tracker_id"); it)
        {
            tracker_id_ = (*it).ToString();
            LoadState(); // Instantly hydrate RAM state from disk if it exists
        }
    }

    // ============================================================================
    // Binary Crash Resilience (Automated Native I/O)
    // ============================================================================
    void SceneTracker::SaveState()
    {
        if (!vdb_ || tracker_id_.empty()) return;
        try {
            fs::path trackDir = fs::path(vdb_->GetBasePath()) / "trackers";
            fs::create_directories(trackDir);
            fs::path binPath = trackDir / (tracker_id_ + ".bin");

            std::ofstream out(binPath, std::ios::binary | std::ios::trunc);
            if (!out) return;

            // 1. Magic Header & Version
            uint32_t magic = 0x54524B52; // "TRKR"
            uint32_t version = 1;
            out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
            out.write(reinterpret_cast<const char*>(&version), sizeof(version));

            // 2. Scalars
            out.write(reinterpret_cast<const char*>(&state_.scene_id), sizeof(state_.scene_id));
            out.write(reinterpret_cast<const char*>(&state_.frame_count), sizeof(state_.frame_count));
            out.write(reinterpret_cast<const char*>(&state_.start_ts), sizeof(state_.start_ts));
            out.write(reinterpret_cast<const char*>(&state_.end_ts), sizeof(state_.end_ts));
            out.write(reinterpret_cast<const char*>(&state_.best_image_id), sizeof(state_.best_image_id));
            out.write(reinterpret_cast<const char*>(&state_.best_score), sizeof(state_.best_score));
            
            // 3. Centroid Vector
            uint32_t vec_size = static_cast<uint32_t>(state_.centroid.size());
            out.write(reinterpret_cast<const char*>(&vec_size), sizeof(vec_size));
            if (vec_size > 0) {
                out.write(reinterpret_cast<const char*>(state_.centroid.data()), vec_size * sizeof(float));
            }

            // 4. Window History (if applicable)
            uint32_t history_size = static_cast<uint32_t>(window_history_.size());
            out.write(reinterpret_cast<const char*>(&history_size), sizeof(history_size));
            for (const auto& w_vec : window_history_) {
                uint32_t w_vec_size = static_cast<uint32_t>(w_vec.size());
                out.write(reinterpret_cast<const char*>(&w_vec_size), sizeof(w_vec_size));
                if (w_vec_size > 0) {
                    out.write(reinterpret_cast<const char*>(w_vec.data()), w_vec_size * sizeof(float));
                }
            }
            out.close();
        } catch (...) {}
    }

    void SceneTracker::LoadState()
    {
        if (!vdb_ || tracker_id_.empty()) return;
        try {
            fs::path binPath = fs::path(vdb_->GetBasePath()) / "trackers" / (tracker_id_ + ".bin");
            if (!fs::exists(binPath)) return;

            std::ifstream in(binPath, std::ios::binary);
            if (!in) return;

            uint32_t magic, version;
            in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
            if (magic != 0x54524B52) return; // Incorrect magic
            in.read(reinterpret_cast<char*>(&version), sizeof(version));

            in.read(reinterpret_cast<char*>(&state_.scene_id), sizeof(state_.scene_id));
            in.read(reinterpret_cast<char*>(&state_.frame_count), sizeof(state_.frame_count));
            in.read(reinterpret_cast<char*>(&state_.start_ts), sizeof(state_.start_ts));
            in.read(reinterpret_cast<char*>(&state_.end_ts), sizeof(state_.end_ts));
            in.read(reinterpret_cast<char*>(&state_.best_image_id), sizeof(state_.best_image_id));
            in.read(reinterpret_cast<char*>(&state_.best_score), sizeof(state_.best_score));

            uint32_t vec_size;
            in.read(reinterpret_cast<char*>(&vec_size), sizeof(vec_size));
            if (vec_size > 0) {
                state_.centroid.resize(vec_size);
                in.read(reinterpret_cast<char*>(state_.centroid.data()), vec_size * sizeof(float));
            }

            uint32_t history_size;
            in.read(reinterpret_cast<char*>(&history_size), sizeof(history_size));
            window_history_.clear();
            for (uint32_t i = 0; i < history_size; ++i) {
                uint32_t w_vec_size;
                in.read(reinterpret_cast<char*>(&w_vec_size), sizeof(w_vec_size));
                if (w_vec_size > 0) {
                    std::vector<float> w_vec(w_vec_size);
                    in.read(reinterpret_cast<char*>(w_vec.data()), w_vec_size * sizeof(float));
                    window_history_.push_back(w_vec);
                }
            }
            in.close();
        } catch (...) {}
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
    X::Value SceneTracker::Append(const X::ARGS& params, const X::KWARGS& kwParams)
    {
        X::Value retValue;
        std::lock_guard<std::mutex> lock(tracker_mutex_);
        if (!vdb_ || params.size() < 1)
        {
            retValue = X::Value(false);
            return retValue;
        }

        unsigned long long imageId =
            static_cast<unsigned long long>(params[0].ToLongLong());

        // Parse kwargs
        long long timestampMs = 0;
        std::string partitionTag = "default";
        float score = 0.0f;
        std::vector<float> vec;

        if (auto it = Keyword(kwParams, "timestamp"); it)
        {
            timestampMs = (*it).ToLongLong();
        }
        if (auto it = Keyword(kwParams, "partitionTag"); it)
        {
            partitionTag = (*it).ToString();
        }
        if (auto it = Keyword(kwParams, "score"); it)
        {
            score = static_cast<float>((*it).ToDouble());
        }

        // Step 1: WAL Bypass Direct Injection vs Synchronous DB Lookup
        if (auto it = Keyword(kwParams, "embedding"); it) {
            vec = FloatInput(*it).Take();
        } 
        
        // Fallback: If not dynamically injected, run the synchronous hard-disk lookup
        if (vec.empty()) {
            std::string tsPartition = vdb_->TimestampToPartitionName(timestampMs);
            int customIndex = vdb_->GetOrCreateCustomIndex(partitionTag);
            std::string fullKey = tsPartition + "_" + std::to_string(customIndex);
            
            vec = vdb_->FetchVectorForItem(imageId, fullKey, false);
            if (vec.empty())
            {
                retValue = X::Value(false);
                return retValue;
            }
        }

        if (dimension_ == 0)
        {
            dimension_ = static_cast<int>(vec.size());
        }

        if (vec.size() != static_cast<size_t>(dimension_)) throw X::Error("Tracker embedding dimension mismatch");
        NormalizeVec(vec);

        // Step 2: First frame — initialize
        if (state_.scene_id < 0)
        {
            state_.scene_id = 0;
            state_.centroid = vec;
            state_.frame_count = 1;
            state_.start_ts = timestampMs;
            state_.end_ts = timestampMs;
            state_.best_image_id = imageId;
            state_.best_score = score;
            
            if (method_ == "window") {
                window_history_.push_back(vec);
            }

            retValue = BuildResult(false, 1.0f);
            return retValue;
        }

        // Step 3: Cosine similarity 
        float similarity = 0.0f;
        if (state_.centroid.size() == vec.size())
        {
            for (size_t d = 0; d < vec.size(); ++d)
            {
                similarity += vec[d] * state_.centroid[d];
            }
        }

        // Step 4: Same scene
        if (similarity >= threshold_)
        {
            size_t dim = vec.size();
            
            if (method_ == "window") {
                window_history_.push_back(vec);
                if (static_cast<int>(window_history_.size()) > window_size_) {
                    window_history_.pop_front();
                }
                
                // Recompute active subset centroid completely from scratch
                std::vector<float> new_centroid(dim, 0.0f);
                for(const auto& w_vec : window_history_) {
                    for(size_t d = 0; d < dim; ++d) {
                        new_centroid[d] += w_vec[d];
                    }
                }
                state_.centroid = new_centroid;
                NormalizeVec(state_.centroid);
            } else {
                // Centroid mode (original): Infinite running average
                int count = state_.frame_count;
                for (size_t d = 0; d < dim; ++d)
                {
                    state_.centroid[d] = state_.centroid[d] * count + vec[d];
                }
                NormalizeVec(state_.centroid);
            }

            state_.frame_count++;
            state_.end_ts = timestampMs;

            if (score > state_.best_score)
            {
                state_.best_score = score;
                state_.best_image_id = imageId;
            }

            // Automated Checkpoint saving every 150 frames (~5 seconds at 30fps)
            if (state_.frame_count % 150 == 0) SaveState();

            retValue = BuildResult(false, similarity);
            return retValue;
        }

        // Step 5: New scene
        SceneState completed = state_;

        state_.scene_id++;
        state_.centroid = vec;
        state_.frame_count = 1;
        state_.start_ts = timestampMs;
        state_.end_ts = timestampMs;
        state_.best_image_id = imageId;
        state_.best_score = score;
        
        if (method_ == "window") {
            window_history_.clear();
            window_history_.push_back(vec);
        }

        // Extremely critical boundary event, definitively flush state to disk
        SaveState();

        retValue = BuildResult(true, similarity, &completed);
        return retValue;
    }

    // ============================================================================
    // Reset
    // ============================================================================
    void SceneTracker::Reset()
    {
        std::lock_guard<std::mutex> lock(tracker_mutex_);
        state_ = SceneState{};
        window_history_.clear();
        SaveState(); 
    }

    // ============================================================================
    // GetState
    // ============================================================================
    X::Value SceneTracker::GetState()
    {
        std::lock_guard<std::mutex> lock(tracker_mutex_);
        auto dict = X::Value::Dict(Host());
        dict.SetItem("scene_id", state_.scene_id);
        dict.SetItem("frame_count", state_.frame_count);
        dict.SetItem("start_ts", state_.start_ts);
        dict.SetItem("end_ts", state_.end_ts);
        dict.SetItem("best_image_id",
            static_cast<long long>(state_.best_image_id));
        dict.SetItem("best_score",
            static_cast<double>(state_.best_score));
        dict.SetItem("method", method_);
        return dict;
    }

    // ============================================================================
    // BuildResult
    // ============================================================================
    X::Value SceneTracker::BuildResult(bool isNewScene, float similarity,
        const SceneState* completedScene)
    {
        auto result = X::Value::Dict(Host());
        result.SetItem("scene_id", state_.scene_id);
        result.SetItem("is_new_scene", isNewScene);
        result.SetItem("similarity", static_cast<double>(similarity));
        result.SetItem("scene_frame_count", state_.frame_count);
        result.SetItem("scene_start_ts", state_.start_ts);
        result.SetItem("scene_end_ts", state_.end_ts);
        result.SetItem("best_image_id",
            static_cast<long long>(state_.best_image_id));
        result.SetItem("best_score",
            static_cast<double>(state_.best_score));

        if (completedScene)
        {
            auto completed = X::Value::Dict(Host());
            completed.SetItem("scene_id", completedScene->scene_id);
            completed.SetItem("best_image_id",
                static_cast<long long>(completedScene->best_image_id));
            completed.SetItem("best_score",
                static_cast<double>(completedScene->best_score));
            completed.SetItem("frame_count", completedScene->frame_count);
            completed.SetItem("start_ts", completedScene->start_ts);
            completed.SetItem("end_ts", completedScene->end_ts);
            result.SetItem("completed_scene", completed);
        }

        return result;
    }
}
