# 更新模块拥有网络、正式版校验与下载任务，不依赖兄弟模块或窗口。
add_library(open_st_update STATIC)
add_library(OpenST::Update ALIAS open_st_update)
target_sources(open_st_update
    PRIVATE source/update_model.cpp source/update_client.cpp source/update_transport.cpp source/protected_download.cpp
            private/update_model.h private/update_transport.h private/protected_download.h
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/update_client.h)
target_include_directories(open_st_update PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/private")
target_link_libraries(open_st_update PUBLIC open_st_project_options
    PRIVATE OpenST::Common nlohmann_json::nlohmann_json winhttp bcrypt advapi32)
