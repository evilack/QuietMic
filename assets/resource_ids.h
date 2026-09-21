// 실행 파일 리소스의 아이콘/메뉴/문구 번호를 정의한다. Windows 어댑터 밖에서는 의미 있는 명령 열거형을 사용한다.
#pragma once

// Shared by the resource compiler and the Windows adapter; IDs never escape as application commands.
// 실행 파일 아이콘과 트레이 메뉴 전체를 찾는 리소스 ID다.
#define IDI_APP 1

#define IDR_TRAY_MENU 201
#define IDM_TRAY_SHOW 401
#define IDM_TRAY_MUTE 402
#define IDM_TRAY_PROCESSING 403
#define IDM_TRAY_QUIT 404
#define IDM_TRAY_DENOISE 405
#define IDM_TRAY_STARTUP 406
#define IDM_TRAY_VOLUME 407
