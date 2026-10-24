; Screen Share for Windows (native) – installer. For this user only (no administrator rights),
; into %LOCALAPPDATA%\Programs\ScreenShare, with Start menu and desktop shortcuts.
;   makensis /DVERSION=2.0.123 /DEXE=path\ScreenShare.exe /DOUT=ScreenShare-Setup.exe installer.nsi
; Silent: ScreenShare-Setup.exe /S  (the in-app update adds /run: the app starts when done)

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
  SetOutPath "$INSTDIR"
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
  ; an in-app update: start the new version
  ${GetParameters} $R0
  ${GetOptions} $R0 "/run" $R1
  IfErrors +2 0
    Exec '"$INSTDIR\ScreenShare.exe"'
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\ScreenShare.exe"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\Screen Share.lnk"
  Delete "$DESKTOP\Screen Share.lnk"
  DeleteRegKey HKCU "${UNINST}"
SectionEnd
