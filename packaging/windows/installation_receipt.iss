// 选择与安装范围一致的专用归属记录根，不访问用户设置。
// 入参：无。
// 返回：当前用户或所有用户的注册表根。
function ReceiptRoot: Integer;
begin
  if IsAdminInstallMode then Result := HKLM64 else Result := HKCU;
end;

// 计算安装器使用的规范绝对路径，Unicode 文本完整保留。
// 入参：无。
// 返回：无尾部斜线的完整安装路径。
function ReceiptPath: String;
begin
  Result := RemoveBackslashUnlessRoot(ExpandFileName(ExpandConstant('{app}')));
end;

// 对完整 Unicode 路径计算稳定的安装器专用记录键。
// 入参：无。
// 返回：只属于本安装目录的键，不与卸载登记或 Settings 混用。
function ReceiptKey: String;
begin
  Result := 'Software\Open-ST\InstallationReceipts\' + GetSHA256OfUnicodeString(Uppercase(ReceiptPath));
end;

// 发布明确的安装范围文字，记录仅由标准安装器读写。
// 入参：无。
// 返回：固定作用域标识。
function ReceiptScope: String;
begin
  if IsAdminInstallMode then Result := 'all-users' else Result := 'current-user';
end;

// 验证完整成功记录归属，并返回数值版本给安装器执行降级保护。
// 入参：Version 输出原安装版本。
// 返回：AppId、范围、规范路径、成功标识和版本全部有效时为 true。
function MatchingReceipt(var Version: String): Boolean;
var Root: Integer; Key, Identity, Scope, Path, State: String; Packed: Int64;
begin
  Result := False; Root := ReceiptRoot; Key := ReceiptKey;
  if not RegQueryStringValue(Root, Key, 'State', State) or (State <> 'complete') then exit;
  if not RegQueryStringValue(Root, Key, 'AppId', Identity) or (Identity <> '{#ProductId}') then exit;
  if not RegQueryStringValue(Root, Key, 'Scope', Scope) or (Scope <> ReceiptScope) then exit;
  if not RegQueryStringValue(Root, Key, 'Path', Path) or (CompareText(Path, ReceiptPath) <> 0) then exit;
  if not RegQueryStringValue(Root, Key, 'Version', Version) or not StrToVersion(Version, Packed) then exit;
  Result := True;
end;

// 在所有安装文件验证完成后发布成功记录，部分写入不得表示完成。
// 入参：无。
// 返回：失败抛异常，由标准安装器报告并恢复已记录操作。
procedure StoreReceipt;
var Root: Integer; Key: String;
begin
  Root := ReceiptRoot; Key := ReceiptKey;
  if not RegWriteStringValue(Root, Key, 'State', 'pending') or
     not RegWriteStringValue(Root, Key, 'AppId', '{#ProductId}') or
     not RegWriteStringValue(Root, Key, 'Scope', ReceiptScope) or
     not RegWriteStringValue(Root, Key, 'Path', ReceiptPath) or
     not RegWriteStringValue(Root, Key, 'Version', '{#AppVersion}') or
     not RegWriteStringValue(Root, Key, 'State', 'complete') then
    RaiseException('Cannot publish the installation ownership record.');
end;

// 仅在显式数据删除成功后清理身份完全匹配的本目录记录。
// 入参：无。
// 返回：无；其他路径、范围或身份的键不受影响。
procedure RemoveReceipt;
var Version: String;
begin
  if MatchingReceipt(Version) then
    if not RegDeleteKeyIncludingSubkeys(ReceiptRoot, ReceiptKey) then
      RaiseException('Data was removed, but its installation ownership record could not be removed.');
end;
