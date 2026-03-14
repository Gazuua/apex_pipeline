---
description: "Automated multi-agent review -> fix -> re-review loop until clean, then PR + CI"
argument-hint: "[task|full]"
allowed-tools: ["Bash", "Glob", "Grep", "Read", "Edit", "Write", "Agent", "TeamCreate", "SendMessage"]
---

# Auto-Review: 3계층 팀장 위임 모델 리뷰 자동화

3계층 teammate mode 구조로 코드 리뷰를 자동화한다.
팀장(coordinator)이 11명의 전문 리뷰어를 지휘하여 find-and-fix 방식으로 이슈를 발견+수정한다.
메인 오케스트레이터는 초기화, 팀장 디스패치, PR/CI 관리에만 집중한다.

**모드:** "$ARGUMENTS" (기본값: `task`)

> **자동화 원칙**: Phase 0~5 전 과정을 유저 확인 없이 자동 진행한다.
> 각 Phase 완료 후 "진행할까?" 등의 확인을 묻지 않는다.
> 유저 개입이 필요한 경우는 안전장치(동일 이슈 5회 반복, CI 5회 실패, 라운드 10회 초과)에 의한 강제 중단뿐이다.

---

## Phase 0: 초기화

1. **모드 결정**
   - `task` (기본): `git diff main...HEAD`로 변경된 파일만 리뷰
   - `full`: 전체 프로젝트 리뷰 (diff 무관)

2. **브랜치 확인**
   - 현재 브랜치가 `main`이 아닌지 확인 -- main이면 즉시 중단
   - `task` 모드: `git log --oneline main...HEAD`로 작업 커밋 존재 확인 -- 없으면 즉시 중단
   - `full` 모드: 작업 커밋 불필요

3. **팀장 디스패치 컨텍스트 수집**
   - `task` 모드: `git diff --name-only main...HEAD`로 변경 파일 목록 추출
   - `full` 모드: 변경 파일 목록 생략

---

## Phase 1+2: 팀장에게 위임

Phase 1(리뷰)과 Phase 2(수정+재리뷰 루프)를 팀장에게 일괄 위임한다.
메인은 이슈 상세를 읽지 않는다 -- 팀장이 자율적으로 운영하고 최종 report만 받는다.

1. **팀장 생성 (TeamCreate)**

   TeamCreate로 `review-coordinator` 에이전트를 teammate mode로 생성:
   - 에이전트 파일: `apex_tools/auto-review/agents/coordinator.md`

2. **dispatch 메시지 전송 (SendMessage)**

   팀장에게 dispatch 메시지 전송:
   ```
   [dispatch]
   mode: {task|full}
   changed_files: {변경 파일 목록 -- task 모드 시}
   diff_ref: main..{현재 브랜치}
   smart_skip_config: apex_tools/auto-review/config.md
   round_limit: 10
   safety_rules:
     - 동일 이슈 5회 반복 -> 에스컬레이션
     - 전체 10라운드 초과 -> 에스컬레이션
   ```

3. **report 수신 대기**

   팀장의 report 메시지를 기다린다. report에는:
   - 총 라운드 수
   - 수정 건수
   - 에스컬레이션 건수 (미해결 이슈)
   - 미리뷰 영역
   - 빌드 검증 필요 여부

4. **에스컬레이션 처리**

   팀장이 에스컬레이션한 경우 (동일 이슈 5회 / 라운드 10회 초과):
   - 미해결 이슈 목록을 유저에게 보고
   - 프로세스 종료

---

## Phase 3: PR 생성

1. **리뷰 보고서 저장**
   - 경로: `docs/{project}/review/YYYYMMDD_HHMMSS_auto-review.md`
   - 팀장 report 기반으로 전체 라운드 요약 포함

2. **보고서 커밋 + 푸시**
   ```bash
   git add docs/*/review/
   git commit -m "docs: auto-review 리뷰 보고서"
   git push -u origin {브랜치명}
   ```

3. **PR 생성**
   ```bash
   gh pr create --title "{브랜치 요약}" --body "..."
   ```

4. **CI 대기**
   ```bash
   gh run watch {run-id}  # run_in_background: true, timeout: 900000
   ```
   - CI 전체 통과 -> Phase 5로
   - CI 실패 -> Phase 4로

---

## Phase 4: CI 수정 루프

1. **CI 실패 분석**
   ```bash
   gh run view {run-id} --log-failed
   ```

2. **실패 원인 분류**

   **A) GitHub 인프라 문제** (vcpkg CDN, runner 타임아웃 등):
   ```bash
   gh run rerun {run-id} --failed
   gh run watch {run-id}  # run_in_background: true
   ```

   **B) 코드 문제** (빌드 에러, 테스트 실패):
   - 팀장에게 재디스패치 (CI 로그 + 수정 지시)
   - 팀장이 관련 리뷰어 배정하여 수정
   - 수정 완료 -> 커밋 + 푸시 + CI 재대기

3. **반복 제한**
   - 동일 CI 실패 5회 -> 루프 중단 + 유저 보고

---

## Phase 5: 완료

1. **최종 완료 보고서 작성**

2. **프로젝트 문서 업데이트**
   - `docs/Apex_Pipeline.md`: 아키텍처 영향 변경 반영
   - `README.md`: 현재 상태 반영
   - `CLAUDE.md`: 로드맵 현재 버전 반영

3. **완료 기록 작성**
   - 경로: `docs/{project}/progress/YYYYMMDD_HHMMSS_{topic}.md`

4. **보고서 + 문서 + 완료 기록 커밋 + 푸시**
   - 문서 변경만이므로 CI 대기 없이 바로 머지

5. **Squash Merge + 정리**
   ```bash
   gh pr merge {PR번호} --squash --delete-branch
   ```
   - 워크트리 정리: `git worktree remove .worktrees/{name}`

6. **유저에게 완료 보고**: PR URL, 총 라운드 수, 수정 이슈 총 건수

---

## 안전장치 요약

| 조건 | 동작 |
|------|------|
| main 브랜치에서 실행 | 즉시 중단 |
| 작업 커밋 없음 (task 모드) | 즉시 중단 |
| 동일 이슈 5회 반복 | 루프 중단 + 유저 보고 |
| 전체 라운드 10회 초과 | 루프 중단 + 유저 보고 |
| 동일 CI 실패 5회 반복 | 루프 중단 + 유저 보고 |

---

## 에이전트 구성

### 팀장
| 에이전트 | 파일 | 역할 |
|---------|------|------|
| review-coordinator | `auto-review/agents/coordinator.md` | 리뷰어 팀 구성+관리, 라운드 운영, 보고서 |

### 리뷰어 (11명)
| 에이전트 | 파일 | 도메인 |
|---------|------|--------|
| reviewer-docs-spec | `auto-review/agents/reviewer-docs-spec.md` | 원천 문서 정합성 |
| reviewer-docs-records | `auto-review/agents/reviewer-docs-records.md` | 기록 문서 품질 |
| reviewer-architecture | `auto-review/agents/reviewer-architecture.md` | 설계 <-> 코드 정합 |
| reviewer-logic | `auto-review/agents/reviewer-logic.md` | 비즈니스 로직 |
| reviewer-memory | `auto-review/agents/reviewer-memory.md` | 메모리 관리 |
| reviewer-concurrency | `auto-review/agents/reviewer-concurrency.md` | 동시성 |
| reviewer-api | `auto-review/agents/reviewer-api.md` | API/concept 설계 |
| reviewer-test-coverage | `auto-review/agents/reviewer-test-coverage.md` | 테스트 커버리지 |
| reviewer-test-quality | `auto-review/agents/reviewer-test-quality.md` | 테스트 코드 품질 |
| reviewer-infra | `auto-review/agents/reviewer-infra.md` | 빌드/CI/인프라 |
| reviewer-security | `auto-review/agents/reviewer-security.md` | 보안 |
