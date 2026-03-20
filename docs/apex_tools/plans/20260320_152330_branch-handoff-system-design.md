# Branch Handoff System — 설계 스펙

브랜치별 중앙 인수인계 시스템. 물리적으로 격리된 워크스페이스에서 병렬 작업하는 에이전트들이 파일 기반으로 소통하여 충돌·중복 작업·재작업을 사전에 방지한다.

---

## 1. 문제 정의

- 3개 이상의 물리적 워크스페이스(`branch_01~N`)에서 독립 에이전트가 병렬 작업
- 한 에이전트의 변경이 다른 에이전트의 작업에 영향을 주는 경우:
  - 공유 헤더/인터페이스 변경 → 컴파일 실패
  - 같은 파일 동시 수정 → 머지 충돌
  - API 시그니처·설계 전제 변경 → 전면 재작성
- 현재 에이전트 간 소통 수단 없음 → 머지 시점에서야 문제 발견 → 재작업

## 2. 목표

- 에이전트 간 실시간(폴링 기반) 소통 채널 구축
- 작업 충돌을 **코드 작성 전**에 사전 감지·회피
- 에이전트가 자율적으로 판단·대응 (유저 개입 불필요)
- 10개 이상 브랜치 동시 운용 지원

## 3. 아키텍처

### 3.1 접근법: 파일 기반 중앙 허브

`queue-lock.sh`와 동일한 패턴. `%LOCALAPPDATA%/apex-branch-handoff/`에 중앙 디렉토리 운영.

선택 근거:
- 기존 인프라 패턴과 일관성 (학습 비용 0)
- 파일 시스템 기반 = 추가 의존성 없음
- 각 브랜치가 자기 파일만 쓰므로 락 불필요 (공유 파일은 `mkdir` 락으로 보호)
- 로컬 머신 한정이지만, 현재 모든 에이전트가 같은 머신에서 실행

### 3.2 디렉토리 구조

```
%LOCALAPPDATA%/apex-branch-handoff/
├── index                        ← append-only, 한 줄 = 1 알림 (mkdir 락으로 보호)
├── index.lock/                  ← index 쓰기 보호용 mkdir 락
├── watermarks/
│   └── {branch_id}              ← 파일 내용: 숫자 1개 (마지막 확인 ID)
├── payloads/
│   └── {id}.md                  ← Tier 1, 2 상세 (Tier 3은 없음)
├── active/
│   └── {branch_id}.yml          ← 현재 작업 등록 정보 (PID + 타임스탬프 포함)
├── responses/
│   └── {notification_id}/
│       └── {branch_id}.yml      ← 수신 에이전트의 대응 선언
├── backlog-status/
│   └── {branch_id}.yml          ← 해당 브랜치가 작업 중인 백로그 항목
└── archive/                     ← compaction 시 이동되는 구 index/payload
```

### 3.3 데이터 흐름

```
Branch_03 (발신)                    중앙 허브                         Branch_01 (수신)
                                 (LOCALAPPDATA)

[백로그 착수]
  │
  ├─ Tier 1 ──► active/branch_03.yml 생성          ◄── 폴링 ── [체크포인트]
  │             backlog-status/branch_03.yml 생성          │
  │             index에 1줄 append (mkdir 락)              ├─ watermark 비교
  │                                                        ├─ 새 항목 발견
  ▼                                                        ├─ 스코프 필터링
[설계 확정]                                                 └─ 관련 있으면 payload 읽기
  │                                                           → 자체 판단 & 대응
  ├─ Tier 2 ──► payloads/{id}.md 생성                         → ack (대응 선언 포함)
  │             index에 1줄 append (mkdir 락)
  │             active/branch_03.yml 갱신
  ▼
  │  (방향 변경 시 Tier 2 재발행 가능 — 최신 것이 supersede)
  ▼
[머지 완료]
  │
  ├─ Tier 3 ──► index에 1줄 append (mkdir 락)
  │             active/branch_03.yml 삭제
  │             backlog-status/branch_03.yml 삭제
  └─            (페이로드 없음 — 리모트 커밋이 source of truth)
```

## 4. 3-Tier 알림 체계

### Tier 1 — 사전 알림 (작업 착수)

- **시점**: 명확한 작업 목적(백로그 이슈 등)으로 브랜치 생성 후
- **내용**: 대략적 작업 범위, 영향 스코프
- **백로그 연동**: `backlog-status/{branch_id}.yml`에 항목 등록 → 타 에이전트의 중복 착수 방지
- **발행 조건**: 명확한 작업 목적이 있는 경우에만. 탐색적 작업은 Tier 1 없이 Tier 2로 직행

### Tier 2 — 중간 알림 (설계/계획 확정)

- **시점**: 브레인스토밍 → 설계 문서 또는 구현 계획이 확정된 시점
- **내용**: 구체적 작업 계획, 변경 파일, 영향 범위, 타 브랜치 영향 분석
- **재발행 가능**: 유저 개입·리뷰에 의해 방향이 변경되면 언제든 재발행. 같은 브랜치의 이전 Tier 2를 자동 supersede
- **페이로드 포함**: `payloads/{id}.md`에 상세 내용 저장

### Tier 3 — 사후 알림 (머지 완료)

- **시점**: CI 전체 통과 후 머지할 때
- **내용**: 요약만 (index 한 줄). 상세는 리모트 커밋으로 확인 가능
- **정리**: `active/` 파일 삭제, `backlog-status/` 해당 파일 삭제

## 5. 데이터 포맷

### 5.1 index (append-only, mkdir 락으로 보호)

```
{id}|{tier}|{branch_id}|{scopes}|{summary}
```

예시:
```
1|tier1|branch_03|core,shared|BACKLOG-89 core->shared 역방향 의존 해소 착수
2|tier1|branch_01|ci|BACKLOG-99 Nightly Valgrind 결과 확인
3|tier2|branch_03|core,shared,gateway|ErrorCode 서비스별 분리 설계 확정
4|tier2|branch_03|core,shared,gateway|[SUPERSEDE:3] ErrorCode 분리 방향 변경 — unified error registry
5|tier3|branch_01|ci|BACKLOG-99 머지 완료
```

- ID: auto-increment — `mkdir index.lock` 획득 후 마지막 줄에서 추출 + 1, 쓰기, `rmdir index.lock` 해제
- Tier 2 재발행 시 summary에 `[SUPERSEDE:{이전ID}]` 태그
- 스코프 태그: `core|shared|gateway|auth-svc|chat-svc|infra|ci|e2e|docs|tools`

### 5.2 active/{branch_id}.yml

```yaml
branch: branch_03
pid: 12345
backlog: 89                    # 없으면 생략
status: designing              # started → designing → implementing → merging
scopes:
  - core
  - shared
summary: "core->shared 역방향 의존 해소"
latest_tier2_id: 4             # 최신 Tier 2 payload ID (없으면 생략)
started_at: "2026-03-20 14:30:00"
updated_at: "2026-03-20 16:45:00"
```

status 흐름: `started` → `designing` → `implementing` → `merging` → (파일 삭제)

### 5.3 backlog-status/{branch_id}.yml

```yaml
backlog: 89
branch: branch_03
started_at: "2026-03-20 14:30:00"
```

각 브랜치가 자기 파일만 쓴다. 백로그 잠금 확인 시 디렉토리 내 전체 파일을 스캔하여 해당 backlog ID를 찾는다.

### 5.4 payloads/{id}.md

```markdown
---
id: 4
tier: tier2
branch: branch_03
scopes: [core, shared, gateway]
supersedes: 3
timestamp: "2026-03-20 16:45:00"
---

## ErrorCode 서비스별 분리 — 방향 변경

### 변경 요약
unified error registry 방식으로 전환.

### 영향 범위
- `apex_core/include/core/error_code.hpp` — ErrorCode enum 축소
- `apex_shared/include/shared/error_registry.hpp` — 신규
- Gateway, Auth 서비스의 에러 핸들링 변경

### 타 브랜치 영향
- core/shared 스코프 작업 중인 브랜치: 에러 코드 참조 방식 변경 필요
- gateway 스코프: import 경로 변경
```

### 5.5 responses/{notification_id}/{branch_id}.yml

```yaml
notification_id: 4
branch: branch_01
action: design-adjusted
reason: "에러 핸들링 설계 재검토 완료"
timestamp: "2026-03-20 17:00:00"
```

### 5.6 watermarks/{branch_id}

파일 내용: 숫자 1개 (마지막으로 확인한 notification ID).

## 6. CLI 인터페이스

스크립트: `apex_tools/branch-handoff.sh`

### 6.1 알림 발행

```bash
# Tier 1: 작업 시작 등록
branch-handoff.sh notify start \
  --backlog 89 \
  --scopes core,shared \
  --summary "core->shared 역방향 의존 해소"

# Tier 2: 설계/계획 확정 알림
branch-handoff.sh notify design \
  --scopes core,shared,gateway \
  --summary "ErrorCode 서비스별 분리" \
  --payload-file /path/to/design-summary.md

# Tier 2 재발행: 같은 브랜치의 이전 Tier 2를 자동 SUPERSEDE
branch-handoff.sh notify design \
  --scopes core,shared,gateway \
  --summary "ErrorCode 분리 방향 변경" \
  --payload-file /path/to/updated-summary.md

# Tier 3: 머지 완료 알림 (커밋 해시 자동 포함)
branch-handoff.sh notify merge \
  --summary "BACKLOG-89 머지 완료"
```

`notify merge`의 스코프는 `active/{branch_id}.yml`에서 자동 추출. summary에 머지 커밋 해시를 자동 append하여 수신 에이전트가 `git log`에서 즉시 찾을 수 있게 한다 (예: `BACKLOG-89 머지 완료 [abc1234]`).

### 6.2 알림 수신 (폴링)

```bash
# 새 알림 확인 (워터마크 기반)
branch-handoff.sh check
branch-handoff.sh check --scopes core,shared   # 스코프 필터링

# 게이트 체크 (미응답 시 차단)
branch-handoff.sh check --gate design
branch-handoff.sh check --gate plan
branch-handoff.sh check --gate implement
branch-handoff.sh check --gate build
branch-handoff.sh check --gate merge

# 특정 payload 읽기
branch-handoff.sh read 12

# 대응 선언 (ack)
branch-handoff.sh ack --id 4 --action no-impact --reason "영향 없음"
branch-handoff.sh ack --id 4 --action will-rebase --reason "참조 3건 변경 필요"
branch-handoff.sh ack --id 4 --action rebased --reason "rebase 완료"
branch-handoff.sh ack --id 4 --action design-adjusted --reason "설계 재검토 완료"
branch-handoff.sh ack --id 4 --action deferred --reason "머지 전 처리 예정"
```

### 6.3 상태 조회

```bash
# 전체 현황
branch-handoff.sh status

# 백로그 잠금 확인
branch-handoff.sh backlog-check 89
# → "BACKLOG-89: FIXING by branch_03 (since 2026-03-20 14:30:00)" (exit 1)
# → "BACKLOG-89: AVAILABLE" (exit 0)

# 고아 엔트리 정리
branch-handoff.sh cleanup
```

### 6.4 초기화

```bash
branch-handoff.sh init
```

모든 명령은 실행 전 `init`을 자동 호출 (auto-init). 생성 대상:
- `index` (빈 파일), `watermarks/`, `payloads/`, `active/`, `responses/`, `backlog-status/`, `archive/`

## 7. 동시성 & 안전성

### 7.1 index 쓰기 — mkdir 락

Windows/MSYS2에서 `O_APPEND`는 원자성을 보장하지 않으므로, `queue-lock.sh`의 `mkdir` 패턴을 사용:

```bash
# index에 한 줄 추가하는 원자적 흐름
lock_index() {
    while ! mkdir "$HANDOFF_DIR/index.lock" 2>/dev/null; do
        # stale 락 감지: PID 파일 확인
        local lock_pid_file="$HANDOFF_DIR/index.lock/owner"
        if [[ -f "$lock_pid_file" ]]; then
            local lock_pid lock_time
            lock_pid=$(grep '^PID=' "$lock_pid_file" | cut -d= -f2)
            lock_time=$(grep '^ACQUIRED=' "$lock_pid_file" | cut -d= -f2)
            if ! check_pid_alive "$lock_pid"; then
                echo "[handoff] stale index lock (PID $lock_pid dead), breaking"
                rm -rf "$HANDOFF_DIR/index.lock"
                continue
            fi
            local now elapsed
            now=$(date +%s)
            elapsed=$((now - lock_time))
            if [[ $elapsed -ge 30 ]]; then
                echo "[handoff] stale index lock (${elapsed}s), breaking"
                rm -rf "$HANDOFF_DIR/index.lock"
                continue
            fi
        fi
        sleep 0.1
    done
    # 락 획득 후 PID 파일 생성
    echo "PID=$$" > "$HANDOFF_DIR/index.lock/owner"
    echo "ACQUIRED=$(date +%s)" >> "$HANDOFF_DIR/index.lock/owner"
}

unlock_index() {
    rm -rf "$HANDOFF_DIR/index.lock" 2>/dev/null
}

append_index() {
    lock_index
    local last_id
    last_id=$(tail -1 "$HANDOFF_DIR/index" 2>/dev/null | cut -d'|' -f1)
    last_id=${last_id:-0}
    local new_id=$((last_id + 1))
    echo "${new_id}|${tier}|${branch_id}|${scopes}|${summary}" >> "$HANDOFF_DIR/index"
    unlock_index
    echo "$new_id"
}
```

### 7.2 backlog-status — 에이전트별 파일 분리

공유 YAML 파일의 동시 read-modify-write race를 제거하기 위해, `backlog-status/`를 에이전트별 파일로 분리:

- 각 브랜치가 `backlog-status/{branch_id}.yml`만 생성/삭제
- 백로그 잠금 확인: `backlog-status/` 디렉토리 스캔 → 모든 파일에서 해당 backlog ID 검색
- 동시 수정 불가 — 각 파일의 소유자는 해당 브랜치뿐

### 7.3 active 엔트리 staleness 감지

`active/{branch_id}.yml`에 `pid` 필드를 포함. `cleanup` 명령 또는 `check` 실행 시:

1. PID가 살아있는지 확인 (`kill -0` + Windows `tasklist` fallback)
2. `updated_at`이 설정 가능한 타임아웃(기본 24시간)을 초과했는지 확인
3. 둘 다 해당하면 stale로 판단 → 경고 출력 + 선택적 정리

### 7.4 고아 정리 (`cleanup` 명령)

- 존재하지 않는 브랜치의 `watermarks/`, `active/`, `backlog-status/`, `responses/` 파일 제거
  - 브랜치 존재 여부 판단: 워크스페이스 디렉토리 존재 여부 (`/d/.workspace/apex_pipeline_{branch_id}/`) 기준. PID 활성 여부는 보조 확인
- 모든 watermark가 지나간 index 줄 → `archive/`로 이동 (compaction)
- 참조되지 않는 `payloads/` 파일 → `archive/`로 이동

## 8. 게이트 시스템

### 8.1 체크포인트

모든 게이트는 동일한 동작 — 내 스코프와 겹치는 미응답 알림이 있으면 **차단**.

| 시점 | 게이트 이름 | 트리거 |
|------|------------|--------|
| 설계 문서 완료 | `design` | 설계 확정 전 최신 상태 동기화 |
| 구현 계획 완료 | `plan` | 구현 착수 전 마지막 반영 기회 |
| 구현 시작 직전 | `implement` | 코드 작성 직전 최종 확인 — 계획 확정 후 도착한 알림 포착 |
| 빌드 전 | `build` | 공유 헤더/인터페이스 변경 반영 |
| 머지 전 | `merge` | 최종 동기화 |

### 8.2 SUPERSEDE 처리 규칙

- 알림 B가 알림 A를 supersede하면, A는 **모든 에이전트에게 자동 dismiss** (ack 불필요)
- B는 새 알림으로 취급 — 이전에 A를 ack한 에이전트도 B를 별도 ack해야 함
- 같은 브랜치의 가장 최신 Tier 2만 유효. 이전 Tier 2는 전부 auto-dismiss
- 게이트 체크 시 supersede된 알림은 미응답 목록에서 제외

### 8.3 게이트 출력 형식

```
[GATE:design] BLOCKED — 2 unacked notifications matching scopes [core, shared]

  #4 tier2 branch_03 core,shared,gateway
     "ErrorCode 서비스별 분리 — unified error registry 방식"
     payload: payloads/4.md

  #5 tier3 branch_01 ci,core
     "BACKLOG-99 Nightly Valgrind fix 머지 완료"
     (no payload — check remote commits)

Resolve all before proceeding.
```

에이전트는 이 출력만으로 자율적으로 판단·대응 가능:
- payload 읽기 → 영향 분석 → 필요 시 설계/계획 수정 → ack → 재확인

### 8.4 클리어 시 출력

```
[GATE:design] CLEAR — no pending notifications.
```

### 8.5 자동 작업 시 행동

유저 개입 없는 완전 자동 작업에서 게이트 차단 시:
1. 에이전트가 차단 출력 확인
2. 해당 payload 읽기
3. 내 작업에 대한 영향 자체 판단
4. 필요 시 설계/계획 수정
5. ack (대응 선언 포함)
6. `check --gate` 재실행 → 클리어 확인
7. 작업 재개

## 9. 에이전트 행동 규약

### 9.1 수신 시 판단 흐름

```
새 알림 수신 (check 결과)
  │
  ├─ 내 스코프와 무관 → ack --action no-impact
  │
  ├─ 스코프 겹침 → payload 읽기
  │     │
  │     ├─ Tier 1 (착수 알림)
  │     │    → 같은 백로그를 잡으려 했으면 → 회피
  │     │    → 같은 파일을 건드릴 가능성 → 인지하고 계속
  │     │
  │     ├─ Tier 2 (설계/계획 알림)
  │     │    → 내 코드에 영향 있으면 → rebase 또는 설계 조정
  │     │    → 영향 없으면 → ack --action no-impact
  │     │
  │     └─ Tier 3 (머지 알림)
  │          → main에 반영됨 → 다음 rebase 때 반영
  │          → 급한 영향이면 → 즉시 fetch & rebase
  │
  └─ ack (대응 선언 포함)
```

### 9.2 ack action 종류

| action | 의미 |
|--------|------|
| `no-impact` | 확인했고, 내 작업에 영향 없음 |
| `will-rebase` | 다음 rebase 시 반영 예정 |
| `rebased` | 이미 rebase 완료 |
| `design-adjusted` | 설계/계획 수정 완료 |
| `deferred` | 인지했고, 머지 전에 처리 예정 |

## 10. 기존 워크플로우 통합

### 10.1 워크플로우 매핑

```
[기존 흐름]                          [handoff 연동]

백로그 착수                          → notify start
  ↓
브레인스토밍 → 설계 확정              → check --gate design → notify design
  ↓
구현 계획 작성                        → check --gate plan
  ↓
구현 시작                             → check --gate implement
  ↓
구현 → 빌드                          → check --gate build
  ↓
리뷰 → (방향 변경 시)                → notify design (재발행)
  ↓
CI 통과 → 머지                       → check --gate merge
                                     → queue-lock.sh merge acquire
                                     → notify merge
                                     → queue-lock.sh merge release
```

### 10.2 CLAUDE.md 규칙 추가

루트 CLAUDE.md에 handoff 규칙을 추가하여 에이전트가 규약을 따르도록 강제.

### 10.3 향후 hook 자동화 (초기 버전에서는 미구현)

안정화 후 확장 가능:
- `PreToolUse(Bash)` — 빌드 명령 감지 시 자동 `check --gate build`
- 머지 워크플로우 내 `notify merge` 자동 삽입

## 11. 핵심 원칙

- **각 브랜치는 자기 파일만 쓴다** — `watermarks/{자기ID}`, `active/{자기ID}.yml`, `responses/{id}/{자기ID}.yml`, `backlog-status/{자기ID}.yml`
- **공유 리소스(index)는 mkdir 락으로 보호** — Windows/MSYS2에서도 원자성 보장
- **페이로드는 immutable** — 한번 쓰면 수정 안 함. 변경 시 새 ID로 재발행 (SUPERSEDE)
- **SUPERSEDE된 알림은 자동 dismiss** — 같은 브랜치의 최신 Tier 2만 유효
- **stale 감지** — PID + 타임스탬프 기반으로 고아 엔트리 자동 정리
- **토큰 효율**: 워터마크(숫자 1개) → index tail(새 줄만) → 스코프 필터 → 관련 payload만 읽기. 일반 케이스 토큰 소비 ≈ 0
