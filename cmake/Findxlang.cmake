find_path(XLANG_INCLUDE_DIR NAMES xlang3/xlang3.h
    HINTS "${CMAKE_CURRENT_LIST_DIR}/../../xlang3/sdk"
    DOC "XLang3 SDK include directory")
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(xlang REQUIRED_VARS XLANG_INCLUDE_DIR)
