---
description: "Automated multi-agent review -> fix -> re-review loop until clean, then PR + CI"
argument-hint: "[task|full]"
allowed-tools: ["Bash", "Glob", "Grep", "Read", "Edit", "Write", "Agent", "TeamCreate", "TeamDelete", "SendMessage"]
---

# Auto-Review: 3계층 팀장 위임 모델 리뷰 자동화

3계층 teammate mode 구조로 코드 리뷰를 자동화한다.
팀장(coordinator)이 12명의 전문 리뷰어를 지휘하여 find-and-fix 방식으로 이슈를 발견+수정한다.
메인 오케스트레이터는 초기화, 팀장 디스패치, 빌드/CI 실행, PR 관리에 집중한다.

> **역할 분리 원칙**
> - **메인(오케스트레이터)** = 외부 시스템 담당: 빌드 실행, CI 모니터링, PR 생성/머지
> - **coordinator(팀장)** = 리뷰 관리 담당: 리뷰어 assign/취합, 수정 지시, 라운드 운영, 보고서
> - coordinator는 빌드를 직접 실행하지 않는다. 메인으로부터 빌드/CI 실패 로그를 수신하면 파일 소유권 기반으로 리뷰어에게 수정을 지시한다.

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

## Phase 1+2: 팀 구성 + 팀장에게 위임

Phase 1(리뷰)과 Phase 2(수정+재리뷰 루프)를 팀장에게 일괄 위임한다.
메인은 이슈 상세를 읽지 않는다 -- 팀장이 자율적으로 운영하고 최종 report만 받는다.

> **구조**: 메인이 리뷰어 12명을 먼저 스폰 → 전원 등록 확인 → coordinator를 마지막에 스폰한다.
> coordinator는 스폰 시점에 리뷰어 전원 등록이 보장되므로, 즉시 리뷰 프로세스를 시작한다.
> coordinator는 Agent 도구 없이 SendMessage로 이미 등록된 리뷰어 teammate들을 관리만 한다.

1. **팀 생성 (TeamCreate)**

   TeamCreate로 리뷰 팀을 생성한다 (팀 구조만 생성, 에이전트 스폰 아님):
   - team_name: `auto-review`
   - description: `코드 리뷰 자동화 팀`

2. **리뷰어 12명 먼저 스폰 (Agent x 12, 병렬)**

   메인이 Agent 도구로 리뷰어 12명을 같은 팀에 스폰한다.
   12명 모두 독립적인 Agent 호출이므로 **병렬 스폰** 가능하다.

   **각 리뷰어 스폰:**
   - `prompt`: `"에이전트 정의 파일 {에이전트 파일 경로}를 읽고 지시에 따르라. coordinator의 assign 메시지를 기다려라."`
   - `name`: `reviewer-{domain}` (예: `reviewer-logic`, `reviewer-memory`)
   - `team_name`: `auto-review`
   - `mode`: `"auto"`
   - `run_in_background`: `true`

   에이전트 파일 경로: `apex_tools/claude-plugin/agents/reviewer-{domain}.md`

3. **전원 등록 확인**

   리뷰어 12명 스폰 후, 전원이 팀에 등록되었는지 확인한다.
   `~/.claude/teams/auto-review/config.json`을 Read로 읽어서 `members` 배열 검증:
   - 리뷰어 12명 전원 등록 확인
   - 미등록 멤버가 있으면 **5초 후 재확인** (최대 3회)
   - 3회 재확인 후에도 미등록 멤버가 있으면 프로세스 중단 + 유저에게 보고

4. **coordinator 스폰 (Agent x 1)**

   리뷰어 전원 등록 확인 후, coordinator를 마지막에 스폰한다.
   리뷰어가 이미 전원 등록되어 있으므로, coordinator는 스폰 직후 즉시 리뷰 프로세스를 시작할 수 있다.

   - `prompt`: 아래 dispatch 내용을 포함한 전체 지시
   - `name`: `review-coordinator`
   - `team_name`: `auto-review`
   - `mode`: `"auto"`
   - `run_in_background`: `true`

   prompt에 포함할 dispatch 내용:
   ```
   [dispatch]
   mode: {task|full}
   changed_files: {변경 파일 목록 -- task 모드 시}
   diff_ref: main..{현재 브랜치}
   working_directory: {작업 디렉토리}
   smart_skip_config: apex_tools/auto-review/config.md
   round_limit: 10
   safety_rules:
     - 동일 이슈 5회 반복 -> 에스컬레이션
     - 전체 10라운드 초과 -> 에스컬레이션

   리뷰어 12명 전원 등록 완료. 컨텍스트 수집 후 즉시 assign 시작하라.
   ```

   > **coordinator 시작 타이밍**: 리뷰어 12명이 전원 등록된 후 coordinator를 스폰하므로,
   > coordinator는 스폰 직후 컨텍스트 수집(에이전트 정의, config, smart_skip 등) 완료 즉시 리뷰어 assign을 시작한다.
   > start 시그널 대기 단계가 불필요하다 — 스폰 순서 자체가 전원 등록을 구조적으로 보장한다.

   > **참고**: coordinator는 Agent 도구를 사용하지 않는다. 리뷰어들이 이미 같은 팀에 스폰되어 있으므로,
   > coordinator는 SendMessage로 assign 메시지를 보내 관리만 수행한다. 상세는 `coordinator.md` Step 4 참조.

5. **report 수신 대기**

   팀장의 report 메시지를 기다린다. report에는:
   - 총 라운드 수
   - 수정 건수
   - 에스컬레이션 건수 (미해결 이슈)
   - 미리뷰 영역
   - 빌드 검증 필요 여부 (수정이 1건이라도 있으면 Yes → 메인이 Phase 2.5 진행)

6. **에스컬레이션 처리**

   coordinator로부터 에스컬레이션을 수신하면 다음 정책에 따라 처리한다.
   에스컬레이션은 "리뷰어가 못 고침"이지 "유저에게 물어봐야 함"이 아니다.

   **A) 메인이 자체 수정 시도**
   - 에스컬레이션된 이슈를 메인이 직접 분석하고 수정을 시도한다
   - 리뷰어보다 넓은 컨텍스트를 활용하여 해결한다

   **B) 자체 수정 불가 시 — 백로그 문서로 기록**
   - 유저에게 물어보지 않는다. 대신 백로그 문서를 생성한다:
     - 경로: `docs/{project}/review/YYYYMMDD_HHMMSS_backlog.md`
     - 내용: 이슈 상세, 수정 불가 사유, 권장 해결 시점 (예: v0.5, v0.6), 관련 코드 위치
     - 이 문서는 리뷰 보고서와 함께 커밋된다

   **C) 자체 수정 불가 판단 기준**
   - 로드맵에 명시된 재설계가 필요한 경우 (v0.5/v0.6 스코프 변경)
   - 아키텍처 수준의 변경이 필요하여 리뷰 범위를 벗어나는 경우
   - 외부 의존성 변경이 필요한 경우

   **D) 유저 개입은 안전장치에 의한 강제 중단 시에만**
   - 동일 이슈 5회 반복, 라운드 10회 초과, CI 5회 실패 등
   - 이 경우에만 프로세스를 중단하고 유저에게 보고한다

   > **핵심 원칙**: auto-review는 완전 자동이다. 유저에게 "이거 고칠까요?" "어떻게 할까요?"를 묻지 않는다.
   > 고칠 수 있으면 고친다. 못 고치면 백로그에 기록한다. 끝.

---

## Phase 2.5: 빌드 검증 (메인 직접 실행)

coordinator의 report 수신 후, 메인이 직접 빌드를 실행하여 검증한다.
coordinator는 빌드를 실행하지 않으며, 메인이 빌드 결과를 판단하고 필요 시 coordinator에게 수정을 지시한다.

> **팀 유지 원칙**: 이 Phase 동안 coordinator와 리뷰어는 해산되지 않고 대기 상태(idle)를 유지한다.
> 빌드 실패 시 메인이 coordinator에게 `build-failure`를 보내면, coordinator가 파일 소유권 기반으로 리뷰어를 재배정하여 수정을 진행한다.

1. **빌드 실행**
   ```bash
   cmd.exe //c "D:\\.workspace\\build.bat debug"
   ```
   - **빌드 실행 규칙**: `run_in_background: true` 필수, `timeout` 파라미터 절대 설정 금지
   - 빌드 완료 알림까지 무한 대기

2. **빌드 결과 판정 (3분류)**

   | 분류 | 판정 조건 | 처리 |
   |------|----------|------|
   | **빌드 성공** | exit code = 0 | 테스트(ctest) 진행 → 성공 시 Phase 3으로 |
   | **빌드 실패** | exit code ≠ 0 (컴파일/링크 오류) | 아래 수정 루프 |
   | **시스템 오류** | killed / timeout / 기타 | 재시도 1회 → 재실패 시 빌드 검증 스킵, Phase 3으로 |

3. **빌드 실패 시 수정 루프**
   - 메인이 빌드 실패 로그를 coordinator에게 `build-failure` 메시지로 전달:
     ```
     [build-failure]
     phase: build (또는 test)
     log: {실패 로그 핵심 부분}
     failed_files: {컴파일 실패 파일 목록}
     ```
   - coordinator가 파일 소유권 기반으로 담당 리뷰어에게 수정 지시
   - 리뷰어 수정 완료 → coordinator가 커밋 + report 전송
   - 메인이 재빌드 실행
   - **반복 제한**: 빌드 실패 5회 → 루프 중단 + 유저 보고

4. **테스트 실행** (빌드 성공 시)
   ```bash
   cd build && ctest --preset debug-test
   ```
   - 테스트 실패 시에도 위 수정 루프와 동일한 절차 (log에 테스트 실패 내용 포함)

---

## Phase 3: PR 생성

1. **리뷰 보고서 저장**
   - 경로: `docs/{project}/review/YYYYMMDD_HHMMSS_auto-review.md`
   - 팀장 report 기반으로 전체 라운드 요약 포함

2. **보고서 커밋 + 푸시**

   > **참고**: 코드 수정분은 coordinator가 라운드별로 이미 커밋 완료함 (`fix(review): Round {N} 리뷰 수정 반영`).
   > 여기서는 리뷰 보고서만 추가 커밋한다.

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

> **팀 유지 원칙**: 이 Phase 동안 coordinator와 리뷰어는 해산되지 않고 대기 상태(idle)를 유지한다.
> CI 실패 시 메인이 coordinator에게 `build-failure`를 보내면, coordinator가 파일 소유권 기반으로 리뷰어를 재배정하여 수정을 진행한다.

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
   - 메인이 CI 실패 로그를 확인하고 coordinator에게 `build-failure` 메시지로 전달:
     ```
     [build-failure]
     phase: ci
     log: {CI 실패 로그 핵심 부분}
     failed_files: {실패 관련 파일 목록}
     ```
   - coordinator가 파일 소유권 기반으로 담당 리뷰어 배정하여 수정
   - 수정 완료 -> 커밋 + 푸시 + CI 재대기

3. **반복 제한**
   - 동일 CI 실패 5회 -> 루프 중단 + 유저 보고

---

## Phase 5: 완료

1. **팀 해산**

   빌드(Phase 2.5), CI(Phase 4), PR 머지까지 모든 외부 검증이 완료된 후에만 팀을 해산한다.
   report 수신 시점에는 해산하지 않는다 — 빌드/CI 실패 시 coordinator에게 추가 수정을 지시해야 하므로 팀을 유지한다.

   1. 메인이 빌드/CI/PR 전부 완료 확인
   2. coordinator에게 SendMessage로 `shutdown_request` 전송
   3. coordinator가 모든 리뷰어에게 `shutdown_request`를 보내고, 리뷰어 전원 종료 확인 후 자신도 승인
   4. 모든 teammate 종료 확인 후 TeamDelete로 팀 정리

   ### 좀비 에이전트 Fallback
   - coordinator shutdown 후 TeamDelete 시도
   - TeamDelete 실패 시 (active member 존재):
     1. `~/.claude/teams/{team-name}/config.json` 읽기
     2. 남아있는 멤버를 config.json의 members 배열에서 직접 제거
     3. TeamDelete 재시도
   - 이 fallback은 안전장치이며, 정상 흐름에서는 발동하지 않음

2. **최종 완료 보고서 작성**

3. **프로젝트 문서 업데이트**
   - `docs/Apex_Pipeline.md`: 아키텍처 영향 변경 반영
   - `README.md`: 현재 상태 반영
   - `CLAUDE.md`: 로드맵 현재 버전 반영

4. **완료 기록 작성**
   - 경로: `docs/{project}/progress/YYYYMMDD_HHMMSS_{topic}.md`

5. **보고서 + 문서 + 완료 기록 커밋 + 푸시**
   - 문서 변경만이므로 CI 대기 없이 바로 머지

6. **Squash Merge + 정리**
   ```bash
   gh pr merge {PR번호} --squash --delete-branch
   ```
   - 워크트리 정리: `git worktree remove .worktrees/{name}`

7. **유저에게 완료 보고**: PR URL, 총 라운드 수, 수정 이슈 총 건수

---

## 안전장치 요약

| 조건 | 동작 |
|------|------|
| main 브랜치에서 실행 | 즉시 중단 |
| 작업 커밋 없음 (task 모드) | 즉시 중단 |
| 리뷰어 에스컬레이션 (수정 불가) | 메인이 자체 수정 시도 → 불가 시 백로그 문서 기록 (유저 미개입) |
| 동일 이슈 5회 반복 | 루프 중단 + 유저 보고 |
| 전체 라운드 10회 초과 | 루프 중단 + 유저 보고 |
| 빌드 실패 5회 반복 (Phase 2.5) | 루프 중단 + 유저 보고 |
| 동일 CI 실패 5회 반복 | 루프 중단 + 유저 보고 |

---

## 에이전트 구성

### 팀장
| 에이전트 | 파일 | 역할 |
|---------|------|------|
| review-coordinator | `apex_tools/claude-plugin/agents/coordinator.md` | 리뷰어 팀 관리 (SendMessage 기반), 라운드 운영, 보고서 |

### 리뷰어 (12명)
| 에이전트 | 파일 | 도메인 |
|---------|------|--------|
| reviewer-docs-spec | `apex_tools/claude-plugin/agents/reviewer-docs-spec.md` | 원천 문서 정합성 |
| reviewer-docs-records | `apex_tools/claude-plugin/agents/reviewer-docs-records.md` | 기록 문서 품질 |
| reviewer-architecture | `apex_tools/claude-plugin/agents/reviewer-architecture.md` | 설계 <-> 코드 정합 |
| reviewer-logic | `apex_tools/claude-plugin/agents/reviewer-logic.md` | 비즈니스 로직 |
| reviewer-memory | `apex_tools/claude-plugin/agents/reviewer-memory.md` | 메모리 관리 |
| reviewer-concurrency | `apex_tools/claude-plugin/agents/reviewer-concurrency.md` | 동시성 |
| reviewer-api | `apex_tools/claude-plugin/agents/reviewer-api.md` | API/concept 설계 |
| reviewer-test-coverage | `apex_tools/claude-plugin/agents/reviewer-test-coverage.md` | 테스트 커버리지 |
| reviewer-test-quality | `apex_tools/claude-plugin/agents/reviewer-test-quality.md` | 테스트 코드 품질 |
| reviewer-infra | `apex_tools/claude-plugin/agents/reviewer-infra.md` | 빌드/CI/인프라 |
| reviewer-security | `apex_tools/claude-plugin/agents/reviewer-security.md` | 보안 |
| reviewer-cross-cutting | `apex_tools/claude-plugin/agents/reviewer-cross-cutting.md` | 도메인 간 경계 검증 |
