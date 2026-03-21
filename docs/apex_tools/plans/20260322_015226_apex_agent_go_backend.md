# apex-agent: Hook 시스템 Go 백엔드 재작성 설계 논의

- **상태**: 논의 단계 (미착수)
- **백로그**: BACKLOG-126
- **스코프**: tools, infra
- **작성일**: 2026-03-22

---

## 1. 배경 및 동기

### 현재 시스템 현황

Claude Code hook 시스템이 **11개 bash 스크립트** (총 ~2,080줄)로 구성되어 있으며,
에이전트 워크플로우의 핵심 게이트(빌드 직접 호출 차단, 머지 잠금, 핸드오프 상태 기반 편집 제한, 자동 리베이스 등)를 강제한다.

| 구분 | 스크립트 수 | 총 라인 | 복잡도 |
|------|:---------:|:------:|:-----:|
| PreToolUse hooks (`.claude/hooks/`) | 5개 | ~350줄 | 낮음~중간 |
| 자동화 도구 (`apex_tools/`) | 6개 | ~1,730줄 | 낮음~매우높음 |

### 복잡도 집중

전체 복잡도의 대부분이 **2개 스크립트에 집중**:

| 스크립트 | 줄 수 | 함수 | 조건 분기 | 역할 |
|----------|:-----:|:----:|:---------:|------|
| `branch-handoff.sh` | 705 | 21 | 60+ | 브랜치 간 알림 상태머신 + 인덱스 잠금 + YAML 파싱 |
| `queue-lock.sh` | 345 | 18 | 40+ | FIFO 큐 + PID 기반 stale 감지 + 빌드/머지 채널 잠금 |
| `cleanup-branches.sh` | 374 | 2 | 35+ | 3-layer 머지 감지 + 워크트리/로컬/리모트 정리 |

### 리뷰 피드백

- bash 스크립트의 **구조적 복잡성**이 유지보수 부담
- 스크립트 수가 계속 늘어나는 추세에 대한 우려
- Go 또는 Python 기반 재작성 제안 수신

### 기존 bash 구현의 구체적 문제점

1. **MSYS 경로 버그 반복 발생** — #89, #90이 모두 MSYS 경로 변환 오류 수정
2. **YAML 파싱이 fragile** — `grep+sed+awk` 조합으로 미니 파서 구현, 복잡한 값 처리 불가
3. **상태머신 디버깅 난이도** — bash에서 60+ 조건 분기 추적이 어려움
4. **에러 핸들링 한계** — exit code + stderr 문자열 기반, 구조화된 에러 타입 없음
5. **테스트 불가** — unit test 인프라 부재, 통합 테스트도 사실상 불가능
6. **공통 패턴 중복** — Windows 경로 감지, PID 체크, jq 폴백, mkdir 잠금 등 동일 코드가 여러 스크립트에 산재

---

## 2. 현재 아키텍처 분석

### 실행 흐름

```
Claude Code Hook (settings.json)
    │
    ▼  매번 새 프로세스 spawn
  bash script (콜드 스타트 ~50ms)
    │
    ▼  매번 파일 읽기/파싱
  파일 시스템 (index, YAML, mkdir lock)
```

### Hook 구성 (settings.json)

```
PreToolUse [Bash]
  ├─ validate-build.sh    (5s timeout) — 빌드 도구 직접 호출 차단
  ├─ validate-merge.sh    (5s timeout) — 머지 잠금 미획득 차단
  ├─ validate-handoff.sh  (10s timeout) — 핸드오프 미등록/상태 기반 편집 차단
  └─ enforce-rebase.sh    (30s timeout) — 자동 리베이스

PreToolUse [Edit|Write]
  └─ handoff-probe.sh     (5s timeout) — 핸드오프 알림 감지 + 편집 게이트

SessionStart
  ├─ setup-claude-plugin.sh (10s timeout) — 플러그인 등록
  └─ session-context.sh     (10s timeout) — 프로젝트 컨텍스트 주입
```

### 스크립트 간 의존 관계

```
session-context.sh ──calls──→ branch-handoff.sh check
handoff-probe.sh   ──calls──→ branch-handoff.sh check --gate merge
validate-handoff.sh──calls──→ branch-handoff.sh check
validate-merge.sh  ──reads──→ queue-lock.sh가 생성한 merge.lock
```

### 공유 패턴 (중복 코드)

| 패턴 | 사용처 | 내용 |
|------|--------|------|
| Windows 경로 감지 | 6개 스크립트 | `LOCALAPPDATA` vs `XDG_DATA_HOME` 분기 |
| PID 생존 체크 | 2개 스크립트 | `kill -0` + `tasklist` 폴백 |
| jq / grep+sed JSON 폴백 | 4개 스크립트 | JSON 파싱 이중 경로 |
| mkdir 원자적 잠금 | 2개 스크립트 | `mkdir` test-and-set + stale 감지 |
| YAML 미니파서 | 2개 스크립트 | `grep+sed+awk` 조합 |

### 파일 시스템 기반 상태 저장

**queue-lock.sh 데이터**:
```
$QUEUE_DIR/
├── build-queue/     # FIFO: ${timestamp}_${branch}_${PID}
├── merge-queue/
├── build.lock/      # 디렉토리 존재 = 잠금 보유
├── merge.lock/
├── build.owner      # PID, BRANCH, ACQUIRED, STATUS
├── merge.owner
└── logs/            # ${branch}.log
```

**branch-handoff.sh 데이터**:
```
$HANDOFF_DIR/
├── index              # 파이프 구분 append-only 로그
├── index.lock/        # 디렉토리 잠금
├── active/            # 브랜치별 YAML: ${BRANCH_ID}.yml
├── payloads/          # 설계 문서: ${id}.md
├── responses/         # 알림별 ACK: ${id}/${BRANCH_ID}.yml
├── backlog-status/    # 백로그→브랜치 링크
├── watermarks/        # 마지막 확인 알림 ID
└── archive/           # 압축된 이전 항목
```

### 외부 명령 의존성

| 명령 | 호출 횟수 | 필수 여부 |
|------|:---------:|:---------:|
| git | 41 | 필수 |
| sed | 35 | 필수 |
| jq | 16 | 선택 (grep+sed 폴백) |
| gh | 15 | 선택 (blob hash 폴백) |
| mkdir | 13 | 필수 |
| awk | 12 | 필수 |
| date | 11 | 필수 |
| tasklist | 2 | Windows 전용 폴백 |

---

## 3. 제안 아키텍처: apex-agent

### 비전

단순한 "bash → Go 포팅"이 아니라, **에이전트 자동화를 위한 통합 백엔드 시스템**.
C++ 코어가 런타임 인프라라면, apex-agent는 **개발 인프라**.

```
┌─────────────────────┐    ┌──────────────────────┐
│   apex_core (C++)   │    │  apex-agent (Go)     │
│                     │    │                      │
│  서버 프레임워크     │    │  에이전트 자동화      │
│  코루틴 런타임      │    │  워크플로우 엔진      │
│  네트워크 I/O       │    │  상태머신 + 잠금      │
│  서비스 오케스트라   │    │  hook 게이트 + IPC    │
│                     │    │                      │
│  "런타임 인프라"     │    │  "개발 인프라"        │
└─────────────────────┘    └──────────────────────┘
```

### 실행 모델

```
Claude Code Hook (settings.json)
    │
    ▼  CLI 호출 (콜드 ~10ms) 또는 소켓 IPC (웜 ~1ms)
  apex-agent CLI  ← 단일 바이너리
    │
    ├─ handoff   (notify / check / ack / status / cleanup)
    ├─ queue     (build / merge / acquire / release)
    ├─ hook      (validate-merge / validate-build / validate-handoff / ...)
    ├─ cleanup   (branches / stale-locks)
    ├─ context   (session-context 주입)
    └─ daemon    (상주 프로세스 — 선택적 확장)
         │
         ▼
    SQLite / bbolt  ← 파일 시스템 기반 상태 대체
```

### 현재 대비 개선 사항

| 영역 | 현재 (Bash) | apex-agent (Go) |
|------|------------|-----------------|
| 상태 관리 | 파일 + mkdir lock + grep 파싱 | SQLite 트랜잭션 (ACID) |
| 잠금 | `mkdir` 원자성 트릭 | `flock` / DB 트랜잭션 / 프로세스 내 mutex |
| hook 응답 시간 | ~50ms (bash spawn) + 파일 I/O | ~10ms (CLI) 또는 ~1ms (데몬 소켓) |
| 스크립트 간 상태 공유 | 파일 읽기 반복 | 단일 프로세스 내 메모리 or 공유 DB |
| 에러 핸들링 | exit code + stderr 문자열 | 구조화된 에러 타입 + 구조화 로깅 |
| 테스트 | 거의 불가능 | unit test + integration test |
| 확장성 | 새 스크립트 + settings.json 수정 | 서브커맨드 추가 = 함수 추가 |
| YAML 파싱 | grep+sed (fragile) | `gopkg.in/yaml.v3` (정확) |
| JSON 파싱 | jq 또는 grep+sed 폴백 | `encoding/json` (단일 경로) |
| 경로 처리 | MSYS 수동 정규식 (버그 다발) | `filepath.Abs` (네이티브) |
| PID 체크 | `kill -0` + `tasklist` 폴백 | `os.FindProcess` (크로스플랫폼) |
| 중복 코드 | 6개 스크립트에 동일 패턴 산재 | 공통 패키지로 통합 |

### 데몬 모드 — 선택적 확장

```
apex-agent daemon start    ← SessionStart hook에서 실행
apex-agent daemon stop     ← SessionEnd 또는 자동 타임아웃

# hook에서는 소켓으로 질의 (spawn 비용 0)
apex-agent hook validate-merge --input '{"command":"gh pr merge"}'
```

데몬이 상주하면 열리는 가능성:
- **이벤트 기반 반응** — 파일 시스템 워치로 핸드오프 변경 감지, 능동적 알림
- **인메모리 캐시** — 반복되는 git 호출 결과 캐싱
- **웹 대시보드** — Go 내장 `net/http`로 핸드오프/큐 상태 시각화
- **워크스페이스 간 IPC** — 파일 기반 폴링 → 소켓/named pipe로 실시간 동기화

### 제안 Go 프로젝트 구조 (초안)

```
apex_tools/apex-agent/
├── cmd/
│   └── apex-agent/
│       └── main.go           # CLI 엔트리포인트 (cobra)
├── internal/
│   ├── handoff/              # branch-handoff 로직
│   │   ├── state.go          # 상태머신
│   │   ├── index.go          # 인덱스 관리
│   │   ├── notify.go         # 알림 시스템
│   │   └── cleanup.go        # 정리
│   ├── queue/                # queue-lock 로직
│   │   ├── lock.go           # 잠금 관리
│   │   ├── fifo.go           # FIFO 큐
│   │   └── build.go          # 빌드 실행
│   ├── hook/                 # hook 게이트 로직
│   │   ├── validate.go       # validate-build, validate-merge
│   │   ├── handoff_gate.go   # validate-handoff, handoff-probe
│   │   └── rebase.go         # enforce-rebase
│   ├── cleanup/              # cleanup-branches 로직
│   ├── platform/             # 크로스플랫폼 추상화
│   │   ├── path.go           # 경로 처리 (MSYS 해소)
│   │   ├── pid.go            # PID 체크
│   │   └── env.go            # 환경 변수
│   └── store/                # 상태 저장소
│       ├── sqlite.go         # SQLite 백엔드
│       └── migrate.go        # 파일→DB 마이그레이션
├── go.mod
├── go.sum
├── Makefile                  # 크로스컴파일 타겟
└── README.md
```

---

## 4. 언어 선택 분석

### 필수 요구사항

1. **콜드 스타트 ≤ 20ms** — 5초 hook 타임아웃에서 충분한 여유
2. **단일 바이너리 배포** — 런타임 설치 요구 없음
3. **크로스 플랫폼** — Windows (MSYS/네이티브) + Linux (CI)
4. **임베디드 DB 지원** — SQLite 또는 동등
5. **충분한 생태계** — CLI, JSON, YAML, git 연동

### 후보 비교

| | Go | Rust | Deno (TS) | Python | Zig |
|---|:---:|:---:|:---:|:---:|:---:|
| 콜드 스타트 | ~10ms | ~5ms | ~30-50ms | ~300-500ms | ~3ms |
| 단일 바이너리 | O | O | O (`deno compile`) | X | O |
| 크로스컴파일 | `GOOS=windows` | `--target` | `--target` | N/A | `--target` |
| 바이너리 크기 | ~10-15MB | ~5-8MB | ~50-80MB | N/A | ~2-5MB |
| SQLite (CGo-free) | `modernc.org/sqlite` | `rusqlite` | `deno.land/x/sqlite` | `sqlite3` (내장) | 미성숙 |
| CLI 프레임워크 | `cobra`, `urfave/cli` | `clap` | `cliffy`, `yargs` | `click`, `typer` | 미성숙 |
| 학습 곡선 | 낮음 | **높음** | 낮음 | 가장 낮음 | 중간 |
| 동시성 모델 | goroutine + channel | async/await + tokio | async/await | asyncio (GIL) | — |
| 데몬 구현 | `net/http` 내장 | actix/axum | `Deno.serve` 내장 | uvicorn/flask | 수동 |
| 이 도메인 레퍼런스 | Docker, K8s, Terraform | — | — | — | — |

### 평가 요약

**Go (1순위 추천)** — 가장 균형 잡힌 선택. 빠른 시작, 단일 바이너리, 낮은 학습 곡선, 데몬 모드까지 자연스러움. 이 도메인(CI/CD 오케스트레이션, DevOps 도구)의 사실상 표준 언어.

**Deno/TypeScript (2순위)** — Claude Code 자체가 TypeScript이고, `deno compile`로 단일 바이너리 가능. 바이너리가 50MB+ 로 무거움. 콜드 스타트가 Go보다 느림. 팀이 TS에 익숙하다면 고려 가능.

**Rust** — 성능은 최고지만 이 시스템은 CPU-bound가 아니라 I/O-bound. 소유권 모델의 이점이 크지 않고 학습 비용 대비 ROI가 낮음.

**Python** — 데몬 모드 전제라면 후보가 되지만 단일 바이너리 배포가 현실적으로 어렵고, 데몬 없이 쓸 때 hook 타임아웃 위험. 탈락.

**Zig** — 생태계 미성숙 (CLI 프레임워크, YAML 파서 부족). 탈락.

---

## 5. 마이그레이션 전략 (초안)

### 단계별 접근

| Phase | 내용 | 위험도 | 비고 |
|:-----:|------|:------:|------|
| **0** | Go 프로젝트 스캐폴딩 + CI 빌드 파이프라인 | 낮음 | `apex_tools/apex-agent/` |
| **1** | 5개 단순 hook → Go 서브커맨드 이식 | 낮음 | validate-build, validate-merge, enforce-rebase, handoff-probe, validate-handoff |
| **2** | `branch-handoff.sh` → Go 이식 (핵심) | **높음** | 상태머신 + 인덱스 잠금, 가장 복잡 |
| **3** | `queue-lock.sh` → Go 이식 | 중간 | FIFO 큐 + PID 관리 |
| **4** | 나머지 유틸리티 이식 | 낮음 | cleanup-branches, session-context |
| **5** | 상태 저장소 SQLite 전환 | 중간 | 파일→DB 마이그레이션 도구 포함 |
| **6** | 데몬 모드 + IPC (선택적 확장) | 중간 | Phase 1-5 안정화 후 |

### 병행 운영 전략

- 각 Phase에서 **bash와 Go를 동시에 유지**하고 `settings.json`의 command를 전환하는 방식
- Go 이식 완료 + 안정화 확인 후 해당 bash 스크립트 삭제
- 파일 시스템 기반 데이터 포맷은 Phase 5 전까지 유지 (하위 호환)

### Go 도입 전제조건

1. Go 툴체인 + Makefile 추가 (`apex_tools/apex-agent/`)
2. CI에 Go 빌드 스텝 추가 (`.github/workflows/`)
3. 바이너리 배포 전략 결정:
   - **옵션 A**: CI에서 크로스컴파일 → GitHub Release artifact
   - **옵션 B**: 레포에 바이너리 커밋 (간단하지만 바이너리 크기 부담)
   - **옵션 C**: 각 환경에서 `go build` 실행 (Go 툴체인 필요)

---

## 6. 미해결 논의 사항

착수 전 결정 필요:

1. **언어 최종 확정** — Go vs Deno(TS). 현재 Go 추천이지만 팀 역량/선호도 확인 필요
2. **바이너리 배포 전략** — Release artifact vs 레포 커밋 vs 로컬 빌드
3. **데몬 모드 범위** — Phase 6에서 어디까지 구현할 것인지 (소켓 IPC? 웹 대시보드? 파일 워치?)
4. **하위 호환** — 기존 handoff/queue 파일 포맷에서 SQLite 전환 시 마이그레이션 전략 상세
5. **스코프** — 전면 재작성 vs 하이브리드 (단순 hook은 bash 유지). 리뷰어 의견 재확인
6. **#50과의 관계** — BACKLOG #50 "apex_tools/scripts 폴더 신설 + 스크립트 정리"가 이 작업으로 대체되는지 여부

---

## 7. 참조

- 현재 hook 설정: `.claude/settings.json`
- hook 스크립트: `.claude/hooks/`
- 자동화 도구: `apex_tools/`
- 관련 버그 수정 이력: #89 (MSYS 경로), #90 (MSYS 경로)
