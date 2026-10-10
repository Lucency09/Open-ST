# 测试只关闭自己创建的探针进程，不运行安装器或触碰用户程序。
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_executable(open_st_installer_shutdown_tests WIN32 "${PROJECT_SOURCE_DIR}/packaging/windows/close_application.cpp")
add_executable(open_st_close_application_probe WIN32 close_application_probe.cpp)
foreach(target IN ITEMS open_st_installer_shutdown_tests open_st_close_application_probe)
    set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_link_libraries(${target} PRIVATE open_st_project_options advapi32 user32 shell32)
endforeach()
add_dependencies(open_st_installer_shutdown_tests open_st_close_application_probe)
add_test(NAME packaging.InstallerShutdown COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_installer_shutdown.py"
    --helper "$<TARGET_FILE:open_st_installer_shutdown_tests>" --probe "$<TARGET_FILE:open_st_close_application_probe>")
set_tests_properties(packaging.InstallerShutdown PROPERTIES LABELS "packaging;integration" TIMEOUT 45 RUN_SERIAL TRUE)
