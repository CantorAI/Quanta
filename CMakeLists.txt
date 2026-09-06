
cmake_minimum_required (VERSION 3.10)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
message("CMAKE_MODULE_PATH=" ${CMAKE_MODULE_PATH} )
find_package(xlang REQUIRED)
set(xlang_sdk_folder  "${XLANG_INCLUDE_DIR}")

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("Quanta")


include_directories(${xlang_sdk_folder})
include_directories("src/include")
include_directories("src/common")
include_directories("src/log")
include_directories("src/vdb")
include_directories("src/dfs")
include_directories("src/main")

FILE(GLOB Common_SRC "src/common/*.cpp" "src/log/*.cpp")
FILE(GLOB Dfs_SRC "src/dfs/*.cpp")
FILE(GLOB Vdb_SRC "src/vdb/*.cpp")
FILE(GLOB Main_SRC "src/main/*.cpp")

file(GLOB SCRIPTS_FILES
    "${PROJECT_SOURCE_DIR}/scripts/*.py"
    "${PROJECT_SOURCE_DIR}/scripts/requirements.txt"
)
file(COPY ${SCRIPTS_FILES} DESTINATION "${CMAKE_BINARY_DIR}/bin/Applications/quanta")


add_library(${PROJECT_NAME} 
	SHARED
	${Common_SRC}
	${Main_SRC}
	${Dfs_SRC}
	${Vdb_SRC}
)
if(NOT TARGET xlang3_runtime)
    message(FATAL_ERROR "Quanta requires XLang3; build from CantorAIWorkspace")
endif()
target_link_libraries(Quanta PRIVATE xlang3_runtime ${CMAKE_DL_LIBS})
target_compile_definitions(Quanta PRIVATE XLANG3_USE_SHARED NOMINMAX)
target_compile_features(Quanta PRIVATE cxx_std_17)
if(NOT WIN32)
    target_link_libraries(Quanta PRIVATE pthread)
endif()
add_dependencies(Quanta xlang_sqlite3_native_package)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 3.14...<3.15 REQUIRED COMPONENTS Interpreter)
    add_executable(QuantaSdkTests test/native/sdk_tests.cpp)
    target_link_libraries(QuantaSdkTests PRIVATE xlang3_runtime)
    target_compile_definitions(QuantaSdkTests PRIVATE XLANG3_USE_SHARED)
    add_dependencies(QuantaSdkTests Quanta)
    add_test(NAME quanta_sdk
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/test/native/run_test.py"
            "$<TARGET_FILE:QuantaSdkTests>" "$<TARGET_FILE_DIR:Quanta>")
    add_test(NAME quanta_runtime
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/test/native/run_test.py"
            "$<TARGET_FILE:xlang3>" "${PROJECT_SOURCE_DIR}/test/native/quanta_runtime.py")
    add_test(NAME quanta_cpython_bridge
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/test/native/run_test.py"
            "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/test/native/quanta_runtime.py"
            "$<TARGET_FILE_DIR:Quanta>")
    set_tests_properties(quanta_runtime quanta_cpython_bridge quanta_sdk PROPERTIES TIMEOUT 70)
endif()
