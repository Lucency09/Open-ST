add_executable(open_st_settings_tests)

# 真实窗口测试使用与产品一致的系统视觉样式。
target_link_options(open_st_settings_tests PRIVATE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
)

target_sources(open_st_settings_tests
    PRIVATE
        source/settings_tests.cpp
        source/settings_window_tests.cpp
        source/settings_edit_tests.cpp
)

target_link_libraries(open_st_settings_tests
    PRIVATE
        OpenST::Settings
        OpenST::Common
        open_st_project_options
        GTest::gtest_main
)

target_include_directories(open_st_settings_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/Launcher/Application/Settings/private"
        "${PROJECT_SOURCE_DIR}/src/Common/private"
)

include(GoogleTest)
target_compile_definitions(open_st_settings_tests PRIVATE
    OPEN_ST_SETTINGS_LAYOUT_PATH="${PROJECT_SOURCE_DIR}/resources/setting_windows.json"
)

gtest_discover_tests(open_st_settings_tests TEST_PREFIX "settings." PROPERTIES LABELS "settings;unit")

# 人工窗口验收单独设为无限等待，普通运行由环境开关跳过。
add_executable(open_st_settings_manual_tests source/welcome_manual_tests.cpp)
target_link_libraries(open_st_settings_manual_tests PRIVATE
    OpenST::Settings OpenST::Common open_st_project_options GTest::gtest_main)
target_include_directories(open_st_settings_manual_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/Settings/private"
    "${PROJECT_SOURCE_DIR}/src/Common/private")
target_link_options(open_st_settings_manual_tests PRIVATE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
gtest_discover_tests(open_st_settings_manual_tests TEST_PREFIX "settings."
    PROPERTIES LABELS "settings;manual" TIMEOUT 0)
