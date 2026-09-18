# 配置 Launcher/Application/SystemIntegration 模块的源码、公开头及构建依赖。

add_library(open_st_system_integration STATIC)
add_library(OpenST::SystemIntegration ALIAS open_st_system_integration)
target_sources(open_st_system_integration PRIVATE source/startup_registration.cpp source/single_instance.cpp
    source/installation_lease.cpp source/installer_launcher.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/startup_registration.h include/single_instance.h
    include/installation_lease.h include/installer_launcher.h)
target_link_libraries(open_st_system_integration PUBLIC open_st_project_options OpenST::Common PRIVATE advapi32 user32 shell32)
