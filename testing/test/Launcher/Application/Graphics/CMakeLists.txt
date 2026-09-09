# 配置 Launcher/Application/Graphics 的镜像测试目标、链接依赖及用例发现。

# 选区状态模型使用纯几何测试；覆盖渲染器另以隐藏窗口验证实际 GPU 往返像素。
add_subdirectory(Capture)
add_subdirectory(HDR)

add_executable(open_st_selection_tests source/selection_model_tests.cpp)
target_link_libraries(
    open_st_selection_tests
    PRIVATE
        OpenST::Graphics
        GTest::gtest_main
)
gtest_discover_tests(
    open_st_selection_tests
    TEST_PREFIX "selection."
    PROPERTIES LABELS "graphics;selection;unit"
)

# graphics 测试覆盖预览、裁切、遮罩与设置；混合 SDR/HDR 输出 case 使用实际 D2D 设备。
add_executable(
    open_st_graphics_tests
    source/selection_output_renderer_tests.cpp
    source/desktop_preview_tests.cpp
    source/outside_mask_layout_tests.cpp
    source/overlay_settings_tests.cpp
)
target_link_libraries(
    open_st_graphics_tests
    PRIVATE
        OpenST::Graphics
        GTest::gtest_main
)
target_include_directories(
    open_st_graphics_tests
    PRIVATE
        "${PROJECT_SOURCE_DIR}/src/Launcher/Application/Graphics/private"
)
gtest_discover_tests(
    open_st_graphics_tests
    TEST_PREFIX "graphics."
    PROPERTIES LABELS "graphics"
)

add_executable(open_st_overlay_renderer_tests source/overlay_renderer_tests.cpp)
target_link_libraries(open_st_overlay_renderer_tests PRIVATE OpenST::Graphics GTest::gtest_main)
gtest_discover_tests(
    open_st_overlay_renderer_tests
    TEST_PREFIX "graphics."
    PROPERTIES LABELS "graphics;integration"
)
