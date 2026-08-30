# common 提供不依赖产品模块的公共能力，包括同步日志和按需加载的 JSON 文件缓存。
add_library(open_st_common STATIC)
add_library(OpenST::Common ALIAS open_st_common)

target_sources(open_st_common
    PRIVATE
        source/json_file.cpp
        source/logger.cpp
        private/logger.h
        private/json_file_test_access.h
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/json_file.h
            include/log.h
)

target_link_libraries(open_st_common
    PUBLIC
        open_st_project_options
        nlohmann_json::nlohmann_json
)

target_include_directories(open_st_common PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/private")

