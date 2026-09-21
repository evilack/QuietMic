Unicode true
!include "MUI2.nsh"
; !로 시작하는 지시문은 컴파일할 때, Function/Section 내부 명령은 설치기를 실행할 때 처리된다.
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "WinVer.nsh"
!include "${__FILEDIR__}\process_guard.nsh"
Name "QuietMic"
OutFile "${OUTPUT_FILE}"
InstallDir "$PROGRAMFILES64\QuietMic"
RequestExecutionLevel admin
SetCompressor zlib
ShowInstDetails show
ShowUninstDetails show
Var TargetRoot
Var Result
!macro AcquireInstallLock
 ; 모든 사용자 세션에서 공유하는 이름의 mutex 객체로 설치/제거의 중복 실행을 감지한다.
 ; 여기서는 소유권을 기다리는 잠금 대신 '이미 같은 이름의 객체가 있는가'를 검사한다.
 ; 핸들을 유지하면 객체가 존속하고, 설치기 프로세스 종료 시 OS가 핸들을 닫는다.
 System::Call 'kernel32::CreateMutexW(p 0, i 0, w "Global\QuietMic.InstallOperation") p.r9 ?e'
 Pop $8
 ${If} $9 == 0
 ${OrIf} $8 == 183
  MessageBox MB_ICONSTOP "$(InstallerBusy)" /SD IDOK
  Abort
 ${EndIf}
!macroend
!define MUI_ABORTWARNING
!define MUI_LANGDLL_ALWAYSSHOW
!define MUI_LANGDLL_REGISTRY_ROOT "HKLM"
!define MUI_LANGDLL_REGISTRY_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\QuietMic"
!define MUI_LANGDLL_REGISTRY_VALUENAME "InstallerLanguage"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!include "${__FILEDIR__}\languages.nsh"

!macro ValidateLanguage
 ${If} $LANGUAGE != ${LANG_ENGLISH}
 ${AndIf} $LANGUAGE != ${LANG_KOREAN}
 ${AndIf} $LANGUAGE != ${LANG_JAPANESE}
 ${AndIf} $LANGUAGE != ${LANG_SIMPCHINESE}
 ${AndIf} $LANGUAGE != ${LANG_TRADCHINESE}
 ${AndIf} $LANGUAGE != ${LANG_SPANISH}
 ${AndIf} $LANGUAGE != ${LANG_FRENCH}
 ${AndIf} $LANGUAGE != ${LANG_GERMAN}
  StrCpy $LANGUAGE ${LANG_ENGLISH}
 ${EndIf}
!macroend

Function .onInit
 ; 파일 복사 전에 운영체제/CPU 조건을 확인하고 64비트 레지스트리 뷰를 선택한다.
 SetRegView 64
 StrCpy $LANGUAGE ${LANG_ENGLISH}
 ${GetParameters} $0
 ${GetOptions} $0 "/LANG=" $1
 ${If} $1 != ""
  StrCpy $LANGUAGE $1
 !insertmacro ValidateLanguage
 ${EndIf}
 !insertmacro MUI_LANGDLL_DISPLAY
 !insertmacro AcquireInstallLock
 ${IfNot} ${IsNativeAMD64}
  MessageBox MB_ICONSTOP "$(RequiresX64)"
  Abort
 ${EndIf}
 ${IfNot} ${AtLeastWin11}
  MessageBox MB_ICONSTOP "$(RequiresWin11)"
  Abort
 ${EndIf}
FunctionEnd

Function .onInstSuccess
 ; 재부팅 플래그는 UI 표시용 상태일 뿐 프로세스 종료 코드를 바꾸지 않는다.
 ; 무인 설치 호출자도 '성공했지만 재부팅 필요'를 구분하도록 성공 시에만 3010을 반환한다.
 ; Abort로 끝난 실패는 이 콜백에 들어오지 않으므로 기존 오류 코드가 보존된다.
 IfRebootFlag 0 done
 SetErrorLevel 3010
 done:
FunctionEnd

Function un.onUninstSuccess
 ; 드라이버 제거 또는 잠긴 파일의 지연 삭제가 설정한 재부팅 상태를 호출자에게 전달한다.
 ; 여기서는 재부팅을 실행하지 않는다.
 IfRebootFlag 0 done
 SetErrorLevel 3010
 done:
FunctionEnd

Section "$(MainSection)"
 ; NSIS 임시 폴더에 먼저 풀어 서명을 검사한다. 여기서는 Program Files에 설치하지 않는다.
 InitPluginsDir
 StrCpy $TargetRoot $PLUGINSDIR
 ClearErrors
 !include "${PAYLOAD_INSTALL}"
 IfErrors helper_failed
 ClearErrors
 ExecWait '"$PLUGINSDIR\quietmic_cli.exe" --verify-driver "$PLUGINSDIR\components\usbip-drivers"' $Result
 IfErrors helper_failed
 ${If} $Result != 0
  DetailPrint "$(DriverVerificationFailed)"
  SetErrorLevel 1
  Abort
 ${EndIf}
 ClearErrors
 ExecWait '"$PLUGINSDIR\quietmic_cli.exe" --require-app-stopped' $Result
 ; 실행 중인 파일을 바꿔 버전이 섞이지 않도록 모든 QuietMic 세션이 종료됐는지 확인한다.
 IfErrors helper_failed
 ${If} $Result != 0
  MessageBox MB_ICONSTOP "$(CloseApp)" /SD IDOK
  SetErrorLevel 1
  Abort
 ${EndIf}
 StrCpy $TargetRoot $INSTDIR
 ; 같은 파일 목록을 이제 최종 설치 경로에 복사한다.
 SetOverwrite on
 ClearErrors
 !include "${PAYLOAD_INSTALL}"
 ${If} ${Errors}
  DetailPrint "$(CopyFailed)"
  SetErrorLevel 1
  Abort
 ${EndIf}
 WriteUninstaller "$INSTDIR\Uninstall.exe"
 ; 드라이버 설치 전에 제거 도구와 앱 목록을 준비해 실패 후 재시도/제거 경로를 남긴다.
 IfErrors helper_failed
 SetShellVarContext all
 CreateDirectory "$SMPROGRAMS\QuietMic"
 CreateShortcut "$SMPROGRAMS\QuietMic\QuietMic.lnk" "$INSTDIR\QuietMic.exe"
 WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\QuietMic" "DisplayName" "QuietMic"
 WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\QuietMic" "DisplayVersion" "0.1.0"
 WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\QuietMic" "UninstallString" '$\"$INSTDIR\Uninstall.exe$\"'
 WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\QuietMic" "InstallLocation" "$INSTDIR"
 WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\QuietMic" "DisplayIcon" "$INSTDIR\QuietMic.exe"
 WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\QuietMic" "InstallerLanguage" "$LANGUAGE"
 WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\QuietMic" "NoModify" 1
 WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\QuietMic" "NoRepair" 1
 WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\QuietMic" "DriverRemoved" 0
 IfErrors helper_failed
 ; Keep a usable recovery/uninstaller before modifying any audio device.
 ClearErrors
 ExecWait '"$INSTDIR\quietmic_cli.exe" --install-audio "$INSTDIR\components\usbip-drivers"' $Result
 IfErrors helper_failed
 ${If} $Result == 3010
  ; 3010은 실패가 아니라 재부팅 필요를 뜻한다. 여기서 강제로 재부팅하지 않는다.
  SetRebootFlag true
 ${ElseIf} $Result != 0
  DetailPrint "$(SetupFailed)"
  SetErrorLevel 1
  Abort
 ${EndIf}
 Goto install_done
 helper_failed:
 DetailPrint "$(HelperFailed)"
 SetErrorLevel 1
 Abort
 install_done:
SectionEnd

Function un.onInit
 SetRegView 64
 ReadRegStr $LANGUAGE HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\QuietMic" "InstallerLanguage"
 !insertmacro ValidateLanguage
 !insertmacro AcquireInstallLock
FunctionEnd
Section "Uninstall"
 !insertmacro StopQuietMicForUninstall
 ; 실행 중인 QuietMic을 종료한 뒤 가상 장치와 두 USB/IP 드라이버 패키지를 제거한다.
 ; 실패하면 재시도에 필요한 도우미/앱 파일을 보존한다.
 ReadRegDWORD $Result HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\QuietMic" "DriverRemoved"
 ${If} $Result != 1
 ClearErrors
 ExecWait '"$INSTDIR\quietmic_cli.exe" --remove-audio' $Result
 IfErrors uninstall_failed
 ${If} $Result == 3010
  SetRebootFlag true
 ${ElseIf} $Result != 0
  MessageBox MB_ICONSTOP "$(UninstallCleanupFailed)" /SD IDOK
  SetErrorLevel 1
  Abort
 ${EndIf}
 WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\QuietMic" "DriverRemoved" 1
 ; 후속 파일 삭제만 실패해 다시 실행해도 이미 끝난 드라이버 제거는 반복하지 않는다.
 IfErrors uninstall_failed
 ${EndIf}
 ClearErrors
 !include "${PAYLOAD_REMOVE}"
 ; 생성 목록의 파일만 삭제한다. 알 수 없는 사용자 파일을 재귀적으로 지우지 않는다.
 SetShellVarContext all
 Delete "$SMPROGRAMS\QuietMic\QuietMic.lnk"
 RMDir "$SMPROGRAMS\QuietMic"
 SetShellVarContext current
 Delete "$LOCALAPPDATA\QuietMic\settings.ini"
 RMDir "$LOCALAPPDATA\QuietMic"
 SetShellVarContext all
 DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\QuietMic"
 Delete /REBOOTOK "$INSTDIR\Uninstall.exe"
 RMDir "$INSTDIR"
 Goto uninstall_done
 uninstall_failed:
 MessageBox MB_ICONSTOP "$(UninstallHelperFailed)" /SD IDOK
 SetErrorLevel 1
 Abort
 uninstall_done:
SectionEnd
