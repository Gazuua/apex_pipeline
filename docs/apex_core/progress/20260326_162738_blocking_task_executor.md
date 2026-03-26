# Progress: BlockingTaskExecutor + AuthService bcrypt offload

**PR**: #196
**BACKLOG**: 146
**브랜치**: `feature/bcrypt-thread-offload`

## 작업 결과

코어 프레임워크에 `BlockingTaskExecutor`를 추가하여 CPU-bound 작업을 별도 thread pool로 offload하는 메커니즘 구축. AuthService의 bcrypt verify/hash를 적용하여 코어 IO 스레드 블로킹 문제 해소.

## 변경 사항

### 신규 파일
- `apex_core/include/apex/core/blocking_task_executor.hpp` — awaitable thread pool wrapper
- `apex_core/tests/unit/test_blocking_task_executor.cpp` — 유닛 테스트 11건

### 수정 파일
- `server_config.hpp` — `blocking_pool_threads` 필드 (기본 2)
- `server.hpp` / `server.cpp` — Server 멤버 + 생성/shutdown 통합
- `service_base.hpp` — `blocking_executor()` 접근자 + `bind_blocking_executor()` virtual
- `auth_service.cpp` — `on_login` bcrypt verify + `on_start` password seeding offload
- `apex_core_guide.md` — §7 blocking_executor() 섹션, §2.1 ServerConfig, #5 해결 방안

## 테스트

- 유닛 테스트 11건: 기본 실행, string 반환, void, 동시 실행, executor 복귀, 예외 전파, thread_count, pool saturation, shutdown 대기, double shutdown, move-only 반환
- 로컬 빌드 96/96 전체 통과

## auto-review

5명 리뷰어 (design, logic, systems, test, docs-spec) 디스패치.
발견 이슈 14건 전부 수정 완료 (MAJOR 4, MINOR 10). 잔여 이슈 0건.
