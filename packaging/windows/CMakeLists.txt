# 构建安装器临时使用的原生进程维护助手，不产生产品运行资源。
add_executable(open_st_close_application WIN32 close_application.cpp)
set_target_properties(open_st_close_application PROPERTIES
    OUTPUT_NAME "open-st-close-application"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(open_st_close_application PRIVATE open_st_project_options advapi32 user32 shell32)
