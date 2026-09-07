# GoogleTest vendored 스냅샷

- **버전(pin)**: v1.16.0 — C++14를 지원하는 마지막 브랜치 (README.md 25행 "The 1.16.x branch requires at least C++14" / v1.17.0부터 C++17 최소)
- **출처**: https://github.com/google/googletest/archive/refs/tags/v1.16.0.tar.gz (2026-07-02 반입)
- **반입 방식**: 소스 직접 복사 (repo 선례 `ThirdParty/imgui`와 동일 — clone 한 번으로 오프라인 빌드 재현)
- **트림**: `googlemock/`·`docs/`·`test/`·`samples/`·`ci/`·CMake/Bazel 빌드 파일 제외. 유지 = `LICENSE`·`README.md`·`googletest/include/`·`googletest/src/`
- **빌드 방식**: 별도 lib 없이 `Tests.vcxproj`가 `googletest/src/gtest-all.cc` 1개를 직접 컴파일 (PCH 미사용·경고 억제). include 경로 2개 필요:
  - `ThirdParty/googletest/googletest/include` (공개 헤더 `gtest/gtest.h`)
  - `ThirdParty/googletest/googletest` (gtest-all.cc 내부의 `src/*.cc` 상대 include — gtest-all.cc 컴파일에만 필요)
- **CRT 정합**: 소비자(Tests)와 같은 vcxproj에서 컴파일되므로 런타임(/MDd·/MD) 불일치(LNK2038)가 구조적으로 불가능. CMake 전용 스위치 `gtest_force_shared_crt`는 해당 없음.
- **추기 (2026-07-03)**: Tests 프로젝트가 `/std:c++17`로 전환됨(프로덕션은 C++14 유지·tradeoff §275). v1.16.0 pin은 C++17에서도 유효하며 v1.17+ 선택지가 열렸으나 갱신 불요.
