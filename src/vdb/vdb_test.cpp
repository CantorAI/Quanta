#include <iostream>
#include <vector>
#include <cstdint>
#include "HnswVdb.h" // Adjust include path as needed

int vdb_test() {
    // Set the embedding dimension.
    int D = 128;
    // Define the maximum number of vectors (capacity)
    size_t capacity = 1000; // For example, 1000 vectors.

    // Create an HNSW-based vector database with a valid capacity.
    Quanta::HnswVdb db(D, capacity, 16, 200, 50);

    // Create some example vectors.
    std::vector<float> vec1(D, 0.1f);
    std::vector<float> vec2(D, 0.2f);
    std::vector<float> vec3(D, 0.3f);
    std::vector<float> vec4(D, 0.4f);

    // Add vectors with unique IDs.
    db.AddVector(1001ULL, vec1);
    db.AddVector(1002ULL, vec2);
    db.AddVector(1003ULL, vec3);
    db.AddVector(1004ULL, vec4);

    // Create a query vector.
    std::vector<float> query(D, 0.25f);

    // Perform a lookup for top 3 nearest neighbors.
    std::vector<std::pair<uint64_t, float>> results = db.Lookup(query, 3);

    // Print the lookup results.
    std::cout << "HNSW Lookup Results:" << std::endl;
    for (const auto& result : results) {
        std::cout << "ID: " << result.first << " | Similarity: " << result.second << std::endl;
    }

    // Save the database.
    db.Save("test_hnsw_index");

    // Create a new HNSW VDB instance and load the saved index.
    Quanta::HnswVdb db2(D, capacity, 16, 200, 50);
    db2.Load("test_hnsw_index");

    // Perform another lookup on the loaded index.
    std::vector<std::pair<uint64_t, float>> results2 = db2.Lookup(query, 3);
    std::cout << "\nLookup Results after loading:" << std::endl;
    for (const auto& result : results2) {
        std::cout << "ID: " << result.first << " | Similarity: " << result.second << std::endl;
    }

    return 0;
}
