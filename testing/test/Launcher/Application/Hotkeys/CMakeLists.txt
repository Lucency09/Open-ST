# 配置快捷键镜像测试，系统冲突与回收失败由后端替身复现。
add_executable(open_st_hotkeys_tests source/hotkeys_tests.cpp)
target_link_libraries(open_st_hotkeys_tests PRIVATE OpenST::Hotkeys GTest::gtest_main)
target_include_directories(open_st_hotkeys_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/Hotkeys/private")
include(GoogleTest)
gtest_discover_tests(open_st_hotkeys_tests TEST_PREFIX "hotkeys." PROPERTIES LABELS "hotkeys;unit")
