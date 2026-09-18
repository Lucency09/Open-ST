; 标准安装器：消费 build.ps1 生成的唯一文件清单，不负责应用设置或日志。
#ifndef AppVersion
  #error 通过 scripts/build.ps1 -Configuration Release -Installer 构建
#endif
#define ProductId "{8AB03738-7C3A-4FDB-9148-F3704CD86C74}"

[Setup]
AppId={{8AB03738-7C3A-4FDB-9148-F3704CD86C74}
AppName=Open-ST
AppVersion={#AppVersion}
AppPublisher=Lucency09
AppPublisherURL=https://github.com/Lucency09/Open-ST
DefaultDirName={code:DefaultDirectory}
DefaultGroupName=Open-ST
DisableProgramGroupPage=yes
AllowNoIcons=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19045
OutputDir={#OutputPath}
OutputBaseFilename=Open-ST-{#AppVersion}-win-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\Open-ST.exe
CloseApplications=no
RestartApplications=no
SetupMutex=Global\Open-ST-Setup
LicenseFile=..\..\LICENSE.txt
VersionInfoVersion={#AppVersion}.0
ChangesAssociations=no
UsePreviousPrivileges=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Icons]
Name: "{autoprograms}\Open-ST"; Filename: "{app}\Open-ST.exe"; Check: ShouldCreateIcons
Name: "{autodesktop}\Open-ST"; Filename: "{app}\Open-ST.exe"; Tasks: desktopicon; Check: ShouldCreateIcons

[Files]
Source: "{#RedistPath}"; DestName: "vc_redist.x64.exe"; Flags: dontcopy
#include GeneratedFiles

[Run]
Filename: "{app}\Open-ST.exe"; Description: "Launch Open-ST"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
#include "installer_options.iss"
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{#ProductId}_is1';
  InvalidHandle = -1;
  ReparsePoint = $400;
type
  TLockOverlapped = record
    Internal, InternalHigh, Offset, OffsetHigh, Event: LongWord;
  end;
  TFileInformation = record
    Attributes, CreationLow, CreationHigh, AccessLow, AccessHigh, WriteLow, WriteHigh,
      VolumeSerial, SizeHigh, SizeLow, Links, IndexHigh, IndexLow: LongWord;
  end;
  TSecurityTrustee = record
    MultipleTrustee, MultipleOperation, TrusteeForm, TrusteeType, Sid: LongWord;
  end;
var
  Lease: THandle;
  Anchors: array of THandle;
  LockInfo: TLockOverlapped;
  DeleteData: Boolean;
  BackupFolder: String;
  OldManifest: TArrayOfString;
  NewInstallation: Boolean;

function CreateFileW(Name: String; Access, Share, Security, Creation, Flags: LongWord; Template: THandle): THandle;
  external 'CreateFileW@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';
function GetFileAttributesW(Name: String): LongWord;
  external 'GetFileAttributesW@kernel32.dll stdcall';
function LockFileEx(Handle: THandle; Flags, Reserved, Low, High: LongWord; var Overlapped: TLockOverlapped): Boolean;
  external 'LockFileEx@kernel32.dll stdcall';
function GetFileInformationByHandle(Handle: THandle; var Information: TFileInformation): Boolean;
  external 'GetFileInformationByHandle@kernel32.dll stdcall';
function SetFileInformationByHandle(Handle: THandle; Kind: Integer; var Delete: LongWord; Size: LongWord): Boolean;
  external 'SetFileInformationByHandle@kernel32.dll stdcall';
function ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl: String; Revision: LongWord; var Descriptor: LongWord; Size: LongWord): Boolean;
  external 'ConvertStringSecurityDescriptorToSecurityDescriptorW@advapi32.dll stdcall';
function SetFileSecurityW(Name: String; Information, Descriptor: LongWord): Boolean;
  external 'SetFileSecurityW@advapi32.dll stdcall';
function LocalFree(Memory: LongWord): LongWord;
  external 'LocalFree@kernel32.dll stdcall';
function ConvertStringSidToSidW(Text: String; var Sid: LongWord): Boolean;
  external 'ConvertStringSidToSidW@advapi32.dll stdcall';
function GetNamedSecurityInfoW(Name: String; Kind, Information: LongWord; Owner, Group: LongWord;
  var Dacl: LongWord; Sacl: LongWord; var Descriptor: LongWord): LongWord;
  external 'GetNamedSecurityInfoW@advapi32.dll stdcall';
function GetEffectiveRightsFromAclW(Dacl: LongWord; var Trustee: TSecurityTrustee; var Rights: LongWord): LongWord;
  external 'GetEffectiveRightsFromAclW@advapi32.dll stdcall';

#include "installation_receipt.iss"

// 选择范围对应的默认专用目录，不引入 AppData 数据重定向。
// 入参：Param 为 Inno 参数。
// 返回：默认路径。
function DefaultDirectory(Param: String): String;
var Profile: String;
begin
  if IsAdminInstallMode then Result := ExpandConstant('{autopf}\Open-ST')
  else begin
    Profile := GetEnv('USERPROFILE');
    if (Length(Profile) < 3) or (Copy(Profile, 2, 2) <> ':\') then
      RaiseException('Cannot determine the current Windows user profile directory.');
    Result := AddBackslash(Profile) + 'Applications\Open-ST';
  end;
end;

// 释放维护租约与祖先锚定，锁文件始终留在原位。
// 入参：无。
// 返回：无。
procedure ReleaseProtection;
var I: Integer;
begin
  if Lease <> InvalidHandle then begin CloseHandle(Lease); Lease := InvalidHandle; end;
  for I := GetArrayLength(Anchors) - 1 downto 0 do CloseHandle(Anchors[I]);
  SetArrayLength(Anchors, 0);
end;

// 将本应用专有程序根设为管理员所有且普通用户只读，data 单独给予修改。
// 入参：Path 为本应用拥有的路径；DataAccess 决定是否允许 Users 修改。
// 返回：失败时抛异常，不静默沿用不安全 ACL。
procedure ApplyPermissions(Path: String; DataAccess: Boolean);
var Descriptor, Dacl, Verified, Sid, Rights: LongWord; Sddl: String; Trustee: TSecurityTrustee;
begin
  if not IsAdminInstallMode then exit;
  Sddl := 'O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)';
  if DataAccess then Sddl := Sddl + '(A;OICI;0x1301bf;;;BU)'
  else Sddl := Sddl + '(A;OICI;GRGX;;;BU)';
  Descriptor := 0;
  if not ConvertStringSecurityDescriptorToSecurityDescriptorW(Sddl, 1, Descriptor, 0) then
    RaiseException('Cannot prepare installation permissions.');
  try
    if not SetFileSecurityW(Path, $80000005, Descriptor) then
      RaiseException('Cannot apply protected installation permissions: ' + Path);
  finally LocalFree(Descriptor); end;
  if DataAccess then begin
    Sid := 0; Verified := 0; Dacl := 0;
    if not ConvertStringSidToSidW('S-1-5-32-545', Sid) then RaiseException('Cannot verify data permissions.');
    try
      if GetNamedSecurityInfoW(Path, 1, 4, 0, 0, Dacl, 0, Verified) <> 0 then
        RaiseException('Cannot read back data permissions.');
      Trustee.MultipleTrustee := 0; Trustee.MultipleOperation := 0;
      Trustee.TrusteeForm := 0; Trustee.TrusteeType := 5; Trustee.Sid := Sid;
      if (GetEffectiveRightsFromAclW(Dacl, Trustee, Rights) <> 0) or ((Rights and $1301bf) <> $1301bf) then
        RaiseException('Ordinary Windows users cannot modify this data directory.');
    finally
      if Verified <> 0 then LocalFree(Verified);
      LocalFree(Sid);
    end;
  end;
end;

// 持有目录防删除句柄，并拒绝重解析点。
// 入参：Directory 为现有目录。
// 返回：失败时抛异常。
procedure AnchorDirectory(Directory: String);
var H: THandle; Count: Integer; Information: TFileInformation; AppPath: String;
begin
  if (GetFileAttributesW(Directory) and ReparsePoint) <> 0 then
    RaiseException('Reparse points are not allowed: ' + Directory);
  H := CreateFileW(Directory, $81, 3, 0, 3, $02200000, 0);
  if H = InvalidHandle then RaiseException('Cannot protect directory: ' + Directory);
  if not GetFileInformationByHandle(H, Information) or ((Information.Attributes and ReparsePoint) <> 0) then begin
    CloseHandle(H); RaiseException('Directory identity changed: ' + Directory);
  end;
  Count := GetArrayLength(Anchors);
  SetArrayLength(Anchors, Count + 1); Anchors[Count] := H;
  AppPath := AddBackslash(ExpandConstant('{app}'));
  if (CompareText(Directory, ExpandConstant('{app}')) = 0) or
     ((CompareText(Copy(AddBackslash(Directory), 1, Length(AppPath)), AppPath) = 0) and
      (CompareText(Copy(AddBackslash(Directory), 1, Length(AppPath) + 5), AppPath + 'data\') <> 0)) then
    ApplyPermissions(Directory, False);
end;

// 自盘符根逐层锚定后再创建下一层，避免提升进程沿可替换路径写入。
// 入参：Directory 为本地绝对目录。
// 返回：无；失败终止操作。
procedure EnsureAnchoredDirectory(Directory: String);
var Current, Component: String; I, Start: Integer;
begin
  Directory := RemoveBackslashUnlessRoot(ExpandFileName(Directory));
  if (Length(Directory) < 3) or (Copy(Directory, 2, 2) <> ':\') then
    RaiseException('Only local absolute directories are supported.');
  Current := Copy(Directory, 1, 3); AnchorDirectory(Current); Start := 4;
  for I := 4 to Length(Directory) + 1 do
    if (I > Length(Directory)) or (Directory[I] = '\') then begin
      Component := Copy(Directory, Start, I - Start); Start := I + 1;
      if Component <> '' then begin
        Current := AddBackslash(Current) + Component;
        if not DirExists(Current) and not CreateDir(Current) then
          RaiseException('Cannot create directory: ' + Current);
        AnchorDirectory(Current);
      end;
    end;
end;

// 单次尝试共享协议的独占维护锁，不等待、不强杀实例。
// 入参：无。
// 返回：失败抛出占用/权限错误。
procedure AcquireMaintenance;
var LockPath: String; Information: TFileInformation;
begin
  EnsureAnchoredDirectory(ExpandConstant('{app}'));
  ApplyPermissions(ExpandConstant('{app}'), False);
  EnsureAnchoredDirectory(ExpandConstant('{app}\data'));
  ApplyPermissions(ExpandConstant('{app}\data'), True);
  EnsureAnchoredDirectory(ExpandConstant('{app}\data\.coordination'));
  LockPath := ExpandConstant('{app}\data\.coordination\install.lock');
  if FileExists(LockPath) and ((GetFileAttributesW(LockPath) and ReparsePoint) <> 0) then
    RaiseException('Invalid coordination file.');
  Lease := CreateFileW(LockPath, $C0000000, 3, 0, 4, $00200000, 0);
  if Lease = InvalidHandle then RaiseException('Cannot open installation coordination file.');
  if not GetFileInformationByHandle(Lease, Information) or (Information.Links <> 1) or
     ((Information.Attributes and ReparsePoint) <> 0) then RaiseException('Invalid coordination file identity.');
  if not LockFileEx(Lease, 3, 0, 1, 0, LockInfo) then
    RaiseException('This directory is in use or unavailable. Exit all Open-ST instances using it and manually try again.');
end;

// 验证树中没有重解析点，删除用户数据前先完成全树检查。
// 入参：Directory 为数据树。
// 返回：无；异常阻止删除。
procedure CheckDataTree(Directory: String);
var Find: TFindRec; Path: String;
begin
  if not DirExists(Directory) then exit;
  if (GetFileAttributesW(Directory) and ReparsePoint) <> 0 then RaiseException('Data contains a reparse point; delete it manually.');
  if FindFirst(AddBackslash(Directory) + '*', Find) then try
    repeat
      if (Find.Name <> '.') and (Find.Name <> '..') then begin
        Path := AddBackslash(Directory) + Find.Name;
        if (Find.Attributes and ReparsePoint) <> 0 then RaiseException('Data contains a reparse point; delete it manually.');
        if (Find.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then CheckDataTree(Path);
      end;
    until not FindNext(Find);
  finally FindClose(Find); end;
end;

// 只认可当前范围登记的同一路径与数值版本，拒绝接管 ZIP 和降级。
// 入参：无。
// 返回：无；不满足条件抛出明确错误。
procedure ValidateDestination;
var Root, OtherRoot: Integer; Registered, Other, OldVersion, ExeVersion: String;
    OldPacked, NewPacked: Int64; Find: TFindRec; HasEntries, RetainedDataOnly: Boolean;
begin
  NewInstallation := False;
  Registered := ''; Other := '';
  if IsAdminInstallMode then begin Root := HKLM64; OtherRoot := HKCU; end
  else begin Root := HKCU; OtherRoot := HKLM64; end;
  RegQueryStringValue(Root, UninstallKey, 'InstallLocation', Registered);
  RegQueryStringValue(OtherRoot, UninstallKey, 'InstallLocation', Other);
  if (Other <> '') and (CompareText(RemoveBackslash(Other), RemoveBackslash(ExpandConstant('{app}'))) = 0) then
    RaiseException('Changing installation scope in place is not supported.');
  if (Registered <> '') and (CompareText(RemoveBackslash(Registered), RemoveBackslash(ExpandConstant('{app}'))) <> 0) then
    RaiseException('Upgrade must use the registered installation directory: ' + Registered);
  if (Length(ExpandConstant('{app}')) < 5) or
     (CompareText(ExpandConstant('{app}'), ExpandConstant('{win}')) = 0) or
     (CompareText(ExpandConstant('{app}'), ExpandConstant('{sys}')) = 0) or
     (CompareText(ExpandConstant('{app}'), ExpandConstant('{autopf}')) = 0) then
    RaiseException('Choose a dedicated application directory.');
  if Registered = '' then begin
    HasEntries := False; RetainedDataOnly := True;
    if FindFirst(ExpandConstant('{app}\*'), Find) then try
      repeat
        if (Find.Name <> '.') and (Find.Name <> '..') then begin
          HasEntries := True;
          if (CompareText(Find.Name, 'data') <> 0) or ((Find.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0) or
             ((Find.Attributes and ReparsePoint) <> 0) then RetainedDataOnly := False;
        end;
      until not FindNext(Find);
    finally FindClose(Find); end;
    if HasEntries then begin
      if not RetainedDataOnly or not MatchingReceipt(OldVersion) then
        RaiseException('New installations require an empty directory or this installer''s retained data. ZIP and unknown directories cannot be adopted.');
      if not StrToVersion(OldVersion, OldPacked) or not StrToVersion('{#AppVersion}', NewPacked) then
        RaiseException('The retained installation version is invalid.');
      if ComparePackedVersion(OldPacked, NewPacked) > 0 then RaiseException('Downgrades are not supported.');
      CheckDataTree(ExpandConstant('{app}\data'));
      Log('Recognized retained data using the matching installation ownership record.');
    end;
    NewInstallation := not HasEntries;
  end else begin
    NewInstallation := False;
    if not RegQueryStringValue(Root, UninstallKey, 'DisplayVersion', OldVersion) or
       not StrToVersion(OldVersion, OldPacked) or not StrToVersion('{#AppVersion}', NewPacked) then
      RaiseException('The registered version is invalid.');
    if ComparePackedVersion(OldPacked, NewPacked) > 0 then RaiseException('Downgrades are not supported.');
    if FileExists(ExpandConstant('{app}\Open-ST.exe')) then begin
      if not GetVersionNumbersString(ExpandConstant('{app}\Open-ST.exe'), ExeVersion) or
         not StrToVersion(ExeVersion, OldPacked) then RaiseException('The installed executable version is invalid.');
      if ComparePackedVersion(OldPacked, NewPacked) > 0 then RaiseException('The executable is newer than this installer.');
    end;
  end;
end;

// 读取已安装 x64 运行库并比较最低要求，避免仅相信安装器退出码。
// 入参：无。
// 返回：已安装兼容运行库为 true。
function RuntimePresent: Boolean;
var Installed: Cardinal; Version: String; Current, Required: Int64;
begin
  Result := False;
  if RegQueryDWordValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed) and
     (Installed = 1) and RegQueryStringValue(HKLM64, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Version', Version) then begin
    if Copy(Version, 1, 1) = 'v' then Delete(Version, 1, 1);
    if StrToVersion(Version, Current) and StrToVersion('{#RedistVersion}', Required) then
      Result := ComparePackedVersion(Current, Required) >= 0;
  end;
end;

// 检测并按用户确认安装固定运行库；失败不触碰程序文件。
// 入参：NeedsRestart 输出是否需要系统重启。
// 返回：错误文本，空表示可继续。
function EnsureRuntime(var NeedsRestart: Boolean): String;
var Code: Integer;
begin
  Result := '';
  if RuntimePresent then exit;
  if SuppressibleMsgBox('Microsoft Visual C++ x64 Runtime {#RedistVersion} is required. Install it now? This may request administrator permission.', mbConfirmation, MB_YESNO, IDNO) <> IDYES then begin
    Result := 'Required runtime installation was not approved.'; exit;
  end;
  ExtractTemporaryFile('vc_redist.x64.exe');
  if not ShellExec('open', ExpandConstant('{tmp}\vc_redist.x64.exe'), '/install /passive /norestart', '', SW_SHOWNORMAL, ewWaitUntilTerminated, Code) then
    Result := 'Could not start the runtime installer.'
  else if Code = 3010 then begin NeedsRestart := True; Result := 'The runtime requires a restart. Restart Windows and run Setup again.'; end
  else if Code <> 0 then Result := 'Runtime installation failed or was cancelled. Code: ' + IntToStr(Code)
  else if not RuntimePresent then Result := 'The required runtime could not be verified after installation.';
end;

// 清单相对路径必须位于安装根内，不允许数据与目录穿越。
// 入参：Relative 为文件路径。
// 返回：通过验证的完整本地路径。
function ManagedPath(Relative: String): String;
begin
  StringChangeEx(Relative, '/', '\', True);
  if (Relative = '') or (Pos(':', Relative) > 0) or (Pos('..', Relative) > 0) or
     (Copy(Relative, 1, 1) = '\') or (Copy(Relative, 1, 2) = '.\') or (Pos('\.\', Relative) > 0) or
     (CompareText(Copy(Relative, 1, 5), 'data\') = 0) then
    RaiseException('Invalid managed file manifest.');
  Result := ExpandConstant('{app}\') + Relative;
end;

// 备份被修改资源并锚定所有旧文件父目录，未知文件不进入删除清单。
// 入参：无。
// 返回：无；备份失败在覆盖前终止。
procedure BackupModifiedResources;
var I: Integer; Line, Relative, Source, Target: String; ProtectedFile: THandle; Information: TFileInformation;
begin
  if not FileExists(ExpandConstant('{app}\release-files.sha256')) then exit;
  Source := ExpandConstant('{app}\release-files.sha256');
  ProtectedFile := CreateFileW(Source, $80000080, 1, 0, 3, $00200000, 0);
  if ProtectedFile = InvalidHandle then RaiseException('Cannot protect the previous file manifest.');
  try
    if not GetFileInformationByHandle(ProtectedFile, Information) or (Information.Links <> 1) or
       ((Information.Attributes and ReparsePoint) <> 0) then RaiseException('Invalid manifest identity.');
    ApplyPermissions(Source, False);
    if not LoadStringsFromFile(Source, OldManifest) then RaiseException('Cannot read previous managed file manifest.');
  finally CloseHandle(ProtectedFile); end;
  BackupFolder := '';
  for I := 0 to GetArrayLength(OldManifest) - 1 do begin
    Line := OldManifest[I];
    if Length(Line) < 67 then RaiseException('Invalid previous managed file manifest.');
    Relative := Copy(Line, 67, MaxInt); Source := ManagedPath(Relative);
    if FileExists(Source) then begin
      EnsureAnchoredDirectory(ExtractFileDir(Source));
      ProtectedFile := CreateFileW(Source, $80000080, 1, 0, 3, $00200000, 0);
      if ProtectedFile = InvalidHandle then RaiseException('Cannot protect installed file for backup: ' + Source);
      try
      if not GetFileInformationByHandle(ProtectedFile, Information) or (Information.Links <> 1) or
         ((Information.Attributes and ReparsePoint) <> 0) then RaiseException('Invalid installed file identity.');
      ApplyPermissions(Source, False);
      if (Copy(Relative, 1, 10) = 'resources/') and (CompareText(GetSHA256OfFile(Source), Copy(Line, 1, 64)) <> 0) then begin
        if BackupFolder = '' then begin
          BackupFolder := ExpandConstant('{app}\data\upgrade-backups\{#AppVersion}-') + GetDateTimeString('yyyymmdd-hhnnss', #0, #0);
          if DirExists(BackupFolder) then RaiseException('Backup directory already exists; manually try again.');
        end;
        Log('Backing up modified resource: ' + Relative);
        Target := BackupFolder + '\' + Relative;
        EnsureAnchoredDirectory(ExtractFileDir(Target));
        if not CopyFile(Source, Target, True) then RaiseException('Cannot back up modified resource: ' + Source);
      end;
      finally CloseHandle(ProtectedFile); end;
    end;
  end;
  if BackupFolder <> '' then SuppressibleMsgBox('Modified resources were backed up to:' + #13#10 + BackupFolder, mbInformation, MB_OK, IDOK);
end;

// 在用户确认实际安装后取得维护资格，前置失败保持程序文件不变。
// 入参：NeedsRestart 为运行库重启结果。
// 返回：空为继续，文本为立即失败提示。
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := ''; ReleaseProtection;
  try
    Log('Validating installation identity and version.');
    ValidateDestination;
    Log('Acquiring installation maintenance lease.');
    AcquireMaintenance;
    Log('Checking the required runtime.');
    Result := EnsureRuntime(NeedsRestart);
    if Result = '' then begin
      Log('Checking modified resources for backup.');
      BackupModifiedResources;
      Log('Installation preparation completed.');
    end;
  except Result := GetExceptionMessage; end;
  if Result <> '' then begin
    // 首次安装前置失败只清理本次空协调目录；不递归触碰未知内容。
    if NewInstallation and (Lease <> InvalidHandle) then begin
      CloseHandle(Lease); Lease := InvalidHandle;
      DeleteFile(ExpandConstant('{app}\data\.coordination\install.lock'));
    end;
    ReleaseProtection;
    if NewInstallation then begin
      RemoveDir(ExpandConstant('{app}\data\.coordination'));
      RemoveDir(ExpandConstant('{app}\data'));
      RemoveDir(ExpandConstant('{app}'));
    end;
  end;
end;

// 初始化纯状态，Setup 打开时不阻挡仍在运行的 App。
// 入参：无。
// 返回：允许显示标准安装界面。
function InitializeSetup: Boolean;
begin
  Lease := InvalidHandle; Result := True;
end;

// 根据新清单移除旧版废弃且未修改文件，保留未知内容。
// 入参：CurStep 为标准安装阶段。
// 返回：无。
procedure CurStepChanged(CurStep: TSetupStep);
var Current: TArrayOfString; I, J: Integer; Relative, Path: String; Present: Boolean;
begin
  if CurStep = ssPostInstall then begin
    if not LoadStringsFromFile(ExpandConstant('{app}\release-files.sha256'), Current) then RaiseException('Cannot verify installed manifest.');
    for I := 0 to GetArrayLength(Current) - 1 do begin
      Path := ManagedPath(Copy(Current[I], 67, MaxInt));
      if CompareText(GetSHA256OfFile(Path), Copy(Current[I], 1, 64)) <> 0 then RaiseException('Installed file verification failed: ' + Path);
      ApplyPermissions(Path, False);
    end;
    for I := 0 to GetArrayLength(OldManifest) - 1 do begin
      Relative := Copy(OldManifest[I], 67, MaxInt); Present := False;
      for J := 0 to GetArrayLength(Current) - 1 do
        if CompareText(Relative, Copy(Current[J], 67, MaxInt)) = 0 then Present := True;
      Path := ManagedPath(Relative);
      if not Present and FileExists(Path) then
        if CompareText(GetSHA256OfFile(Path), Copy(OldManifest[I], 1, 64)) = 0 then
          if not DeleteFile(Path) then RaiseException('Cannot remove retired owned file: ' + Path);
    end;
    ApplyPermissions(ExpandConstant('{app}\release-files.sha256'), False);
    StoreReceipt;
    ReleaseProtection;
  end;
end;

// 任意退出回收系统资源，异常失败由标准安装器回滚已记录操作。
// 入参：无。
// 返回：无。
procedure DeinitializeSetup;
begin
  ReleaseProtection;
end;

// 逐层持有含 DELETE 权限且禁止改名的句柄，按该身份删除而不跟随路径。
// 入参：Path 为父目录已经锚定的子项。
// 返回：失败立即终止并保留剩余数据。
procedure DeleteDataItem(Path: String);
var H: THandle; Info: TFileInformation; Find: TFindRec; Disposition: LongWord;
begin
  H := CreateFileW(Path, $10081, 3, 0, 3, $02200000, 0);
  if H = InvalidHandle then RaiseException('Cannot protect data for deletion: ' + Path);
  try
    if not GetFileInformationByHandle(H, Info) or ((Info.Attributes and ReparsePoint) <> 0) then
      RaiseException('Unsafe data path; remaining data was preserved.');
    if (Info.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
      if FindFirst(AddBackslash(Path) + '*', Find) then try
        repeat
          if (Find.Name <> '.') and (Find.Name <> '..') then DeleteDataItem(AddBackslash(Path) + Find.Name);
        until not FindNext(Find);
      finally FindClose(Find); end;
    Disposition := 1;
    if not SetFileInformationByHandle(H, 4, Disposition, 4) then
      RaiseException('Cannot remove data item; remaining data was preserved: ' + Path);
  finally CloseHandle(H); end;
end;

// 卸载开始取得维护锁，默认保留数据并明确跨账户自启限制。
// 入参：无。
// 返回：是否可以开始卸载。
function InitializeUninstall: Boolean;
begin
  Lease := InvalidHandle; Result := False;
  try
    AcquireMaintenance;
    CheckDataTree(ExpandConstant('{app}'));
    DeleteData := SuppressibleMsgBox('Delete this installation''s data (settings, logs, backups and update cache)? Default is No.', mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES;
    if IsAdminInstallMode then
      SuppressibleMsgBox('Each Windows user must disable Open-ST startup in the application before uninstalling. Startup entries belonging to other accounts are not removed.', mbInformation, MB_OK, IDOK);
    Result := True;
  except SuppressibleMsgBox(GetExceptionMessage, mbError, MB_OK, IDOK); ReleaseProtection; end;
end;

// 程序文件已被卸载后才释放稳定协调路径并按用户选择清理数据。
// 入参：CurUninstallStep 为标准卸载阶段。
// 返回：无。
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var Command, Expected: String;
begin
  if (CurUninstallStep = usUninstall) and not IsAdminInstallMode then begin
    Expected := '"' + ExpandConstant('{app}\Open-ST.exe') + '" --startup';
    if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Open-ST', Command) then
      if CompareStr(Command, Expected) = 0 then
        if not RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'Open-ST') then
          RaiseException('Cannot remove this installation''s startup entry.');
  end;
  if CurUninstallStep = usPostUninstall then begin
    if DeleteData then begin
      if FileExists(ExpandConstant('{app}\Open-ST.exe')) then RaiseException('Executable remains; data was preserved.');
      CheckDataTree(ExpandConstant('{app}\data'));
      ReleaseProtection;
      EnsureAnchoredDirectory(ExpandConstant('{app}'));
      DeleteDataItem(ExpandConstant('{app}\data'));
      RemoveReceipt;
    end;
  end;
end;

// 关闭卸载进程时释放保护，默认数据与未知文件仍保留。
// 入参：无。
// 返回：无。
procedure DeinitializeUninstall;
begin
  ReleaseProtection;
end;
