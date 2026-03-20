# Handoff Probe — Mid-Session Notification Detection

## 배경

SessionStart hook에서만 `branch-handoff.sh check`를 실행하므로, 세션 중 다른 브랜치의 알림(설계 변경 등)을 감지 못하는 사각지대가 존재한다. 이로 인해 에이전트가 옛 인터페이스 기반으로 수 시간 구현한 뒤 머지 시점에서야 충돌을 발견하는 시나리오가 발생할 수 있다.

## 해법

index watermark 비교 기반 2단 probe. 변경 감지(~1ms)와 full check(~100ms)를 분리하여, 변경이 없는 대다수의 호출에서는 비용 0으로 통과한다.

## 변경 범위

| 파일 | 변경 | 설명 |
|---|---|---|
| `.claude/hooks/handoff-probe.sh` | 신규 | Edit/Write PreToolUse hook. index 마지막 ID ↔ watermark 비교, 변경 시만 full check |
| `.claude/hooks/validate-handoff.sh` | 수정 | 기존 Bash PreToolUse hook 앞부분에 동일 probe 로직 삽입 |
| `.claude/settings.json` | 수정 | PreToolUse에 Edit/Write matcher → handoff-probe.sh 등록 |

## 동작

```
tool call (Edit/Write/Bash)
  │
  ├─ tail -1 index → 마지막 ID 추출
  ├─ cat watermarks/{BRANCH_ID} → 저장된 ID
  │
  ├─ 같으면 → exit 0 (0 토큰, ~1ms)
  └─ 다르면 → branch-handoff.sh check 실행
              → 알림 있으면 stderr 출력 (~150 토큰, 1회)
              → watermark 갱신 (이후 중복 출력 없음)
```

## 비용

- 평시: 0 토큰, ~1ms/call (프로세스 spawn 제외)
- Edit/Write hook 프로세스 spawn: ~20-40ms/call (Windows/MSYS)
- 알림 감지 시: ~150 토큰 1회
- 재작업 1회 방지 시 절감: ~19,000 토큰

## 안 바꾸는 것

- 기존 gate 정책 (머지 시점 차단)
- SessionStart hook
- 차단 동작 없음 (경고만)
