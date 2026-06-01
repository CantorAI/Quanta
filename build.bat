@echo off
echo Building Quanta VDB...

if not exist build mkdir build
cd build

echo Running CMake configuration...
cmake ..
if errorlevel 1 goto config_fail

echo Building project...
cmake --build . --config Release
if errorlevel 1 goto build_fail

echo Build complete!
goto :eof

:config_fail
echo CMake configuration failed. Please ensure xlang is cloned in the parent directory (../xlang) and CMake is installed.
exit /b 1

:build_fail
echo Build failed.
exit /b 1
