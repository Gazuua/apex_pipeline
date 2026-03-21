# 핸드오프 시스템 강제화 설계

## 문제

병렬 에이전트들이 `branch-handoff.sh notify`를 호출하지 않아 핸드오프 시스템이 무용지물.
수신 측(probe/check)은 강제되지만, 발신 측(notify)은 CLAUDE.md 준수에만 의존하는 비대칭 구조.

결과: `backlog-check`이 AVAILABLE로 나오지만 실제로는 다른 에이전트가 작업 중 → 중복 착수·재작업 발생.

## 설계 원칙

- **동작 보장 우선** — 오버엔지니어링보다 강제력 확보가 먼저
- **예외 없는 차단** — active 미등록이면 문서든 소스든 모든 Edit/Write/git commit 차단
- **기존 인프라 확장** — 새 파일 없이 기존 4개 파일 수정만

## 상태 머신

```
notify start                 notify design           notify plan
(--skip-design 시             (설계 완료)             (계획 완료)
 바로 implementing)
     │                            │                       │
     ▼                            ▼                       ▼
  started ──────────────► design-notified ──────► implementing ──► (merged)
     │                                                              ▲
     └──── --skip-design ───────────────────────────────────────────┘
```

| 전환 | 명령 | 선행 상태 |
|------|------|-----------|
| → started | `notify start` | (신규) |
| → implementing | `notify start --skip-design` | (신규) |
| → design-notified | `notify design` | started |
| → implementing | `notify plan` | design-notified |
| → (삭제) | `notify merge` | 아무 상태 (기존 동작 유지) |

## Hook별 강제 로직

### 권한 매트릭스

| 상태 | Edit/Write 비소스 | Edit/Write 소스 | git commit |
|------|-------------------|-----------------|------------|
| **미등록** | **차단** | **차단** | **차단** |
| `started` | 허용 | **차단** | 허용 |
| `design-notified` | 허용 | **차단** | 허용 |
| `implementing` | 허용 | 허용 | 허용 |

### Hook 매핑

| Hook 파일 | 이벤트 | 체크 내용 |
|-----------|--------|-----------|
| `handoff-probe.sh` | PreToolUse (Edit\|Write) | active 등록 여부 → status별 소스/비소스 분기 차단 |
| `validate-handoff.sh` | PreToolUse (Bash) | `git commit` 시 active 등록 여부 차단 + 기존 merge 게이트 유지 |
| `session-context.sh` | SessionStart | feature/bugfix 브랜치 + active 미등록 시 경고 출력 |

### 예외

- **main/master 브랜치**: 핸드오프 체크 전부 스킵
- **branch-handoff.sh 자체 호출**: 항상 허용 (Bash hook에서 제외)
- **HANDOFF_DIR 미존재 / index 미존재**: 즉시 통과 (핸드오프 미사용 환경)

### SessionStart가 경고만 하는 이유

SessionStart hook은 세션 초기화 단계에서 실행되며, `exit 2`로 세션 자체를 차단할 수 없다.
따라서 경고 메시지를 출력하고, 실제 차단은 이후 첫 Edit/Write/Bash hook에서 수행한다.

## 에러 메시지

차단 시 무엇이 문제이고 어떻게 해결하는지 한 줄 안내:

```
차단: 핸드오프 미등록. 먼저 실행: branch-handoff.sh notify start --scopes <s> --summary "설명"
```

```
차단: 설계 미완료(status=started). 실행: branch-handoff.sh notify design --scopes <s> --summary "설계 요약"
  (설계 불필요 시: branch-handoff.sh notify start --skip-design)
```

```
차단: 구현 계획 미완료(status=design-notified). 실행: branch-handoff.sh notify plan --summary "계획 요약"
```

## 인자 검증

- `notify start/design/plan` 시 `--summary` 필수. 빈 summary는 에러 반환.
- 각 notify 시 현재 status가 올바른 선행 상태인지 검증. 위반 시 현재 상태와 필요 명령 안내.

## notify merge 정리

- active 파일 삭제 (기존 동작)
- **watermark 파일도 삭제** — 다음 세션에서 stale watermark로 인한 오탐 방지
- backlog-status 파일 삭제 (기존 동작)

## stale active 정리

기존 cleanup은 PID dead + timeout 조건으로 정리.
추가: active 파일에 `session_pid` 필드 저장 → cleanup 시 session PID 생존 여부도 확인.

## 변경 파일

| 파일 | 변경 유형 | 내용 |
|------|-----------|------|
| `apex_tools/branch-handoff.sh` | 수정 | notify plan 추가, --skip-design, status 상태 머신 검증 |
| `.claude/hooks/handoff-probe.sh` | 수정 | active 등록 + status 체크 → 전면 차단 로직 |
| `.claude/hooks/validate-handoff.sh` | 수정 | git commit 시 active 등록 확인 차단 |
| `apex_tools/session-context.sh` | 수정 | feature/bugfix 브랜치 미등록 경고 |

## active 파일 형식

```yaml
branch: branch_03
pid: 12345
backlog: 109
status: implementing
scopes:
  - tools
summary: "벤치마크 큐잉 강제"
started_at: "2026-03-21 14:30:00"
updated_at: "2026-03-21 15:00:00"
latest_tier2_id: 3
```
