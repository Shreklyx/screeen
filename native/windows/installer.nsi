; Screen Share for Windows (native) – installer. For this user only (no administrator rights),
; into %LOCALAPPDATA%\Programs\ScreenShare, with Start menu and desktop shortcuts.
;   makensis /DVERSION=2.0.123 /DEXE=path\ScreenShare.exe /DOUT=ScreenShare-Setup.exe installer.nsi
; Silent: ScreenShare-Setup.exe /S  (the in-app update adds /run: the app starts when done)
; It also replaces the earlier Screen Share (Electron): that app's own updater runs this installer
; (--updated /S --force-run), and its per-user install is removed before this one goes in.

Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"

!ifndef VERSION
  !define VERSION "2.0.0"
!endif

Name "Screen Share"
OutFile "${OUT}"
InstallDir "$LOCALAPPDATA\Programs\ScreenShare"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "Screen Share ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "Screen Share"
VIAddVersionKey "FileDescription" "Screen Share – telepítő"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"

!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\ScreenShareNative"
; the Electron app (electron-builder, per user): its key is a UUID v5 of its app id
!define ELECTRON_KEY "Software\f099d69f-07c3-529a-ae81-aacc543a1020"
!define ELECTRON_UNINSTALLER "Uninstall Screen Share.exe"
!define MUI_ICON "app.ico"
!define MUI_UNICON "app.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\ScreenShare.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Screen Share indítása"

!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Hungarian"

Section "Screen Share"
  ; the Electron app first (its uninstaller would take the shortcuts made below): silently, in
  ; place (_?=), so that it has finished before this goes on
  ReadRegStr $9 HKCU "${ELECTRON_KEY}" "InstallLocation"
  StrCmp $9 "" 0 +2
    StrCpy $9 "$LOCALAPPDATA\Programs\screenshare-desktop"
  IfFileExists "$9\${ELECTRON_UNINSTALLER}" 0 noElectron
    ExecWait '"$9\${ELECTRON_UNINSTALLER}" /S /currentuser _?=$9'
    Delete "$9\${ELECTRON_UNINSTALLER}"
    RMDir /r "$9"
  noElectron:
  SetOutPath "$INSTDIR"
  ; the app's windows (there may be several) end: each, asked with WM_APP+6, ends its session and closes
  StrCpy $2 0
  closeWindows:
    FindWindow $1 "ScreenShare"
    StrCmp $1 0 closed
    SendMessage $1 0x8006 0 0 /TIMEOUT=3000
    Sleep 150
    IntOp $2 $2 + 1
    IntCmp $2 30 closed
    Goto closeWindows
  closed:
  ; the app that started this update closes itself: wait until its file is free
  StrCpy $0 0
  retry:
    ClearErrors
    Delete "$INSTDIR\ScreenShare.exe"
    IfErrors 0 free
    IntOp $0 $0 + 1
    IntCmp $0 50 free
    Sleep 200
    Goto retry
  free:
  File "/oname=ScreenShare.exe" "${EXE}"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\Screen Share.lnk" "$INSTDIR\ScreenShare.exe"
  CreateShortcut "$DESKTOP\Screen Share.lnk" "$INSTDIR\ScreenShare.exe"
  WriteRegStr HKCU "${UNINST}" "DisplayName" "Screen Share"
  WriteRegStr HKCU "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST}" "Publisher" "Screen Share"
  WriteRegStr HKCU "${UNINST}" "DisplayIcon" "$INSTDIR\ScreenShare.exe"
  WriteRegStr HKCU "${UNINST}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINST}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $1 $2 $3
  WriteRegDWORD HKCU "${UNINST}" "EstimatedSize" $1
  ; an in-app update (/run) or the Electron app's (--force-run): start the new version
  ${GetParameters} $R0
  ${GetOptions} $R0 "/run" $R1
  IfErrors 0 run
  ${GetOptions} $R0 "--force-run" $R1
  IfErrors done
  run:
    Exec '"$INSTDIR\ScreenShare.exe"'
  done:
SectionEnd

Section "Uninstall"
  StrCpy $2 0
  closeWindows:
    FindWindow $1 "ScreenShare"
    StrCmp $1 0 closed
    SendMessage $1 0x8006 0 0 /TIMEOUT=3000
    Sleep 150
    IntOp $2 $2 + 1
    IntCmp $2 30 closed
    Goto closeWindows
  closed:
  Sleep 300
  Delete "$INSTDIR\ScreenShare.exe"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\Screen Share.lnk"
  Delete "$DESKTOP\Screen Share.lnk"
  DeleteRegKey HKCU "${UNINST}"
SectionEnd
