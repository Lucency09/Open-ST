# 本地依赖缓存

此目录保存可重新生成且不提交 Git 的本地产物。构建脚本将 vcpkg installed tree 写入：

```text
.cache/vcpkg_installed/x64-windows/<profile>/
```

基础产品依赖使用 `product-base` profile；启用可选 feature 时使用 `product-<features>`，测试依赖使用 `tests`。
`scripts/build.ps1 -Clean` 只清理对应配置的构建产物；Release还清理自己的临时工作与整理目录，均不会删除这里的依赖。

除本说明文件外，此目录内容均被 Git 忽略。需要彻底重装第三方依赖时，可以手工删除对应 profile 目录。
