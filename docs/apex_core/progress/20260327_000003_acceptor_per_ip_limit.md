# Acceptor Per-IP 연결 제한 완료 (BACKLOG-256)

- **PR**: #211
- **브랜치**: feature/acceptor-per-ip-limit

## 변경 요약

Seastar-style owner-shard 패턴으로 per-IP 연결 수를 Listener/Acceptor 프레임워크 레벨에서 제한. Gateway의 per-IP rate limiting(요청 수/초)과 상호보완하여 connection exhaustion 공격 방어.

## 핵심 설계

- `hash(IP) % num_cores`로 담당 코어 결정 → 해당 코어만 카운터 보유
- Accept 시: `cross_core_call`로 owner 코어에 체크+증가
- Close 시: `boost::asio::post`로 owner 코어에 감소 (fire-and-forget)
- mutex/atomic/CAS 없는 진정한 no-locking — shared-nothing 완전 준수

## auto-review

6건 발견 (CRITICAL 1, MAJOR 4, MINOR 1) — 전부 수정:
- owner_core() division-by-zero 가드
- decrement() uint32_t underflow 방지
- finalize_shutdown UAF: ~Server로 소멸 이동
- cross_core_call 타임아웃 보상 decrement
- shutdown 중 limiter null 체크

## 빌드/테스트

- 로컬: MSVC debug 97 테스트 통과
- CI: 3개 컴파일러 (MSVC + GCC + Clang) + ASAN/TSAN/UBSAN
