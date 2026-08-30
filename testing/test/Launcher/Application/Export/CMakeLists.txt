add_executable(open_st_export_tests source/export_tests.cpp)
target_link_libraries(open_st_export_tests PRIVATE OpenST::Export OpenST::ExportMocks GTest::gtest_main
    windowscodecs ole32)
gtest_discover_tests(open_st_export_tests TEST_PREFIX "export." PROPERTIES LABELS "export;unit")
