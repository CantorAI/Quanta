
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
include_directories("src/dfs")
include_directories("src/main")

FILE(GLOB XLANG_API_SRC "${xlang_sdk_folder}/value.cpp")
FILE(GLOB Common_SRC "src/common/*.cpp" "src/log/*.cpp")
FILE(GLOB Dfs_SRC "src/dfs/*.cpp")
FILE(GLOB Main_SRC "src/main/*.cpp")


add_library(${PROJECT_NAME} 
	SHARED
	${XLANG_API_SRC}
	${Common_SRC}
	${Main_SRC}
	${Dfs_SRC}
)
if (WIN32)
else()
    target_link_libraries(${PROJECT_NAME} pthread dl)
endif()
