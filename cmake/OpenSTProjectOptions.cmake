# 创建一个只传播编译要求、不产生 .lib 或 .dll 的 INTERFACE 目标。
# 各源码模块只要链接 open_st_project_options，就能获得完全一致的基础编译设置，
# 避免在每个模块中重复维护 C++ 标准、宏定义和警告参数。
function(open_st_create_project_options)
    add_library(open_st_project_options INTERFACE)

    # 要求所有链接此目标的自有代码使用 C++20。
    target_compile_features(open_st_project_options INTERFACE cxx_std_20)

    # 统一 Windows 头文件行为：
    # - UNICODE/_UNICODE：Win32 API 和 C 运行库默认使用宽字符版本；
    # - WIN32_LEAN_AND_MEAN：减少 windows.h 引入的不常用内容；
    # - NOMINMAX：阻止 Windows 头定义 min/max 宏，避免与标准库冲突。
    target_compile_definitions(open_st_project_options INTERFACE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        $<$<CONFIG:Debug>:OPEN_ST_DEBUG_LOGS=1>
    )

    # 统一 MSVC 编译规则：
    # - /W4：开启较严格的项目代码警告；
    # - /permissive-：使用更严格的标准一致性模式；
    # - /utf-8：源文件和执行字符集都按 UTF-8 处理；
    # - /external:W0：第三方和系统外部头文件不参与项目自身的警告门槛。
    target_compile_options(open_st_project_options INTERFACE
        /W4
        /permissive-
        /utf-8
        /external:W0
    )

    # 默认把项目代码警告视为错误，防止警告长期积累。
    # build.ps1 -AllowWarnings 会将 OPEN_ST_ALLOW_WARNINGS 设为 ON，临时取消 /WX。
    if(NOT OPEN_ST_ALLOW_WARNINGS)
        target_compile_options(open_st_project_options INTERFACE /WX)
    endif()
endfunction()
