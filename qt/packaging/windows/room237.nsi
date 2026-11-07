; Per-user installer: no administrator rights, and the app can update itself in place.
!include "MUI2.nsh"
Unicode true
ManifestDPIAware true
SetDatablockOptimize on
Name "Room237"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\Room237"
InstallDirRegKey HKCU "Software\Room237" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
!define MUI_ICON "${SRCDIR}/app/resources/icon.ico"
!define MUI_UNICON "${SRCDIR}/app/resources/icon.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\room237.exe"
!insertmacro MUI_PAGE_LICENSE "${SRCDIR}/packaging/THIRD_PARTY_NOTICES.md"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Russian"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "Room237"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "Room237 Setup"
VIAddVersionKey "LegalCopyright" "AGPL-3.0-only"
Section
  SetShellVarContext current
  SetOutPath "$INSTDIR"
  File /r "${STAGE}/*.*"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortcut "$SMPROGRAMS\Room237.lnk" "$INSTDIR\room237.exe"
  WriteRegStr HKCU "Software\Room237" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Room237" "DisplayName" "Room237"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Room237" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Room237" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Room237" "DisplayIcon" "$INSTDIR\room237.exe"
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Room237" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Room237" "NoRepair" 1
SectionEnd
Section "Uninstall"
  SetShellVarContext current
  Delete "$SMPROGRAMS\Room237.lnk"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\Room237"
  DeleteRegKey HKCU "Software\Room237"
SectionEnd
