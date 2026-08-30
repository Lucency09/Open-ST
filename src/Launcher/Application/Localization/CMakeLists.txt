# localization 通过 Common 的懒加载 JSON 句柄读取动态多语言资源。
add_library(open_st_localization STATIC)
add_library(OpenST::Localization ALIAS open_st_localization)

target_sources(open_st_localization
    PRIVATE
        source/ui_text.cpp
        private/ui_text_internal.h
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/ui_text.h
)

target_link_libraries(open_st_localization
    PUBLIC
        open_st_project_options
    PRIVATE
        OpenST::Common
)

target_include_directories(open_st_localization PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/private")
