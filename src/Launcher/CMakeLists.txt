# launcher 没有公开接口，只有生成 Windows GUI 程序入口的私有源文件。
add_subdirectory(Application)

enable_language(RC)
add_executable(open_st WIN32)

target_sources(open_st PRIVATE source/main.cpp resources/open_st.rc)
# RC 从版本化资源目录读取图标，图标随 EXE 嵌入，无运行期外部路径依赖。
target_include_directories(open_st PRIVATE "${PROJECT_SOURCE_DIR}/resources/icons")

target_link_libraries(open_st
    PRIVATE
        OpenST::Application
        open_st_project_options
)

# 界面文本与默认设置是版本化运行时资源，始终复制到可执行文件旁的 resources/。
add_custom_command(TARGET open_st POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:open_st>/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/resources/ui_text.json"
        "$<TARGET_FILE_DIR:open_st>/resources/ui_text.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/resources/default_settings.json"
        "$<TARGET_FILE_DIR:open_st>/resources/default_settings.json"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_SOURCE_DIR}/resources/setting_windows.json"
        "$<TARGET_FILE_DIR:open_st>/resources/setting_windows.json"
    VERBATIM
)

set_target_properties(open_st PROPERTIES OUTPUT_NAME "Open-ST")

# 在 EXE 激活上下文中启用系统视觉样式；仅链接 comctl32 不会启用 v6 主题。
target_link_options(open_st PRIVATE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
)
