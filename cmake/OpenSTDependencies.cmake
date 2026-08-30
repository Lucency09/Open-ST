# 集中执行第三方包的“查找”阶段。
# vcpkg.json 负责声明并固定包版本；本文件负责把已安装的包转换成 CMake 可使用的导入目标；
# 真正使用某个包的源码模块，仍需在自己的 CMakeLists.txt 中单独链接对应目标。
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
