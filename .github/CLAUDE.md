# CI/CD 트러블슈팅

- **TSAN**: Boost.Asio false positive → `tsan_suppressions.txt` (루트+apex_core 양쪽 배치). `race:boost::asio::detail::*` (atomic_thread_fence) + `mutex:boost::asio::detail::posix_mutex` (io_context 소멸 시)
- **ASAN/LSAN**: spdlog 글로벌 레지스트리 leak → `lsan_suppressions.txt`
- **ASAN aligned_alloc**: size는 alignment 배수여야 함. `max(capacity, alignment)`로 보정
- **CMakePresets ${sourceDir}**: 루트+하위 양쪽에 suppressions 파일 배치 (include 시 `${sourceDir}` 변환 대응)
- **[[nodiscard]]**: GCC에서 EXPECT_THROW 내 `[[nodiscard]]` 반환값 경고 → `(void)` 캐스트로 수정 (경고 무시 금지, 반드시 코드 수정)
- **경고 정책**: `/W4 /WX`(MSVC) + `-Wall -Wextra -Wpedantic -Werror`(GCC/Clang) 전 타겟 적용. 경고 0건이 CI 통과 조건. 상세: 루트 `CLAUDE.md` § 경고 정책
- **test preset**: TSAN_OPTIONS/LSAN_OPTIONS는 configure preset이 아닌 **test preset**에 설정
- **CI workflow**: `ctest --preset <name>` 사용 (--test-dir 대신)
- **vcpkg 다운로드 실패**: GitHub CDN 간헐적 HTTP 502 → `gh run rerun --failed`
- **CI path filter**: 소스(`.cpp/.hpp`, `CMakeLists.txt`, `vcpkg.json` 등) 미변경 시 빌드/테스트 자동 스킵 — 문서/도구만 변경된 커밋은 CI 불필요
- **CI 실패 분석 원칙**: 한 잡만 보고 판단하지 말고 **모든 실패 잡의 로그를 확인** — 잡마다 실패 원인이 다를 수 있음
- **CI 대기**: `gh run watch`는 **반드시 백그라운드(`run_in_background`)로 실행** — 타임아웃 제한 없이 완료까지 대기
- **GCC `SIZE_MAX`**: `<cstdint>` include 필수. MSVC는 transitively include되어 빌드되지만, GCC에서 직접 include하면 `'SIZE_MAX' was not declared` 에러
