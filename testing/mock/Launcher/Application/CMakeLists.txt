# 配置 Launcher/Application 的测试替身构建与逐级目录入口。

# application 后续在抽象托盘、快捷键和捕获会话边界后，在这里提供应用协调 gMock。
# 真实 mock 到来时采用 include 与 source 目录，
# 并在本文件创建 OpenST::<Module>Mocks target，私有链接 GTest::gmock。
add_subdirectory(Export)

# OCR 只在双功能替身中定义；纯翻译变体不带入 OCR 符号。
if(OPEN_ST_ENABLE_TRANSLATION)
    foreach(variant IN ITEMS text translation)
        add_library(open_st_${variant}_task_mocks STATIC source/text_task_fake.cpp)
        target_include_directories(open_st_${variant}_task_mocks PUBLIC include
            "${PROJECT_SOURCE_DIR}/src/Launcher/Application/Translation/include"
            "${PROJECT_SOURCE_DIR}/src/Launcher/Application/OCR/include")
        target_compile_definitions(open_st_${variant}_task_mocks PUBLIC OPEN_ST_HAS_TRANSLATION=1)
        target_link_libraries(open_st_${variant}_task_mocks PUBLIC open_st_project_options nlohmann_json::nlohmann_json)
    endforeach()
    target_compile_definitions(open_st_text_task_mocks PUBLIC OPEN_ST_HAS_OCR=1)
    add_library(OpenST::TextTaskMocks ALIAS open_st_text_task_mocks)
    add_library(OpenST::TranslationTaskMocks ALIAS open_st_translation_task_mocks)
endif()
