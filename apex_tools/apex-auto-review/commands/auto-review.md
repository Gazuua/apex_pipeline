---
description: "Auto-review: 서브에이전트 기반 코드 리뷰 + 수정 자동화"
argument-hint: "[task|full]"
allowed-tools: ["Bash", "Glob", "Grep", "Read", "Edit", "Write", "Agent"]
---

# Auto-Review

## 목적

변경 코드의 품질을 높인다.
미발견 버그, 부실한 구현, 정합성 문제, 더 나은 개선 방안을 포괄적으로 검토하여 직접 수정한다.

**모드:** "$ARGUMENTS" (기본값: `task`)
- `task`: `git diff main...HEAD` 기준 변경 파일만 리뷰
- `full`: 전체 코드베이스 리뷰

> **자동화 원칙**: 전 과정을 유저 확인 없이 자동 진행한다.
> 고칠 수 있으면 고친다. 못 고치면 `docs/BACKLOG.md`에 기록한다.

---

## 리뷰어 (최대 7명)

| 리뷰어 | 도메인 |
|--------|--------|
| docs-spec | 원천 문서 정합성 (설계서<->코드, 버전, 용어, 로드맵) |
| docs-records | 기록 문서 품질 (plans/progress/review 포맷, 추적성) |
| logic | 비즈니스 로직 (알고리즘, 상태 전이, 에러 처리, 엣지 케이스) |
| systems | 메모리 + 동시성 (RAII, lifetime, 코루틴, strand, Boost.Asio) |
| test | 테스트 (커버리지, assertion, 격리성, 네이밍) |
| design | API + 아키텍처 (인터페이스, concept/CRTP, 모듈 경계, 의존성) |
| infra-security | 인프라 + 보안 (CMake, vcpkg, CI, Docker, 입력 검증, 크레덴셜) |

이것은 최대 구성이다. 변경 범위에 따라 필요한 리뷰어만 디스패치한다.
소규모 변경이면 서브에이전트 없이 메인이 직접 처리해도 된다.

---

## 흐름 개요

순서는 가이드일 뿐 강제 절차가 아니다. 메인 에이전트가 상황에 맞게 자율 판단한다.

1. **변경 분석** -- diff 확인, 변경 파일 읽기, 영향 범위 파악
2. **리뷰어 디스패치** -- Agent tool로 필요한 리뷰어를 병렬 디스패치
3. **결과 종합** -- 서브에이전트 결과를 취합하고 cross-cutting 분석 (메인 직접)
4. **빌드 검증** -- 수정이 있으면 빌드 + 테스트 실행
5. **마무리** -- 문서 갱신, 커밋, PR, CI

---

## 서브에이전트 디스패치 가이드

Agent tool로 리뷰어를 디스패치할 때 다음을 포함한다:

- **목적**: 왜 리뷰하는지, 어떤 관점으로 봐야 하는지
- **담당 파일 목록**: 수정 가능한 파일 범위 (서브에이전트 간 충돌 방지)
- **변경 맥락**: diff 요약, 작업 목적, 관련 설계 결정
- **읽어야 할 CLAUDE.md**: 해당 영역의 규칙 파일 경로 (서브에이전트가 직접 Read)
- **수정 규칙**: 담당 파일 내 이슈는 직접 수정, 담당 밖 이슈는 보고만

---

## 안전 제약

| 조건 | 동작 |
|------|------|
| main 브랜치에서 실행 | 리뷰 브랜치 자동 생성 후 진행 (아래 § main 자동 분기 참조) |
| 작업 커밋 없음 (task 모드) | 즉시 중단 |
| 과도한 라운드 반복 | 중단 + 유저 보고 |
| 빌드는 한 번에 하나만 | `run_in_background: true`, timeout 설정 금지 |
| 수정 불가 이슈 | `docs/BACKLOG.md`에 기록 (유저 미개입) |

### main 자동 분기

main 브랜치에서 auto-review가 실행되면 다음을 자동 수행한다:

1. **최신화**: `git fetch origin main && git pull origin main`
2. **브랜치 생성**: `review/auto-review-YYYYMMDD_HHMMSS` (타임스탬프는 `date +"%Y%m%d_%H%M%S"`로 취득)
3. **핸드오프 등록**: `apex-agent handoff notify start --skip-design` (설계 불필요 → 바로 implementing)
4. **리뷰 진행**: 이후 정상 흐름대로 진행

리뷰 완료 후:
- **수정 있음** → 커밋 + PR 생성 + CI 검증 + 머지 (정상 머지 플로우)
- **수정 없음** → 브랜치 삭제 (`git push origin --delete {branch} && git checkout main && git branch -D {branch}`), 리뷰 문서만 main에 커밋

---

## 마무리 규칙

### 머지 전 필수 갱신
- `docs/Apex_Pipeline.md` -- 아키텍처 영향 변경 반영
- `CLAUDE.md` -- 로드맵 현재 버전 반영
- `README.md` -- 현재 상태 반영
- `docs/BACKLOG.md` -- 완료 항목 삭제, 신규 항목 추가

### 리뷰 문서
- 경로: `docs/{project}/review/YYYYMMDD_HHMMSS_auto-review.md`
- 리뷰 항목 상세 포함 필수 (헤더/통계만 있는 빈 껍데기 금지)

### 진행 기록
- 경로: `docs/{project}/progress/YYYYMMDD_HHMMSS_{topic}.md`
- 작업 결과 요약 필수

### CI 결과 확인
1. `gh run watch {run-id}` -- 완료 대기 (`run_in_background: true`)
2. 완료 후 반드시 `gh run view {run-id}` -- 전체 run 상태 확인
3. `gh run watch` 출력의 tail만으로 성공 여부를 판단하지 말 것
