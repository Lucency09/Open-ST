# 使用协议样例验证供应商请求，不连接真实服务或读取用户凭据。
add_executable(open_st_translation_tests source/protocol_tests.cpp source/custom_tests.cpp source/client_tests.cpp)
target_include_directories(open_st_translation_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/Translation/private")
target_compile_definitions(open_st_translation_tests PRIVATE OPEN_ST_TEST_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
target_link_libraries(open_st_translation_tests PRIVATE OpenST::Translation OpenST::Common OpenST::Http GTest::gtest_main)
gtest_discover_tests(open_st_translation_tests TEST_PREFIX "translation." PROPERTIES LABELS "translation" TIMEOUT 30)
add_subdirectory(Local)
