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

# 在 EXE 激活上下文中启用系统视觉样式；仅链接 comctl32 不会启用 v6 主题。
target_link_options(open_st PRIVATE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
)
