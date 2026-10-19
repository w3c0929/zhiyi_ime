; 知语输入法 / Zhiyu IME installer (NSIS 3.x, Unicode).
;
;   makensis /INPUTCHARSET UTF8 /DVERSION=1.6.5 /DBUILD_ID=ab12cd34 \
;           /DSRCDIR=..\dist\zhiyu-v1.6.5 scripts\zhiyu-setup.nsi
;
; Layout (mirrors upstream's multi-version model, docs/installation.md):
;   <base>\<版本号>.<8位十六进制>\   the program files of one version, plus uninstall.exe and
;                                    install-manifest.json
;   <base>\maintenance\install-state.json   the lifecycle state (active / prepared / retired)
;   <base>\update\                   (only while an install is staging)
; A new install gets a NEW version directory, so the running version's files are never
; overwritten (that is what used to make upgrades fail with locked DLLs).
;
; The installer is silent-capable: `/S` and `/D=<base>` (NSIS built-ins). scoop installs it as
;   Start-Process setup.exe '/S',"/D=$persist_dir\zhiyuime"
;
; Registry (64-bit view, written under HKLM as the programs read it):
;   HKLM\...\Uninstall\ZhiyuIME   the uninstall entry; InstallLocation is the ACTIVE version
;                                 directory (shared/server_launcher.cc reads it to start the
;                                 background service on demand, settings_launcher.cc to open
;                                 settings from the taskbar menu)
;   HKCU\Software\ZhiyuIME        InstallDir = the base directory (the directory page default)
;   HKCU\...\Run\ZhiyuIMEServer   "zhiyu-server.exe" --autostart  (sign-in start)

Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "WinVer.nsh"
!include "x64.nsh"

!ifndef VERSION
  !define VERSION "1.6.5"
!endif
; Eight hex digits, generated at packaging time: the version directory is "<VERSION>.<BUILD_ID>".
!ifndef BUILD_ID
  !define BUILD_ID "00000000"
!endif
!ifndef SRCDIR
  !define SRCDIR "..\dist\zhiyu-v${VERSION}"
!endif

!define APP_NAME "知语输入法"
!define APP_NAME_EN "Zhiyu IME"
!define COMPANY "Zhiyu IME"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ZhiyuIME"
!define RUN_VALUE "ZhiyuIMEServer"

Name "${APP_NAME} ${VERSION}"
OutFile "zhiyu-v${VERSION}-setup.exe"
; /D from the command line overrides this (scoop passes the persist directory).
InstallDir "$LOCALAPPDATA\ZhiyuIME"
InstallDirRegKey HKCU "Software\ZhiyuIME" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

Var AppDir     ; <base>\<version>.<build id>
Var OldActive  ; the version directory of the copy being replaced, if any
Var BaseDir    ; the base directory holding the version directories and maintenance

!macro RegisterTsf
  ExecWait '"$SYSDIR\regsvr32.exe" /s "$AppDir\zhiyu_tsf_x64.dll"'
  ${If} ${RunningX64}
    ExecWait '"$WINDIR\SysWOW64\regsvr32.exe" /s "$AppDir\zhiyu_tsf_x86.dll"'
  ${EndIf}
!macroend

!macro UnregisterTsf
  ExecWait '"$SYSDIR\regsvr32.exe" /s /u "$AppDir\zhiyu_tsf_x64.dll"'
  ${If} ${RunningX64}
    ExecWait '"$WINDIR\SysWOW64\regsvr32.exe" /s /u "$AppDir\zhiyu_tsf_x86.dll"'
  ${EndIf}
!macroend

; zhiyu_ime_<arch>.ime -> the system IMM module zhiyu.ime (the Keyboard Layouts entry written at
; registration names it; without the file the input method cannot be loaded).
!macro InstallImeModules
  ${If} ${RunningX64}
    ${DisableX64FSRedirection}
    SetOutPath "$WINDIR\System32"
    File /oname=zhiyu.ime "${SRCDIR}\zhiyu_ime_x64.ime"
    ${EnableX64FSRedirection}
    SetOutPath "$WINDIR\SysWOW64"
    File /oname=zhiyu.ime "${SRCDIR}\zhiyu_ime_x86.ime"
  ${Else}
    SetOutPath "$WINDIR\System32"
    File /oname=zhiyu.ime "${SRCDIR}\zhiyu_ime_x86.ime"
  ${EndIf}
  SetOutPath "$AppDir"
!macroend

!macro RemoveImeModules
  ${If} ${RunningX64}
    ${DisableX64FSRedirection}
    Delete /REBOOTOK "$WINDIR\System32\zhiyu.ime"
    ${EnableX64FSRedirection}
    Delete /REBOOTOK "$WINDIR\SysWOW64\zhiyu.ime"
  ${Else}
    Delete /REBOOTOK "$WINDIR\System32\zhiyu.ime"
  ${EndIf}
!macroend

Section "Install"
  SetShellVarContext all
  SetRegView 64

  ; A running copy holds its files and the TSF DLLs: stop it and let its uninstaller go first.
  ReadRegStr $OldActive HKLM "${UNINST_KEY}" "InstallLocation"
  ${If} $OldActive != ""
    IfFileExists "$OldActive\zhiyu-installer-helper.exe" 0 +2
      ExecWait '"$OldActive\zhiyu-installer-helper.exe" force-stop-server "$OldActive"'
    IfFileExists "$OldActive\uninstall.exe" 0 +2
      ExecWait '"$OldActive\uninstall.exe" /S _?=$OldActive'
  ${EndIf}

  ; Every version gets its own directory; the running version is never overwritten.
  StrCpy $AppDir "$INSTDIR\${VERSION}.${BUILD_ID}"
  CreateDirectory "$INSTDIR\maintenance"
  CreateDirectory $AppDir
  SetOutPath "$AppDir"
  File /r "${SRCDIR}\*.*"

  ; AppContainer 进程（Win11 的搜索框 SearchHost.exe、各种 UWP 应用）只加载它读得到的 DLL。
  ; C:\Program Files 与 System32 本来就给"所有应用程序包"授了读取和执行；装在别处（比如 scoop 的
  ; persist 目录）就没有这一条，结果是**搜索框里输入法整个不可用**（DLL 根本不加载，看起来像"被禁用"）。
  ; 这里按 SID 授权，与系统目录保持一致；失败不拦安装（只是那个环境里输入法仍然不可用）。
  nsExec::ExecToLog 'icacls "$AppDir" /grant *S-1-15-2-1:(OI)(CI)(RX) /T /C /Q'
  Pop $0
  nsExec::ExecToLog 'icacls "$AppDir" /grant *S-1-15-2-2:(OI)(CI)(RX) /T /C /Q'
  Pop $0

  ; A marker the settings program and the helper look for.
  FileOpen $0 "$AppDir\.zhiyu-install-complete" w
  FileClose $0

  WriteUninstaller "$AppDir\uninstall.exe"

  !insertmacro InstallImeModules
  !insertmacro RegisterTsf

  ; The lifecycle state (installer/src/installer_lifecycle_state.cc): this version is active;
  ; older version directories are re-discovered from the base directory when they are needed.
  FileOpen $0 "$INSTDIR\maintenance\install-state.json" w
  FileWrite $0 '{$\r$\n  "active": "${VERSION}.${BUILD_ID}",$\r$\n  "format": "zhiyu-install-lifecycle",$\r$\n  "prepared": "",$\r$\n  "retired": [],$\r$\n  "version": 1$\r$\n}$\r$\n'
  FileClose $0

  ; The uninstall entry: programs read InstallLocation (the ACTIVE version directory) from here.
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APP_NAME} (${APP_NAME_EN})"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "${COMPANY}"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$AppDir"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$AppDir\uninstall.exe"'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1

  WriteRegStr HKCU "Software\ZhiyuIME" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\ZhiyuIME" "InstallLocation" "$AppDir"

  ; Sign-in start; the server exits at once when startup.autostart is off.
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "${RUN_VALUE}" \
              '"$AppDir\zhiyu-server.exe" --autostart'

  ; A Start Menu folder of its own, so 知语 and upstream 知意 are easy to tell apart.
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}设置.lnk" "$AppDir\zhiyu-settings.exe"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" "$AppDir\uninstall.exe"

  IfFileExists "$AppDir\zhiyu-server.exe" 0 +2
    Exec '"$AppDir\zhiyu-server.exe"'
SectionEnd

Section "Uninstall"
  SetShellVarContext all
  SetRegView 64

  ; The registered active version directory; the base directory is its parent. Do not rely on
  ; $INSTDIR here: it is not the base, and the base is what holds maintenance\ and the version
  ; directories that must be removed.
  ReadRegStr $AppDir HKLM "${UNINST_KEY}" "InstallLocation"
  ${If} $AppDir != ""
    ${GetParent} "$AppDir" $BaseDir
  ${EndIf}
  ${If} $AppDir == ""
    StrCpy $AppDir "$INSTDIR"
  ${EndIf}
  ${If} $BaseDir == ""
    StrCpy $BaseDir "$INSTDIR"
  ${EndIf}

  IfFileExists "$AppDir\zhiyu-installer-helper.exe" 0 +2
    ExecWait '"$AppDir\zhiyu-installer-helper.exe" force-stop-server "$AppDir"'

  !insertmacro UnregisterTsf
  !insertmacro RemoveImeModules

  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "${RUN_VALUE}"
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKCU "Software\ZhiyuIME"

  RMDir /r "$SMPROGRAMS\${APP_NAME}"
  ; Removes the version directories, maintenance\ and any update\ staging left behind. Files
  ; still mapped into running programs (the TSF DLLs) are deleted at the next restart.
  Delete "$BaseDir\maintenance\install-state.json"
  RMDir /r /REBOOTOK "$BaseDir"
SectionEnd

Function .onInit
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_ICONSTOP "Windows 10 or later is required."
    Abort
  ${EndIf}
FunctionEnd
