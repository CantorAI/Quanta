#!/bin/bash

echo "Building Quanta VDB..."

mkdir -p build
cd build

echo "Running CMake configuration..."
cmake ..
if [ $? -ne 0 ]; then
    echo "CMake configuration failed. Please ensure xlang is cloned in the parent directory (../xlang) and CMake is installed."
    exit 1
fi

echo "Building project..."
cmake --build . --config Release
if [ $? -ne 0 ]; then
    echo "Build failed."
    exit 1
fi

echo "Build complete!"
