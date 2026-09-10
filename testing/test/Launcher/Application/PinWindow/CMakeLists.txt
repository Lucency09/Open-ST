# 配置贴图图像、交互及窗口生命周期测试，真实桌面门槛与人工验收独立显式启用。

add_executable(open_st_pin_window_tests
    source/pin_image_tests.cpp
    source/pin_renderer_tests.cpp
    source/pin_interaction_tests.cpp
    source/pin_window_tests.cpp
)
target_link_libraries(open_st_pin_window_tests PRIVATE
    OpenST::PinWindow GTest::gtest_main user32 gdi32 ole32 d3d11 dxgi d2d1 dcomp dwmapi)
target_include_directories(open_st_pin_window_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/PinWindow/private")
include(GoogleTest)
add_executable(open_st_pin_window_gate_tests source/pin_window_gate_tests.cpp)
target_include_directories(open_st_pin_window_gate_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/PinWindow/private")
target_link_libraries(open_st_pin_window_gate_tests PRIVATE
    OpenST::PinWindow GTest::gtest_main user32 ole32 dwmapi)
gtest_discover_tests(open_st_pin_window_gate_tests TEST_PREFIX "pin_window."
    PROPERTIES LABELS "pin_window;desktop" TIMEOUT 45 RUN_SERIAL TRUE)

gtest_discover_tests(open_st_pin_window_tests TEST_PREFIX "pin_window."
    PROPERTIES LABELS "pin_window;integration" TIMEOUT 45)

add_executable(open_st_pin_window_manual_tests source/pin_window_manual_tests.cpp)
target_link_libraries(open_st_pin_window_manual_tests PRIVATE
    OpenST::PinWindow GTest::gtest_main user32 gdi32 ole32 d3d11 dwmapi)
target_include_directories(open_st_pin_window_manual_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/PinWindow/private")
gtest_discover_tests(open_st_pin_window_manual_tests TEST_PREFIX "pin_window."
    PROPERTIES LABELS "pin_window;manual" TIMEOUT 0)
