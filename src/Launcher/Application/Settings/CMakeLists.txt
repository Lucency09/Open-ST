# Settings 拥有设置业务与非模态窗口；仅使用 Common 通用 JSON 句柄读写文件。
add_library(open_st_settings_window STATIC)
add_library(OpenST::SettingsWindow ALIAS open_st_settings_window)
add_library(OpenST::Settings ALIAS open_st_settings_window)

target_sources(open_st_settings_window
    PRIVATE
        source/settings.cpp
        source/settings_window.cpp
        source/settings_edit.cpp
        private/settings_internal.h
        private/settings_edit.h
        private/settings_window_test_access.h
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/settings.h
            include/settings_window.h
)

target_include_directories(open_st_settings_window PRIVATE private)

target_link_libraries(open_st_settings_window
    PUBLIC
        open_st_project_options
    PRIVATE
        OpenST::Common
        OpenST::WindowRenderer
)
