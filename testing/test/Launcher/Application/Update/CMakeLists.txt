# 更新模型、任务与文件保护使用可控传输，不访问真实发行或执行安装器。
add_executable(open_st_update_tests source/update_tests.cpp source/protected_download_tests.cpp)
target_link_libraries(open_st_update_tests PRIVATE OpenST::Update GTest::gtest_main nlohmann_json::nlohmann_json)
target_include_directories(open_st_update_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/Update/private")
gtest_discover_tests(open_st_update_tests TEST_PREFIX "update." PROPERTIES LABELS "update" TIMEOUT 30)
