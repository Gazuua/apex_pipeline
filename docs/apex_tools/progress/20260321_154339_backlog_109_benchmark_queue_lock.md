# BACKLOG-109: 벤치마크 빌드 큐잉 시스템 강제

- **브랜치**: feature/backlog-109-benchmark-queue-lock
- **PR**: #66

## 작업 요약

벤치마크 실행을 `queue-lock.sh` 큐잉 시스템에 강제 편입시켜 빌드/벤치마크 간 CPU 경합을 방지한다.

## 변경 내역

### queue-lock.sh — `benchmark` 서브커맨드 추가
- `do_benchmark()` 함수: `build` 채널 lock 공유, per-execution lock 패턴
- exe 존재 검증, exit code 전파, trap EXIT lock 해제
- main() case에 `benchmark` 분기 추가

### validate-build.sh — 벤치마크 직접 실행 차단
- `\bbench_\w+` 패턴 추가로 `bench_*.exe` 직접 호출 차단
- `queue-lock.sh` 경유 호출은 기존 로직으로 허용

### 벤치마크 README — 워크플로우 갱신
- 모든 실행 예시를 `queue-lock.sh benchmark` 경유로 변경
- `bench_architecture_comparison` 누락 보완 (auto-review 발견)

## 설계 결정

- **채널 전략**: `build` 채널 공유 — 별도 채널 대비 구현 단순, 상호배제 자동 보장
- **lock 운용**: Per-execution lock — 세션 lock 대비 lock 점유 시간 최소

## auto-review 결과

- 발견 1건 (Minor): `bench_architecture_comparison` README 누락 → 수정 완료
