# 配置 Launcher/Application/Graphics/HDR 的镜像测试目标、链接依赖及用例发现。

# 纯颜色测试和 Windows 原生效果集成都进入标准 gTest/CTest；测试名以 hdr. 前缀注册。
add_executable(open_st_hdr_color_tests source/color_conversion_tests.cpp)
target_link_libraries(open_st_hdr_color_tests PRIVATE OpenST::HDR GTest::gtest_main)
gtest_discover_tests(
    open_st_hdr_color_tests
    TEST_PREFIX "hdr."
    PROPERTIES LABELS "hdr;graphics;unit" TIMEOUT 15
)

add_executable(open_st_hdr_native_tests source/native_tone_mapper_tests.cpp source/adaptive_candidate_tests.cpp)
target_link_libraries(open_st_hdr_native_tests PRIVATE OpenST::HDR GTest::gtest_main d3dcompiler d3d11 d2d1 dxgi)
target_include_directories(open_st_hdr_native_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/Launcher/Application/Graphics/HDR/private")
gtest_discover_tests(
    open_st_hdr_native_tests
    TEST_PREFIX "hdr."
    PROPERTIES LABELS "hdr;graphics;integration" TIMEOUT 60 RUN_SERIAL TRUE
)
