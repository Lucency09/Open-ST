# 配置独立快捷键模块，仅依赖标准库和 Windows SDK。
add_library(open_st_hotkeys STATIC)
add_library(OpenST::Hotkeys ALIAS open_st_hotkeys)
target_sources(open_st_hotkeys PRIVATE source/hotkeys.cpp private/hotkeys_backend.h private/hotkeys_test_access.h
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/hotkeys.h)
target_include_directories(open_st_hotkeys PRIVATE private)
target_link_libraries(open_st_hotkeys PUBLIC open_st_project_options PRIVATE user32)
