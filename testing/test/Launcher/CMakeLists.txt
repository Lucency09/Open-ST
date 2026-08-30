# launcher 使用标准 gTest case 验证基础产品元数据。
add_subdirectory(Application)

add_executable(open_st_launcher_tests source/launcher_tests.cpp)
target_link_libraries(
    open_st_launcher_tests
    PRIVATE
        open_st_project_options
        GTest::gtest_main
)
gtest_discover_tests(
    open_st_launcher_tests
    TEST_PREFIX "launcher."
    PROPERTIES LABELS "launcher;unit"
)
