# 配置 Launcher/Application/SystemIntegration 的镜像测试目标、链接依赖及用例发现。

add_executable(open_st_system_integration_tests source/system_integration_tests.cpp)
target_link_libraries(open_st_system_integration_tests PRIVATE OpenST::SystemIntegration GTest::gtest_main advapi32 user32)
gtest_discover_tests(open_st_system_integration_tests TEST_PREFIX "system_integration."
    PROPERTIES LABELS "system_integration;unit" TIMEOUT 20)
