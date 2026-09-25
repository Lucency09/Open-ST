# 配置 Open-ST 工程选项、依赖与版本生成，并加载产品和可选测试构建。

# 指定配置本项目所需的最低 CMake 版本。
# 3.28 及以上版本能够稳定支持本项目使用的现代 CMake 功能和 vcpkg manifest 模式。
cmake_minimum_required(VERSION 3.28)

# OCR 是默认产品能力；在 project 加载 vcpkg 工具链之前同步实际依赖选择。
option(OPEN_ST_ENABLE_OCR "构建本地 OCR 模块" ON)
if(OPEN_ST_ENABLE_OCR)
    list(APPEND VCPKG_MANIFEST_FEATURES ocr)
    list(REMOVE_DUPLICATES VCPKG_MANIFEST_FEATURES)
else()
    list(REMOVE_ITEM VCPKG_MANIFEST_FEATURES ocr)
endif()

# 声明项目名称、当前版本和使用的编程语言。
# VERSION 会提供 PROJECT_VERSION 等变量，并用于生成程序内显示的版本号。
project(OpenST VERSION 0.4.1 LANGUAGES CXX)

# 让 CMake 在构建目录生成 compile_commands.json，供 IDE（C/C++ 插件 / clangd）解析
# include 路径与编译宏。对产品构建和测试构建同时生效，且不影响任何编译行为。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 当前工程直接调用 Win32、Direct2D 等 Windows API，因此不允许在其他平台误配置。
if(NOT WIN32)
    message(FATAL_ERROR "Open-ST 当前只支持 Windows 平台")
endif()

# 是否允许编译警告存在。
# 默认关闭，即项目自身的警告会按照 /WX 视为错误；临时排查问题时可由构建脚本打开。
option(OPEN_ST_ALLOW_WARNINGS "允许项目代码带警告通过编译" OFF)

# 是否构建自动化测试与 Mock。普通产品构建关闭，仅允许 test.ps1 开启。
option(OPEN_ST_BUILD_TESTS "构建 Open-ST 自动化测试与 Mock" OFF)

# 翻译尚未实现，继续保留显式功能边界。
option(OPEN_ST_ENABLE_TRANSLATION "构建翻译服务模块" OFF)

# 把仓库内的 cmake/ 加入模块搜索路径，随后可按文件名 include 公共构建脚本。
# 这里保存的是需要提交到仓库的构建逻辑，不是 CMake 自动生成的 build/ 目录。
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(OpenSTProjectOptions)
include(OpenSTDependencies)

# 创建所有自有目标共享的编译选项目标，并根据功能开关查找所需第三方包。
open_st_create_project_options()
open_st_resolve_dependencies()
if(OPEN_ST_ENABLE_OCR)
    include(OpenSTOcrModels)
endif()

# 根据模板生成版本头文件。@ONLY 表示只替换 @变量@，不会误处理源文件里的 ${变量}。
configure_file(config/version.h.in generated/open_st/version.h @ONLY)
file(READ "${PROJECT_SOURCE_DIR}/vcpkg.json" OPEN_ST_VCPKG_MANIFEST)
string(JSON OPEN_ST_VCPKG_VERSION GET "${OPEN_ST_VCPKG_MANIFEST}" version-semver)
if(NOT OPEN_ST_VCPKG_VERSION STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR "vcpkg 项目版本必须与 PROJECT_VERSION 一致")
endif()
configure_file(config/release-metadata.json.in generated/release-metadata.json @ONLY)
configure_file(config/version.rc.in generated/open_st/version.rc @ONLY)

# 进入源码模块索引。具体 .cpp/.h 必须由各模块自己的 CMakeLists.txt 管理。
add_subdirectory(src)

# 普通产品构建完全不加载 CTest、测试或 Mock。
# 只有 OPEN_ST_BUILD_TESTS=ON 时才进入 testing/；Mock 必须先于测试加载。
if(OPEN_ST_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        add_subdirectory(testing)
    endif()
endif()
