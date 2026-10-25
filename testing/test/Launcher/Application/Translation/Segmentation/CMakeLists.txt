# 镜像纯文本分段与有界拼接测试，不依赖模型、网络或 Windows。
add_executable(open_st_translation_segmentation_tests source/text_segmentation_tests.cpp)
target_link_libraries(open_st_translation_segmentation_tests PRIVATE OpenST::TranslationSegmentation GTest::gtest_main)
gtest_discover_tests(open_st_translation_segmentation_tests TEST_PREFIX "translation_segmentation."
    PROPERTIES LABELS "translation" TIMEOUT 15)
