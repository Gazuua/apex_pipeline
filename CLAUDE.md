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
└── apex_tools/         ← CLI, 스크립트, git-hooks
```

## 빌드 환경

Windows 10 Pro, VS2022 (MSVC 19.44), C++23, CMake + Ninja + vcpkg.
상세 (빌드 명령, 변형, 의존성, MSVC 주의사항) → `apex_core/CLAUDE.md`

## 로드맵

- **버전 체계**: `v[메이저].[대].[중].[소]` — 메이저 0=개발중, 1=프레임워크 완성. 대=도메인 전환, 중=마일스톤, 소=수정/리뷰
- **현재**: v0.4.4.0 (외부 어댑터 완료)
- **다음**: v0.4 (외부 어댑터) → v0.5 (서비스 체인) → v0.6 (운영 인프라) → v1.0.0.0 (프레임워크 완성)
- **v1.0.0.0 이후**: v1.1+ (게임 레퍼런스 — 게임 서비스 + Android 클라이언트 + AWS)
- 상세: `docs/Apex_Pipeline.md` §10

## 워크플로우 규칙

### Git / 브랜치
- **초기 설정** (클론 후 1회): `git config core.hooksPath apex_tools/git-hooks`
- **main 직접 커밋 절대 금지** (pre-commit hook 강제) — feature/* 또는 bugfix/* 에서 작업
- **git worktree**: `.worktrees/` 하위에 생성, 에이전트별 독립 작업
  - 생성: `git worktree add .worktrees/<name> -b <branch-name>`
  - 삭제: `git worktree remove .worktrees/<name>`
- **머지**: 리뷰 이슈 0건 → squash merge → 브랜치+워크트리 삭제

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

## 프로젝트 정보

- GitHub: `Gazuua/apex_pipeline`

## 상세 가이드 포인터

- 빌드/MSVC/아키텍처 결정/벤치마크 상세 → `apex_core/CLAUDE.md`
- CI/CD 트러블슈팅 → `.github/CLAUDE.md`
