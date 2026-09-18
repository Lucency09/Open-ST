; 编译期查询实际引擎版本；PE 固定版本字段在官方工具中可能为零。
#ifndef ExpectedCompilerVersion
  #error ExpectedCompilerVersion is required
#endif
#if VER != Int(ExpectedCompilerVersion)
  #error The Inno Setup compiler does not match the pinned version
#endif
[Setup]
AppName=Open-ST compiler version check
AppVersion=1.0
CreateAppDir=no
Uninstallable=no
Output=no
