# Apex Pipeline — 프로젝트 가이드

## 개요

C++23 코루틴 기반 고성능 서버 프레임워크 모노레포.
자체 네트워크 프레임워크 + MSA 아키텍처 (Gateway → Kafka → Services → Redis/PostgreSQL).

## 모노레포 구조

```
D:\.workspace/
├── CMakeLists.txt / CMakePresets.json  ← 모노레포 루트 빌드
├── build.bat / build.sh               ← 루트 빌드 스크립트
├── apex_core/          ← 코어 프레임워크 (C++23, Boost.Asio 코루틴)
│   ├── include/apex/core/  ← 헤더
│   ├── src/ tests/ examples/ benchmarks/ schemas/
│   ├── bin/{variant}/  ← 빌드 출력 (debug/, release/)
│   └── build.bat / build.sh / CMakePresets.json
├── docs/               ← 전체 프로젝트 문서 (중앙 집중)
│   ├── Apex_Pipeline.md  ← 마스터 설계서
│   ├── apex_common/      ← 공통 (plans/progress/review)
│   └── apex_core/ apex_infra/ apex_shared/ apex_tools/
├── apex_services/      ← MSA 서비스
├── apex_shared/        ← 공유 C++ 라이브러리 + 외부 어댑터
│   ├── lib/adapters/     ← 외부 어댑터 (common, kafka, redis, pg)
│   └── tests/            ← unit/ + integration/ 테스트
├── apex_infra/         ← Docker, K8s 인프라 (Kafka/Redis/PG + Prometheus/Grafana)
│   └── docker/ci.Dockerfile  ← CI + 로컬 Linux 빌드 겸용 Docker 이미지
└── apex_tools/         ← CLI, 스크립트, git-hooks, auto-review
```

## 빌드 환경

Windows 10 Pro, VS2022 (MSVC 19.44), C++23, CMake + Ninja + vcpkg.

- **빌드 명령** (bash 셸에서):
  ```bash
  cmd.exe //c build.bat debug      # 모노레포 루트 빌드 (configure + build + test)
  cmd.exe //c build.bat release    # release 빌드
  ```
  - Windows에서는 vcvarsall.bat 호출이 필요하므로 cmd.exe 경유 필수 (build.sh는 Linux/CI 전용)
  - `//c` 사용 (MSYS bash가 `/c`를 경로로 변환하므로)
  - `run_in_background` 사용 시 리다이렉트 없이 직접 출력: `cmd.exe //c "D:\\.workspace\\build.bat debug"`
  - 포그라운드에서 출력 캡처 필요 시: `> build_log.txt 2>&1` 후 `cat build_log.txt`
  - **빌드는 항상 `run_in_background: true`로 실행** (타임아웃 방지)
- **빌드 변형**: debug / release / asan / tsan
- 상세 (의존성, MSVC 주의사항) → `apex_core/CLAUDE.md`

## 로드맵

- **버전 체계**: `v[메이저].[대].[중].[소]` — 메이저 0=개발중, 1=프레임워크 완성. 대=도메인 전환, 중=마일스톤, 소=수정/리뷰
- **현재**: v0.4.5.0 (코어 메모리 아키텍처 + 어댑터 개선)
- **다음**: v0.4 (외부 어댑터) → v0.5 (서비스 체인) → v0.6 (운영 인프라) → v1.0.0.0 (프레임워크 완성)
- **v1.0.0.0 이후**: v1.1+ (게임 레퍼런스 — 게임 서비스 + Android 클라이언트 + AWS)
- 상세: `docs/Apex_Pipeline.md` §10

## 워크플로우 규칙

### Git / 브랜치
- **커밋 메시지는 한국어로 작성** — `feat(core): 코루틴 할당기 추가` 형태. 타입 접두사(`feat`, `fix`, `refactor`, `docs` 등)와 스코프는 영어, 설명은 한국어
- **초기 설정** (클론 후 1회): `git config core.hooksPath apex_tools/git-hooks`
- **main 직접 커밋 절대 금지** (pre-commit hook 강제) — feature/* 또는 bugfix/* 에서 작업
- **git worktree**: `.worktrees/` 하위에 생성, 에이전트별 독립 작업
  - 생성: `git worktree add .worktrees/<name> -b <branch-name>` → 직후 `git config --global --add safe.directory D:/.workspace/.worktrees/<name>` (Windows 소유권 오류 방지)
  - 삭제: `git worktree remove .worktrees/<name>`
- **머지**: 리뷰 이슈 0건 → squash merge (`gh pr merge --squash --admin --delete-branch`) → 워크트리 삭제
- **머지 전 필수 갱신**: `docs/Apex_Pipeline.md` 완료 이력+현재 버전, `CLAUDE.md` 로드맵 현재 버전, `README.md` 현재 상태+변경 내역 — 세 문서 모두 최신 반영 후 머지

### 문서
- **필수 작성**: 계획서(`plans/`), 완료 기록(`progress/`), 리뷰 보고서(`review/`)
- **작성 타이밍**: plans → 구현 전, review → 리뷰 완료 후, progress → CI 통과 후 merge 전
- **문서 위치**: 프로젝트 전용 → `docs/<project>/`, 공통 → `docs/apex_common/`, 걸치는 문서 → 양쪽에 관점 조정하여 작성 (단순 복사 금지)
- 파일명: `YYYYMMDD_HHMMSS_<topic>.md` — 타임스탬프는 실제 작성 시간

### 코드 리뷰
- **clangd LSP + superpowers:code-reviewer 병행** — LSP 정적 분석(타입/참조/호출 추적)과 AI 코드 리뷰를 함께 사용해야 품질이 높아진다
- **clangd LSP 효율 전략**: `documentSymbol` 병렬 → 핵심 API `hover` → 의심 패턴 `findReferences`/`incomingCalls`. 전수 분석 금지, 10분 타임아웃.
- **설계 문서 정합성**: 아키텍처 영향 변경 시 `Apex_Pipeline.md` 일치 확인 필수
- **태스크 완료 후 `/auto-review task` 묻지 말고 자동 실행** — 리뷰 → 수정 → 재리뷰 → Clean → PR+CI 전 과정 자동화

### 브레인스토밍
- 1단계에서 반드시 `docs/Apex_Pipeline.md` 읽고 관련 섹션 식별, 설계 후 업데이트 포함

### 에이전트 작업
- **모든 작업은 에이전트 팀 병렬 실행** — 수정 가능 파일 목록 명시해서 충돌 방지
- **빌드는 한 번에 하나만** — MSVC+Ninja가 멀티코어를 풀로 사용하므로, 동시 빌드 시 시스템 렉 심함. 파일 작성은 병렬로 하되 빌드/테스트는 순차 실행
- **빌드는 항상 백그라운드 실행** — `run_in_background: true`로 실행. 타임아웃 시 kill되므로 빌드는 절대 포그라운드로 돌리지 않음
- **auto-review 프로세스 대기** — coordinator/리뷰어가 정의된 프로세스대로 동작 중이면 재촉하지 않고 기다림. 프로세스에 report 전송이 명시되어 있으면 요청 없이 자동 전송될 때까지 대기

## 프로젝트 정보

- GitHub: `Gazuua/apex_pipeline`

## 상세 가이드 포인터

- 빌드/MSVC/아키텍처 결정/벤치마크 상세 → `apex_core/CLAUDE.md`
- CI/CD 트러블슈팅 → `.github/CLAUDE.md`
