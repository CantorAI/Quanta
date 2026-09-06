#pragma once
#include "quanta_runtime.h"
#include <vector>
#include <string>
#include <cmath>
#include <mutex>
#include <deque>

namespace Quanta
{
    class PartitionedVdb;

    // State for one scene in a single-stream tracker
    struct SceneState {
        int scene_id = -1;
        std::vector<float> centroid;    // normalized unit centroid
        int frame_count = 0;
        long long start_ts = 0;
        long long end_ts = 0;
        unsigned long long best_image_id = 0;
        float best_score = 0.0f;
    };

    // Single-stream scene tracker (one per IPC, Python manages per-device dict).
    class SceneTracker
    {
        PartitionedVdb* vdb_ = nullptr;
        X::Value owner_;
        
        // Algorithms Configuration
        float threshold_ = 0.85f;
        int dimension_ = 0;
        std::string method_ = "window"; // "centroid" or "window"
        int window_size_ = 15;            // Max frames for sliding window
        std::string tracker_id_ = "";     // Device ID for disk serialization
        
        // Live state
        SceneState state_;
        std::deque<std::vector<float>> window_history_;
        std::mutex tracker_mutex_;

    public:
        BEGIN_PACKAGE(SceneTracker)
            APISET().AddVarFunc("Append", &SceneTracker::Append);
            APISET().AddFunc<0>("Reset", &SceneTracker::Reset);
            APISET().AddFunc<0>("GetState", &SceneTracker::GetState);
        END_PACKAGE

        SceneTracker(X::Value parent, const X::ARGS& params, const X::KWARGS& kwParams);
        virtual ~SceneTracker() = default;

        // Append a new frame
        // params[0] = image_id (long long)
        // kwargs: timestamp, partitionTag, score, embedding
        X::Value Append(const X::ARGS& params, const X::KWARGS& kwParams);

        void Reset();
        X::Value GetState();

    private:
        static float NormalizeVec(std::vector<float>& vec);
        X::Value BuildResult(bool isNewScene, float similarity,
            const SceneState* completedScene = nullptr);

        // Crash Resilience (Automated Native I/O)
        void LoadState();
        void SaveState();
    };
}
