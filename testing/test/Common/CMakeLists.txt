# 配置 Common 的镜像测试目标、链接依赖及用例发现。

add_subdirectory(WindowRenderer)

add_executable(open_st_common_tests)

target_sources(open_st_common_tests
    PRIVATE
        source/json_file_tests.cpp
        source/logger_tests.cpp
)

target_link_libraries(open_st_common_tests
    PRIVATE
        OpenST::Common
        GTest::gtest_main
        open_st_project_options
)

target_include_directories(open_st_common_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/Common/private")

# logger_tests 验证日志内记录的源码路径；目录迁移期间保持既有断言所观察到的逻辑路径不变。
if(MSVC)
    target_compile_options(open_st_common_tests
        PRIVATE
            /experimental:deterministic
            "/pathmap:${CMAKE_CURRENT_SOURCE_DIR}=${PROJECT_SOURCE_DIR}/testing/test/logging"
    )
endif()

include(GoogleTest)
gtest_discover_tests(open_st_common_tests TEST_PREFIX "common.")

