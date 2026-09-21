// Windows UTF-16 문자열과 앱 내부 UTF-8 문자열 사이의 엄격한 변환 함수를 선언한다.
#pragma once
#include <string>
namespace qm
{
// UTF-16→UTF-8. 변환할 수 없는 입력은 손실 대체 문자로 숨기지 않고 예외로 알린다.
std::string utf8(const std::wstring &);
// UTF-8→UTF-16. Windows의 W 접미사 API에 전달할 문자열을 만든다.
std::wstring wide(const std::string &);
} // namespace qm
