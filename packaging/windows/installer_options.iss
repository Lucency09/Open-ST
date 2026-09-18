// 识别显式禁止快捷方式参数，避免隐藏开始菜单页面时丢失用户意图。
// 入参：无；读取 Inno 标准命令行参数。
// 返回：包含完整 /NOICONS 参数时为 true。
function NoIconsRequested: Boolean;
var I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), '/NOICONS') = 0 then begin
      Result := True;
      exit;
    end;
end;

// 同时遵守标准向导选择和显式命令行意图。
// 入参：无。
// 返回：允许创建快捷方式时为 true。
function ShouldCreateIcons: Boolean;
begin
  Result := not NoIconsRequested and not WizardNoIcons;
end;
