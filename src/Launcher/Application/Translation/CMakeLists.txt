# 翻译统一适配器依赖本地子模块与公共 HTTP，不依赖兄弟业务模块。
add_subdirectory(Local)
add_library(open_st_translation STATIC)
add_library(OpenST::Translation ALIAS open_st_translation)
target_sources(open_st_translation PRIVATE
    source/translation_options.cpp source/translation_protocol.cpp source/translation_validation.cpp
    source/translation_custom.cpp source/translation_provider.cpp source/translation_client.cpp
    private/translation_protocol.h private/translation_provider.h
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/translation_client.h)
target_include_directories(open_st_translation PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/private")
target_link_libraries(open_st_translation PUBLIC open_st_project_options nlohmann_json::nlohmann_json
    PRIVATE OpenST::Common OpenST::Http OpenST::TranslationLocal ole32 bcrypt)
