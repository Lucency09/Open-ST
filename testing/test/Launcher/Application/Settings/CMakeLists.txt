add_executable(open_st_settings_tests)

target_sources(open_st_settings_tests
    PRIVATE
        source/settings_tests.cpp
        source/settings_window_tests.cpp
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
gtest_discover_tests(open_st_settings_tests TEST_PREFIX "settings." PROPERTIES LABELS "settings;unit")
