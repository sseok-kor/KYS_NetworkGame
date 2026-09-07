#include "pch_tests.h"
#include "TestHarness/MockJob.h"

// static 멤버 실체 정의 (여러 번역 단위가 링크될 때 여기 한 곳에서만 정의)
volatile LONG KYS::TESTS::MockJob::s_executeCount = 0;
volatile LONG KYS::TESTS::MockJob::s_destroyCount = 0;
