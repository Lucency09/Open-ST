# 本地依赖缓存

此目录保存可重新生成且不提交 Git 的本地产物。构建脚本将 vcpkg installed tree 写入：

```text
.cache/vcpkg_installed/x64-windows/<profile>/
```

基础产品依赖使用 `product-base` profile；启用可选 feature 时使用 `product-<features>`，测试依赖使用 `tests`。
产品和测试入口通过 `scripts/build_environment.ps1` 共用以下绝对缓存路径，不使用 AppData 默认缓存：

```text
.cache/vcpkg/downloads/          # 源码归档及 vcpkg 下载文件
.cache/vcpkg/downloads/tools/    # vcpkg 清单固定的依赖辅助工具
.cache/vcpkg/archives/           # 按 ABI 寻址的编译后二进制包
.cache/vcpkg/registries/         # Git 注册表及版本化端口树
```

vcpkg 自身管理的 CMake／PowerShell／Git 使用当前 `VCPKG_ROOT/scripts/vcpkg-tools.json` 的版本，
不因 Codex、普通终端或宿主 PowerShell 的 PATH 不同而另选工具；AppLocal DLL 部署也显式使用工作区 PowerShell。
外层项目 CMake／Ninja／MSVC 仍来自选定的 Visual Studio；端口脚本通过独立探测机制取得的 Ninja 等工具
仍遵循该端口实现，当前 Ninja 为 VS 提供的 1.12.1，不把 `FORCE_DOWNLOADED` 描述成覆盖所有端口自有探测。
首次统一工具版本或以后升级工具链可能改变 ABI 并重建；源码归档和生成的二进制包会继续在这里复用。
脚本只在原生构建调用期间设置缓存变量，成功或失败都恢复调用者进程环境，不写用户／系统环境变量。

`scripts/build.ps1 -Clean` 只清理对应配置的构建产物；Release还清理自己的临时工作与整理目录，均不会删除这里的依赖。

翻译模型通过 `python packaging/translation/convert_models.py --root .` 显式准备；普通 CMake 配置只验证模型。原始归档在 `translation-sources/`，隔离 Python 环境在 `translation-converter/`。
`translation-models/` 下保留现有双向模型；新准备结果先整套校验，再放入 `packs/<ID>/` 并原子更新 `active.txt`。
准备过程使用跨进程锁与唯一临时目录；旧包不自动清理，避免破坏仍引用它的构建。

除本说明文件外，此目录内容均被 Git 忽略。需要彻底重装第三方依赖时，可以手工删除对应 profile 目录。
