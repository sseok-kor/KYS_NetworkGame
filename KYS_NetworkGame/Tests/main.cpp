#include "pch_tests.h"

// GoogleTest 러너 진입점.
// gtest_main.lib를 링크하지 않고 직접 main을 두는 이유:
//   콘솔에서 한글 로그가 깨지지 않게 setlocale을 먼저 걸기 위함 (기존 러너와 동일).
int main(int argc, char** argv)
{
	::setlocale(LC_ALL, "");                  // 콘솔 한글 로그 로캘
	::testing::InitGoogleTest(&argc, argv);   // --gtest_filter 등 커맨드라인 파싱
	return RUN_ALL_TESTS();                   // 등록된 모든 TEST 실행, 전부 성공 0, 하나라도 실패 1 반환
}
