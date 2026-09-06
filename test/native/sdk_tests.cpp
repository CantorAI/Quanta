#include "xlang3/xlang3.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

static void require(bool condition, const char* message) {
    if (!condition) throw X::Error(message);
}
static X::Value call(const X::Value& function, const X::ARGS& args = {}, const X::KWARGS& kwargs = {}) {
    X::Value result;
    if (!function.Call(args, kwargs, result)) throw X::Error(x3_runtime_last_error(function.runtime()));
    return result;
}
static X::Value database(X::Runtime& runtime, const std::string& root) {
    X::Module module(runtime, "quanta", "Quanta");
    return call(module["partitioned_vdb"], {}, {
        {"path", X::Value(runtime, root)}, {"prefix", X::Value(runtime, "sdk")},
        {"dimension", X::Value(3)}, {"max_memory_gb", X::Value(.001)},
        {"wal_cooling_time_seconds", X::Value(1)}});
}
static void check(X::Runtime& runtime, const X::Value& db, int count) {
    auto query = runtime.List();
    query.Append(1.); query.Append(0.); query.Append(0.);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        auto result = call(db["Lookup"], {query, X::Value(count)});
        if (result.Size() == static_cast<uint64_t>(count)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    throw X::Error("queued vectors were lost");
}
int main(int argc, char** argv) {
    try {
        require(argc == 3, "expected module directory and temporary storage");
        const auto root = std::filesystem::path(argv[2]);
        {
            X::Runtime first, second;
            first.AddImportRoot(argv[1]); second.AddImportRoot(argv[1]);
            auto a = database(first, (root / "first").string());
            auto b = database(second, (root / "second").string());
            auto vector = first.List();
            vector.Append(1.); vector.Append(0.); vector.Append(0.);
            std::atomic<bool> ok{true};
            std::vector<std::thread> threads;
            for (int t = 0; t < 4; ++t) threads.emplace_back([&, t] {
                try {
                    for (int i = 0; i < 25; ++i)
                        call(a["AddVectors"], {X::Value(1000 + t * 25 + i), vector});
                } catch (...) { ok = false; }
            });
            for (auto& thread : threads) thread.join();
            require(ok, "concurrent SDK insertion failed");
            require(call(a["GetTotalRecords"]).ToLongLong() == 100, "incorrect accepted count");
            require(call(b["GetTotalRecords"]).ToLongLong() == 0, "runtime caches were shared");
            check(first, a, 100);
            // Intentionally omit Close: package destruction must join workers and preserve data.
        }
        {
            X::Runtime reopened;
            reopened.AddImportRoot(argv[1]);
            auto db = database(reopened, (root / "first").string());
            check(reopened, db, 100);
            require(call(db["GetTotalRecords"]).ToLongLong() == 100, "count did not persist across runtime destruction");
            call(db["Close"]);
            X::Module module(reopened, "quanta", "Quanta");
            auto dfs = call(module["dfs"]);
            X::Stream stream(reopened);
            require(dfs.ToBytes(stream) && stream.Rewind(), "DFS native serialization failed");
            X::Value decoded;
            require(decoded.FromBytes(stream), "DFS native deserialization failed");
            require(call(decoded["Query"], {X::Value(reopened, "absent")}).Size() == 0,
                    "deserialized native DFS object is unusable");
        }
        std::cout << "quanta-runtime-passed: concurrent SDK, independent runtimes, shutdown, serialization\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
