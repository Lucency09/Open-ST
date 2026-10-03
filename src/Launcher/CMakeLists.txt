# 构建 Open-ST 可执行程序，链接应用库并配置图标、清单与运行资源复制。

# launcher 没有公开接口，只有生成 Windows GUI 程序入口的私有源文件。
add_subdirectory(Application)

enable_language(RC)
add_executable(open_st WIN32)

target_sources(open_st PRIVATE source/main.cpp resources/open_st.rc "${PROJECT_BINARY_DIR}/generated/open_st/version.rc")
# RC 从版本化资源目录读取图标，图标随 EXE 嵌入，无运行期外部路径依赖。
target_include_directories(open_st PRIVATE "${PROJECT_SOURCE_DIR}/resources/icons")

target_link_libraries(open_st
    PRIVATE
        OpenST::Application
        open_st_project_options
)

# 运行资源与发行包共用唯一清单，避免 CMake、PowerShell 和 Inno 各维护一份。
file(READ "${PROJECT_SOURCE_DIR}/packaging/windows/release-files.json" OPEN_ST_RELEASE_FILES)
string(JSON OPEN_ST_LAUNCHER_FILE_COUNT LENGTH "${OPEN_ST_RELEASE_FILES}" launcherFiles)
math(EXPR OPEN_ST_LAUNCHER_FILE_LAST "${OPEN_ST_LAUNCHER_FILE_COUNT} - 1")
foreach(OPEN_ST_FILE_INDEX RANGE ${OPEN_ST_LAUNCHER_FILE_LAST})
    string(JSON OPEN_ST_RELEASE_FILE GET "${OPEN_ST_RELEASE_FILES}" launcherFiles ${OPEN_ST_FILE_INDEX})
    if(OPEN_ST_RELEASE_FILE MATCHES "^resources/")
        get_filename_component(OPEN_ST_RESOURCE_DIRECTORY "${OPEN_ST_RELEASE_FILE}" DIRECTORY)
        add_custom_command(TARGET open_st POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:open_st>/${OPEN_ST_RESOURCE_DIRECTORY}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PROJECT_SOURCE_DIR}/${OPEN_ST_RELEASE_FILE}"
                "$<TARGET_FILE_DIR:open_st>/${OPEN_ST_RELEASE_FILE}"
            VERBATIM
        )
    endif()
endforeach()

set_target_properties(open_st PROPERTIES OUTPUT_NAME "Open-ST")

# 六份模型由唯一固定清单恢复；禁用 OCR 时清除本目标遗留模型，防止混入基础包。
if(OPEN_ST_ENABLE_OCR)
    foreach(OPEN_ST_MODEL_INDEX RANGE 0 5)
        string(JSON OPEN_ST_MODEL_FAMILY GET "${OPEN_ST_OCR_MANIFEST}" models ${OPEN_ST_MODEL_INDEX} family)
        string(JSON OPEN_ST_MODEL_LANGUAGE GET "${OPEN_ST_OCR_MANIFEST}" models ${OPEN_ST_MODEL_INDEX} language)
        add_custom_command(TARGET open_st POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:open_st>/resources/ocr/${OPEN_ST_MODEL_FAMILY}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${OPEN_ST_OCR_MODEL_CACHE}/${OPEN_ST_MODEL_FAMILY}/${OPEN_ST_MODEL_LANGUAGE}.traineddata"
                "$<TARGET_FILE_DIR:open_st>/resources/ocr/${OPEN_ST_MODEL_FAMILY}/${OPEN_ST_MODEL_LANGUAGE}.traineddata"
            VERBATIM)
    endforeach()
else()
    add_custom_command(TARGET open_st POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:open_st>/resources/ocr"
        VERBATIM)
endif()

# 本地翻译与编译期白名单共用固定清单，模型、分词器和上游许可一同复制。
if(OPEN_ST_ENABLE_TRANSLATION)
    file(READ "${PROJECT_SOURCE_DIR}/packaging/translation/models.json" OPEN_ST_TRANSLATION_MANIFEST)
    foreach(OPEN_ST_MODEL_INDEX RANGE 0 1)
        string(JSON OPEN_ST_MODEL_DIRECTION GET "${OPEN_ST_TRANSLATION_MANIFEST}" models ${OPEN_ST_MODEL_INDEX} direction)
        string(JSON OPEN_ST_MODEL_FILE_COUNT LENGTH "${OPEN_ST_TRANSLATION_MANIFEST}" models ${OPEN_ST_MODEL_INDEX} files)
        math(EXPR OPEN_ST_MODEL_FILE_LAST "${OPEN_ST_MODEL_FILE_COUNT} - 1")
        foreach(OPEN_ST_FILE_INDEX RANGE ${OPEN_ST_MODEL_FILE_LAST})
            string(JSON OPEN_ST_MODEL_FILE GET "${OPEN_ST_TRANSLATION_MANIFEST}" models ${OPEN_ST_MODEL_INDEX} files ${OPEN_ST_FILE_INDEX} file)
            get_filename_component(OPEN_ST_MODEL_NAME "${OPEN_ST_MODEL_FILE}" NAME)
            add_custom_command(TARGET open_st POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:open_st>/resources/translation/${OPEN_ST_MODEL_DIRECTION}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${OPEN_ST_TRANSLATION_MODEL_CACHE}/${OPEN_ST_MODEL_DIRECTION}/${OPEN_ST_MODEL_NAME}"
                    "$<TARGET_FILE_DIR:open_st>/${OPEN_ST_MODEL_FILE}"
                VERBATIM)
        endforeach()
    endforeach()
    add_custom_command(TARGET open_st POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/packaging/translation/NOTICE.txt"
            "${PROJECT_SOURCE_DIR}/packaging/translation/models.json"
            "$<TARGET_FILE_DIR:open_st>/resources/translation"
        VERBATIM)
else()
    add_custom_command(TARGET open_st POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:open_st>/resources/translation"
        VERBATIM)
endif()

# 在 EXE 激活上下文中启用系统视觉样式；仅链接 comctl32 不会启用 v6 主题。
target_link_options(open_st PRIVATE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
)
