!macro StopQuietMicForUninstall
 ; 모든 사용자 세션의 QuietMic에 먼저 일반 종료를 요청하고, 남은 프로세스만 강제 종료한다.
 ; 고정된 이미지 이름만 사용하며 사용자 입력을 셸 명령으로 연결하지 않는다.
 nsExec::ExecToStack /TIMEOUT=10000 '"$SYSDIR\tasklist.exe" /FI "IMAGENAME eq QuietMic.exe" /FO CSV /NH'
 Pop $0
 Pop $1
 ${If} $0 != 0
  SetErrorLevel 1
  Abort
 ${EndIf}
 System::Call 'shlwapi::StrStrIW(w r1, w "QuietMic.exe") p.r2'
 ${If} $2 != 0
  nsExec::ExecToStack /TIMEOUT=10000 '"$SYSDIR\taskkill.exe" /IM QuietMic.exe /T'
  Pop $0
  Pop $1
  nsExec::ExecToStack /TIMEOUT=10000 '"$SYSDIR\tasklist.exe" /FI "IMAGENAME eq QuietMic.exe" /FO CSV /NH'
  Pop $0
  Pop $1
  System::Call 'shlwapi::StrStrIW(w r1, w "QuietMic.exe") p.r2'
  ${If} $2 != 0
   nsExec::ExecToStack /TIMEOUT=10000 '"$SYSDIR\taskkill.exe" /F /IM QuietMic.exe /T'
   Pop $0
   Pop $1
   nsExec::ExecToStack /TIMEOUT=10000 '"$SYSDIR\tasklist.exe" /FI "IMAGENAME eq QuietMic.exe" /FO CSV /NH'
   Pop $0
   Pop $1
   System::Call 'shlwapi::StrStrIW(w r1, w "QuietMic.exe") p.r2'
   ${If} $2 != 0
    SetErrorLevel 1
    Abort
   ${EndIf}
  ${EndIf}
 ${EndIf}
!macroend
