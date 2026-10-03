# 通用 HTTP 传输独立目标，不依赖产品模块或 Common GUI。
add_library(open_st_http STATIC)
add_library(OpenST::Http ALIAS open_st_http)
target_sources(open_st_http PRIVATE source/http_transport.cpp source/http_request.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/http_transport.h)
target_link_libraries(open_st_http PUBLIC open_st_project_options PRIVATE winhttp)
