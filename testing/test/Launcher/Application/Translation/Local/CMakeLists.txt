# 镜像离线引擎真实模型测试；只消费已验证本地缓存，不访问网络。
add_executable(open_st_translation_local_tests source/local_tests.cpp source/line_format_tests.cpp source/language_tests.cpp)
target_include_directories(open_st_translation_local_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/Translation/Local/private")
target_link_libraries(open_st_translation_local_tests PRIVATE OpenST::TranslationLocal CTranslate2::ctranslate2 GTest::gtest_main)
target_compile_definitions(open_st_translation_local_tests PRIVATE
    OPEN_ST_TRANSLATION_CACHE=L"${OPEN_ST_TRANSLATION_MODEL_CACHE}"
    OPEN_ST_TRANSLATION_TEST_OUTPUT=L"${PROJECT_SOURCE_DIR}/testing/testoutput/translation-local")
gtest_discover_tests(open_st_translation_local_tests TEST_PREFIX "translation_local."
    PROPERTIES LABELS "translation" TIMEOUT 90)
