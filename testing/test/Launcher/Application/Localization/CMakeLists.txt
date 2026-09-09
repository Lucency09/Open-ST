# 配置 Launcher/Application/Localization 的镜像测试目标、链接依赖及用例发现。

add_executable(open_st_localization_tests source/ui_text_tests.cpp)

target_link_libraries(open_st_localization_tests
    PRIVATE
        OpenST::Localization
        OpenST::Common
        open_st_project_options
        nlohmann_json::nlohmann_json
        GTest::gtest_main
)

target_include_directories(open_st_localization_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/Launcher/Application/Localization/private"
        "${PROJECT_SOURCE_DIR}/src/Common/private"
)

add_custom_command(TARGET open_st_localization_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:open_st_localization_tests>/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/resources/ui_text.json"
        "$<TARGET_FILE_DIR:open_st_localization_tests>/resources/ui_text.json"
    VERBATIM
)

gtest_discover_tests(
    open_st_localization_tests
    TEST_PREFIX "localization."
    PROPERTIES LABELS "localization;unit"
)
