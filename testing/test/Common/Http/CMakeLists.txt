# 公共网络测试只连接回环代理，不访问外网或修改系统代理。
add_executable(open_st_http_tests source/http_tests.cpp)
target_link_libraries(open_st_http_tests PRIVATE OpenST::Http GTest::gtest_main ws2_32)
include(GoogleTest)
gtest_discover_tests(open_st_http_tests TEST_PREFIX "http." PROPERTIES LABELS "http" TIMEOUT 15)
