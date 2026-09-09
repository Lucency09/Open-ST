# 定义依赖解析函数，将 manifest 安装的包转换为各模块可用的 CMake 目标。

# 解析 manifest 已安装的第三方包，供实际使用它们的模块链接导入目标。
# 入参：无显式参数；读取 OPEN_ST_BUILD_TESTS 和 OPEN_ST_ENABLE_OCR 决定可选依赖。
# 返回：无返回值；建立包提供的导入目标，必需包缺失时终止 CMake 配置。
function(open_st_resolve_dependencies)
    # 设置和界面本地化都是基础功能，所有产品与测试构建都需要 JSON 解析。
    find_package(nlohmann_json CONFIG REQUIRED)

    # 只有构建测试时才查找 GoogleTest/GoogleMock。
    # test.ps1 通过 vcpkg 的 tests feature 安装 gtest；普通 build.ps1 永远不会进入此分支。
    if(OPEN_ST_BUILD_TESTS)
        find_package(GTest CONFIG REQUIRED)
    endif()

    # 只有启用 OCR 模块时才查找 Tesseract。
    if(OPEN_ST_ENABLE_OCR)
        find_package(Tesseract CONFIG REQUIRED)
    endif()

endfunction()
