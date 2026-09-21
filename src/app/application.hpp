// Windows 진입 함수와 시작 절차의 구현을 분리하는 응용 프로그램 실행 선언이다.
#pragma once
#include <string>
#include <vector>
namespace qm
{
// arguments에는 실행 파일 이름을 뺀 인수만 들어온다. 반환값은 프로세스 종료 코드다.
int run_application(const std::vector<std::wstring> &arguments);
} // namespace qm
