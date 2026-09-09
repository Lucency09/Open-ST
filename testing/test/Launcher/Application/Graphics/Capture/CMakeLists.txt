# 配置 Launcher/Application/Graphics/Capture 的镜像测试目标、链接依赖及用例发现。

# capture 模型测试覆盖虚拟桌面几何、帧内存布局、原生 plane 所有权和旋转复制。
add_executable(
    open_st_capture_model_tests
    source/capture_model_tests.cpp
    source/captured_plane_writer_tests.cpp
    source/display_color_info_tests.cpp
    source/frozen_desktop_frame_tests.cpp
    source/output_capture_policy_tests.cpp
)
target_link_libraries(
    open_st_capture_model_tests
    PRIVATE
        OpenST::Capture
        GTest::gtest_main
)
target_include_directories(
    open_st_capture_model_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/Launcher/Application/Graphics/Capture/private"
)
gtest_discover_tests(
    open_st_capture_model_tests
    TEST_PREFIX "capture."
    PROPERTIES LABELS "capture;unit"
)

# capture 集成测试访问真实 Windows 桌面，属于可按平台条件跳过的 gTest 集成测试。
add_executable(open_st_capture_tests source/desktop_capturer_tests.cpp)
target_link_libraries(
    open_st_capture_tests
    PRIVATE
        OpenST::Capture
        GTest::gtest_main
)
gtest_discover_tests(
    open_st_capture_tests
    TEST_PREFIX "capture."
    PROPERTIES LABELS "capture;integration"
)
