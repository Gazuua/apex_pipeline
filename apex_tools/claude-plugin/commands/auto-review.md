---
description: "Automated multi-agent review → fix → re-review loop until clean, then PR + CI"
argument-hint: "[task|full]"
allowed-tools: ["Bash", "Glob", "Grep", "Read", "Edit", "Write", "Agent"]
---

# Auto-Review: 멀티에이전트 리뷰 → 수정 → CI 자동화

자동화된 리뷰 파이프라인. 5개 전문 리뷰어 에이전트가 병렬로 코드를 검사하고, 발견된 이슈를 수정하고, Clean(0건)까지 반복한 뒤 PR 생성 + CI 통과까지 처리.

**모드:** "$ARGUMENTS" (기본값: `task`)

> **자동화 원칙**: Phase 0~5 전 과정을 유저 확인 없이 자동 진행한다.
> 각 Phase 완료 후 "진행할까?" 등의 확인을 묻지 않는다.
> 유저 개입이 필요한 경우는 안전장치(동일 이슈 5회 반복, CI 5회 실패)에 의한 강제 중단뿐이다.

---

## Phase 0: 초기화

1. **모드 결정**
   - `task` (기본): `git diff main...HEAD`로 변경된 파일만 리뷰
   - `full`: 전체 프로젝트 리뷰 (diff 무관)

2. **브랜치 확인**
   - 현재 브랜치가 `main`이 아닌지 확인 — main이면 즉시 중단
   - `task` 모드: `git log --oneline main...HEAD`로 작업 커밋 존재 확인 — 없으면 즉시 중단
   - `full` 모드: 작업 커밋 불필요 — 전체 프로젝트를 리뷰하므로 diff 없이 진행 가능

3. **이슈 추적 초기화**
   - 이슈 추적 맵: `파일경로:카테고리:이슈요약` → 라운드별 누적 카운트
   - 라운드 카운터: 0

---

## Phase 1: 병렬 리뷰

1. **리뷰 범위 결정**
   - `task` 모드: `git diff --name-only main...HEAD`로 변경 파일 목록 추출
   - `full` 모드: 전체 프로젝트 파일 대상 (빌드 출력/bin/ 제외)

2. **리뷰어 에이전트 병렬 디스패치**

   Agent 도구로 **참여 대상 에이전트를 동시에** 하나의 메시지에서 호출:
   - Round 1: 5개 전원 디스패치
   - Round 2+: Phase 2 Smart Re-review 스킵 판단에 따라 참여 에이전트만 디스패치

   | 에이전트 | 파일 | 역할 |
   |---------|------|------|
   | `reviewer-docs` | agents/reviewer-docs.md | 문서 정합성 |
   | `reviewer-structure` | agents/reviewer-structure.md | 구조/구현 정합성 |
   | `reviewer-code` | agents/reviewer-code.md | 코드 품질 |
   | `reviewer-test` | agents/reviewer-test.md | 테스트 커버리지 |
   | `reviewer-general` | agents/reviewer-general.md | 기타 (빌드/CI/의존성) |

   각 에이전트에게 전달할 정보:
   - 리뷰 모드 (`task` 또는 `full`)
   - 변경 파일 목록 (task 모드 시)
   - 현재 브랜치명

3. **결과 취합 → 종합 리뷰 보고서 작성**

   ```markdown
   # 리뷰 보고서 — Round {N}

   ## 요약
   - Critical: X건 / Important: Y건 / Minor: Z건
   - 리뷰어별 이슈 수

   ## Critical Issues
   - [{리뷰어}] 파일:라인 — 설명 + 수정 방안

   ## Important Issues
   - [{리뷰어}] 파일:라인 — 설명 + 수정 방안

   ## Minor Issues
   - [{리뷰어}] 파일:라인 — 설명 + 수정 방안
   ```

4. **이슈 0건이면 → Phase 3으로 점프**

---

## Phase 2: 수정 + 재리뷰 루프

1. **이슈 분류 및 병렬 수정**
   - 이슈를 파일 단위로 그룹핑
   - **파일이 겹치지 않도록** 분할하여 멀티에이전트 병렬 수정
   - 각 수정 에이전트에게: 해당 파일의 이슈 목록 + 수정 방안 전달

2. **이슈 추적 업데이트**
   - 각 이슈의 `파일경로:카테고리:이슈요약` 키로 누적 카운트 증가
   - **동일 이슈 5회 반복 감지 시:**
     - 해당 이슈를 "해결 불가"로 마킹
     - 전체 루프 중단
     - 유저에게 해결 불가 이슈 목록 보고
     - **여기서 프로세스 종료**

3. **수정 커밋**
   - 수정된 파일을 스테이징 + 커밋
   - 커밋 메시지: `fix: auto-review round {N} — {수정 건수}건 수정`

4. **재리뷰 (Phase 1로 돌아감) — Smart Re-review**
   - 라운드 카운터 증가

   **스킵 판단 절차:**

   a) 수정된 파일 목록 추출: `git diff HEAD~1..HEAD --name-only`

   b) 파일 타입 매핑으로 "최소 필수 리뷰어" 결정:

   | 수정 파일 타입 | 영향받는 리뷰어 |
   |--------------|--------------|
   | `.cpp`, `.hpp` (소스/헤더) | code, test, structure |
   | `test_*.cpp`, `test_helpers.hpp` | test |
   | `CMakeLists.txt`, `vcpkg.json`, `build.*`, `CMakePresets*` | structure, general |
   | `*.md` (문서) | docs |
   | `.gitignore`, hooks, scripts | general |

   c) 수정 내용의 **의미적 영향** 추가 판단 (오케스트레이터 자율):
   - 코드 로직 변경 (if/for/while 등 제어 흐름) → docs 추가 (설계 정합성 영향)
   - 의존성 변경 (vcpkg, include) → general 추가
   - 디렉토리/파일 구조 변경 → 전원 재리뷰
   - **판단이 애매하면 → 해당 리뷰어를 포함** (보수적으로)

   d) 스킵 조건 (AND):
   - 직전 라운드에서 **Clean (0건)**
   - 위 판단에서 "영향받는 리뷰어"에 **미포함**
   - 두 조건 모두 만족 → 해당 에이전트 스킵

   e) Phase 1 재실행 시 **스킵 대상이 아닌 에이전트만 디스패치**

   f) 리뷰 보고서에 참여 현황 기록:
   ```markdown
   ## Round {N} 참여 현황
   - reviewer-docs: 스킵 (Round {N-1} Clean + 수정 무관)
   - reviewer-code: 리뷰 수행
   - ...
   ```

   - Clean(0건) 달성까지 반복

---

## Phase 3: PR 생성

1. **리뷰 보고서 저장**
   - 경로: `docs/{project}/review/YYYYMMDD_HHMMSS_auto-review.md`
   - 전체 라운드 요약 포함

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
   gh run watch
   ```
   - CI 전체 통과 → Phase 5로
   - CI 실패 → Phase 4로

---

## Phase 4: CI 수정 루프

1. **CI 실패 분석**
   ```bash
   gh run view {run-id} --log-failed
   ```
   - 실패 잡 + 에러 로그 추출

2. **실패 원인 분류 → 대응**

   **A) GitHub 인프라 문제** (vcpkg 다운로드 실패, CDN HTTP 502, runner 타임아웃 등):
   - 코드 수정 불필요 → 실패한 잡만 재실행
   ```bash
   gh run rerun {run-id} --failed
   gh run watch {run-id}
   ```

   **B) 코드 문제** (빌드 에러, 테스트 실패 등):
   - 에러 원인 분석 → 코드 수정
   - 커밋: `fix: CI {잡이름} 실패 수정 — round {N}`
   - 재푸시 + 재대기:
   ```bash
   git push
   gh run watch
   ```

3. **CI 수정 후 재리뷰 판단**
   - CI 수정으로 코드 변경이 발생한 경우, 변경 규모를 평가:
     - **소규모** (오타, 컴파일러 경고, suppressions 추가 등): 재리뷰 불필요 → CI 재대기
     - **대규모** (테스트 추가/삭제, 로직 변경, 파일 구조 변경 등): **Phase 1로 돌아가 전체 재리뷰**
   - 재리뷰 시 이슈 추적 맵과 라운드 카운터는 유지 (이전 리뷰 기록 보존)

4. **반복 제한**
   - 인프라 문제: 동일 잡 **5회 rerun 실패** → 루프 중단 + 유저 보고
   - 코드 문제: 동일 CI 잡이 같은 원인으로 **5회 실패** → 루프 중단 + 유저 보고
   - **여기서 프로세스 종료**

5. **CI 전체 통과 → Phase 5**

---

## Phase 5: 완료

1. **최종 완료 보고서 작성**
   ```markdown
   # Auto-Review 완료 보고서

   ## 결과
   - 리뷰 라운드: {N}회
   - 총 이슈: {X}건 발견 → 전량 수정
   - CI: 전체 통과

   ## 라운드별 요약
   - Round 1: Critical X / Important Y / Minor Z
   - Round 2: ...
   - Final: Clean (0건)

   ## PR
   - URL: {PR URL}
   - CI Status: ✅ 전체 통과
   ```

2. **프로젝트 문서 업데이트**
   - `docs/Apex_Pipeline.md` (마스터 설계서): 아키텍처 영향 변경이 있었다면 해당 섹션 반영
   - 워크트리 루트 `README.md` (프로젝트 진행 상황): 완료 항목 추가, 알려진 이슈 갱신 등 현재 상태 반영

3. **보고서 + 문서 커밋 + 푸시**

4. **Squash Merge + 정리**
   ```bash
   gh pr merge {PR번호} --squash --delete-branch
   ```
   - 워크트리 정리: `git worktree remove .worktrees/{name}`

5. **유저에게 완료 보고**
   - PR URL
   - 총 리뷰 라운드 수
   - 수정된 이슈 총 건수

---

## 안전장치 요약

| 조건 | 동작 |
|------|------|
| main 브랜치에서 실행 | 즉시 중단 |
| 작업 커밋 없음 (task 모드) | 즉시 중단 |
| 동일 이슈 5회 반복 | 루프 중단 + 유저 보고 |
| 동일 CI 실패 5회 반복 | 루프 중단 + 유저 보고 |

---

## 리뷰어 에이전트 요약

| 에이전트 | 전문 영역 | subagent_type |
|---------|----------|---------------|
| **reviewer-docs** | 문서 정합성 (마스터/설계/README/MEMORY) | `apex-auto-review:reviewer-docs` |
| **reviewer-structure** | 구조/구현 정합성 (디렉토리/CMake/모듈경계) | `apex-auto-review:reviewer-structure` |
| **reviewer-code** | 코드 품질 (버그/보안/성능/패턴) | `apex-auto-review:reviewer-code` |
| **reviewer-test** | 테스트 (커버리지/엣지케이스/격리/assertion) | `apex-auto-review:reviewer-test` |
| **reviewer-general** | 기타 자율 (빌드/CI/의존성/라이선싱) | `apex-auto-review:reviewer-general` |
