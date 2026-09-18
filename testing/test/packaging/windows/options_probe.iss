; 只运行参数解析并立即退出，不安装文件、创建快捷方式或登记卸载项。
[Setup]
AppName=Open-ST installer option probe
AppId=Open-ST-Installer-Option-Probe
AppVersion=1.0
CreateAppDir=no
Uninstallable=no
PrivilegesRequired=lowest
DisableStartupPrompt=yes
OutputBaseFilename=options-probe
OutputDir=..\..\..\testoutput\installer-options

[Code]
#include "..\..\..\..\packaging\windows\installer_options.iss"
// 使用与产品相同的参数解析器，记录结果后在初始化阶段退出。
// 入参：无；Expected 为本测试的期望值。
// 返回：始终 false，确保不进入安装阶段。
function InitializeSetup: Boolean;
var Expected: Boolean;
begin
  Expected := CompareText(ExpandConstant('{param:Expected|false}'), 'true') = 0;
  if NoIconsRequested = Expected then Log('OPTIONS_TEST_PASS')
  else Log('OPTIONS_TEST_FAIL');
  Log('UNICODE_PATH_HASH=' + GetSHA256OfUnicodeString(Uppercase('E:\Open-ST\中文 空格')));
  Result := False;
end;
