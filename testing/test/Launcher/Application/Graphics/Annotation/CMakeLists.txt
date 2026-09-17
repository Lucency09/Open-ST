# 配置纯标准库标注模型镜像测试。
add_executable(open_st_annotation_tests source/annotation_tests.cpp source/annotation_document_tests.cpp
    source/annotation_m2_tests.cpp source/annotation_m3_tests.cpp)
target_link_libraries(open_st_annotation_tests PRIVATE OpenST::Annotation GTest::gtest_main)
gtest_discover_tests(open_st_annotation_tests TEST_PREFIX "annotation." PROPERTIES LABELS "annotation;unit")
