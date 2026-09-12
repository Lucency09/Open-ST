# Open-ST Mock 目录

`testing/mock/` 保存测试替身，已有模块镜像 `src/` 的相对目录依赖树及模块名称大小写。
模块目录使用 PascalCase，`mock`、`include`、`source` 等基础和职责目录保持小写；不预建没有实际用途的空模块。

当前只有 Export 系统边界替身，使用显式函数表和线程局部内存状态，不操作真实剪贴板或输出文件：

```text
testing/mock/
├── CMakeLists.txt
└── Launcher/
    ├── CMakeLists.txt
    └── Application/
        ├── CMakeLists.txt
        └── Export/
            ├── CMakeLists.txt
            └── include/
                └── export_system_fake.h
```

规则：

- mock 不能进入主程序链接关系，只能由 `testing/test/` 下的测试目标使用。
- 一个 mock target 只模拟对应源码模块的可注入接口。
- 公共 mock 头直接放在模块自己的 `include/`，实现放在 `source/`。
- 每一级目录通过自己的 `CMakeLists.txt` 递归加载直接子目录；没有真实 mock 时不创建空库目标。
- `test.ps1` 已通过 vcpkg manifest 的 `tests` feature 提供 GoogleTest/GoogleMock；真实 mock target 按需链接 `GTest::gmock` 或 `GTest::gmock_main`。
- 不为尚不存在的接口编写伪 mock，也不让产品代码依赖 GoogleMock。
