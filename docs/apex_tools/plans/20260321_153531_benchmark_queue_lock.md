# BACKLOG-109: 벤치마크 빌드 큐잉 시스템 강제

## 배경

벤치마크와 빌드가 동시에 실행되면 CPU 경합으로 벤치마크 결과가 왜곡된다.
현재 빌드는 `queue-lock.sh`로 직렬화되지만, 벤치마크(`bench_*.exe`)는 직접 실행 가능하여 큐잉 시스템 밖에 있다.

## 목표

벤치마크 실행을 코드 레벨에서 `queue-lock.sh` 큐잉 시스템에 강제 편입시킨다.

## 설계 결정

### D1. 채널 전략: `build` 채널 공유

벤치마크 전용 채널을 신설하지 않고 기존 `build` 채널을 공유한다.
빌드 ↔ 벤치마크 상호배제가 자동으로 보장되어 CPU 경합을 방지한다.

### D2. Lock 운용: Per-execution lock

`build` 서브커맨드와 동일한 패턴 — lock 획득 → 벤치마크 1개 실행 → lock 해제.
세션 단위 lock(acquire/release)은 장시간 lock 점유로 빌드 큐 대기가 길어지므로 채택하지 않는다.

## 변경 사항

### 1. `apex_tools/queue-lock.sh` — `benchmark` 서브커맨드 추가

```
queue-lock.sh benchmark <exe_path> [benchmark_args...]
```

내부 동작:
1. `build` 채널 lock 획득 (기존 큐 대기 메커니즘 사용)
2. `exe_path` 존재 여부 + 실행 가능 여부 검증
3. 벤치마크 실행, stdout/stderr 그대로 전파
4. exit code 전파
5. lock 해제 (trap으로 비정상 종료 시에도 보장)

`do_benchmark()` 함수를 추가하며, `do_build()`와 동일한 구조를 따른다.
로그: `$QUEUE_DIR/logs/{BRANCH_ID}.log` (기존 빌드 로그와 동일 위치).

### 2. `.claude/hooks/validate-build.sh` — 벤치마크 직접 실행 차단

차단 패턴 추가:
- `bench_*` (경로 무관, Linux/Windows 모두)
- `*/benchmarks/bench_*` (경로 포함 패턴)

기존 허용 규칙 유지: `queue-lock.sh`로 시작하는 커맨드는 통과.

### 3. `apex_core/benchmarks/README.md` — 워크플로우 업데이트

직접 실행 예시를 `queue-lock.sh benchmark` 경유로 변경.

## 범위 밖

- `bench_main.cpp` (C++) 수정 없음 — lock은 스크립트 레벨에서 해결
- `generate_benchmark_report.py` 수정 없음 — JSON 결과만 읽음
- 새 채널 신설 없음
- 세션 단위 lock 없음

## 변경 파일

| 파일 | 변경 |
|------|------|
| `apex_tools/queue-lock.sh` | `benchmark` 서브커맨드 + `do_benchmark()` |
| `.claude/hooks/validate-build.sh` | `bench_*` 차단 패턴 |
| `apex_core/benchmarks/README.md` | 워크플로우 queue-lock 경유 반영 |
