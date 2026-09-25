# OCR 边界测试使用实际任务线程和可控引擎；模型校验测试读取固定缓存，不访问网络。
add_executable(open_st_ocr_tests source/ocr_tests.cpp source/model_tests.cpp source/real_engine_tests.cpp)
target_link_libraries(open_st_ocr_tests PRIVATE OpenST::OCR GTest::gtest_main gdi32 user32)
target_include_directories(open_st_ocr_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/OCR/private")
target_compile_definitions(open_st_ocr_tests PRIVATE
    OPEN_ST_OCR_MODEL_CACHE=L"${PROJECT_SOURCE_DIR}/.cache/ocr-models"
    OPEN_ST_OCR_TEST_OUTPUT=L"${PROJECT_SOURCE_DIR}/testing/testoutput/ocr")
gtest_discover_tests(open_st_ocr_tests TEST_PREFIX "ocr." PROPERTIES LABELS "ocr" TIMEOUT 30)
