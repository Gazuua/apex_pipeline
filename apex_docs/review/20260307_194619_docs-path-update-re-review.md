# 재리뷰: 문서 경로 갱신 + 디렉토리 스캐폴딩

**리뷰어:** Claude Opus 4.6 (Senior Code Reviewer)
**일자:** 2026-03-07
**이전 리뷰:** `apex_docs/review/20260307_193245_docs-path-update-and-scaffolding.md`
**수정 커밋:** `97235cc` (fix: 리뷰 피드백 반영 -- I-1,I-2,S-1,S-2,S-3 전건 수정)
**Base SHA:** `5af8b2f`
**Head SHA:** `97235cc`

---

## 총평

이전 리뷰의 Important 2건 + Suggestion 3건이 모두 수정 시도되었으며, 대부분 올바르게 반영되었다. 특히 I-1(후행 슬래시 없는 경로 미치환)의 수정이 가장 복잡했는데, 코드 블록 전체를 현재 디렉토리 구조에 맞게 재작성하여 일관성이 크게 개선되었다. I-2의 역할 주석 추가, S-2의 k8s/log-svc 추가, S-3의 커밋 해시 보완도 깔끔하다.

다만 S-1(auth-svc 네임스페이스 통일) 수정 과정에서 설계 문서 트리의 들여쓰기가 하나 깨진 부분이 발견되었다. 기능에 영향은 없지만 설계 문서의 정확성 차원에서 수정이 필요하다.

---

## 이전 이슈 수정 검증

### I-1: 후행 슬래시 없는 패턴 미치환 -- PASS

**파일:** `apex_core/docs/plans/20260306_154038_v0.1.0_phase-1-2-setup-and-foundations.md`

**검증 결과:**

| 원래 지적 (line) | 수정 전 | 수정 후 | 판정 |
|---|---|---|---|
| 35 | `mkdir -p core/src` | `mkdir -p src` | OK |
| 39 | `mkdir -p core/examples` | `mkdir -p examples` | OK |
| 53 | `find core -type d \| sort` | `find . -type d \| sort` | OK |
| 58-64 | `core`, `core/examples` 등 7개 | `.`, `./docs`, `./examples` 등 11개로 완전 재작성 | OK |
| 213 | `add_subdirectory(core)` | `add_subdirectory(apex_core)` | OK |
| 283 | `git add ... core/ docs/` | `git add ... include/ src/ tests/ examples/ docs/` | OK |

`grep 'core/(src|examples|include|tests)'` 결과 0건으로 잔존 미치환 경로 없음. 코드 블록 내부 일관성도 모두 복원되었다. `find` 명령어의 Expected 출력도 현재 구조(`./` prefix 포함)에 맞게 전면 갱신되어 블록 전체가 자연스럽다.

**참고:** line 7의 산문 "모노레포 구조의 core/ 디렉토리에"는 잔존하고 있으나, 이것은 프로젝트 소개 산문이라 코드 경로 치환 범위 밖이다. 역사적 맥락 보존 관점에서 현 상태 유지가 적절하다.

---

### I-2: CMakeLists.txt 중복 항목 모호성 -- PASS

**파일:** `apex_core/docs/plans/20260306_154038_v0.1.0_phase-1-2-setup-and-foundations.md` (line 191-192)

수정 전:
```
- Create: `CMakeLists.txt` (최상위)
- Create: `CMakeLists.txt`
```

수정 후:
```
- Create: `CMakeLists.txt` (최상위, 루트 오케스트레이션)
- Create: `CMakeLists.txt` (apex_core 라이브러리 빌드)
```

역할 주석이 추가되어 두 파일의 구분이 명확해졌다. 어떤 CMakeLists.txt가 어떤 용도인지 한눈에 파악된다.

---

### S-1: auth-svc 네임스페이스 통일 -- PARTIAL (새 이슈 발생)

**파일:** `apex_docs/plans/20260307_174528_directory-structure-design.md` (line 55, 62, 65)

네임스페이스 갱신 자체는 올바르게 수행되었다:
- `include/apex/auth/` -> `include/apex/auth_svc/` (OK)
- `include/apex/chat_svc/` 라인 신규 추가 (OK)
- `include/apex/log_svc/` 라인 신규 추가 (OK)

그러나 **auth-svc의 include 라인(line 55)에 들여쓰기 오류**가 발생했다:

```
# gateway (line 48) -- 올바른 2단계 들여쓰기
|   |   +-- include/apex/gateway/

# auth-svc (line 55) -- 잘못된 3단계 들여쓰기
|   |   |   +-- include/apex/auth_svc/

# chat-svc (line 62) -- 올바른 2단계 들여쓰기
|   |   +-- include/apex/chat_svc/
```

auth-svc만 `|   |   |   +--` (pipe 3개)로 되어 있고, gateway/chat-svc는 `|   |   +--` (pipe 2개)다. diff를 보면 원본 `|   |   +-- include/apex/auth/`를 `|   |   |   +-- include/apex/auth_svc/`로 수정하면서 pipe가 하나 추가된 것으로 보인다. (아래 새 이슈 N-1로 기록)

---

### S-2: k8s/log-svc/ 설계 문서 추가 -- PASS

**파일:** `apex_docs/plans/20260307_174528_directory-structure-design.md` (line 85)

```
|       +-- gateway/
|       +-- auth-svc/
|       +-- chat-svc/
|       +-- log-svc/       <-- 추가됨
```

실제 스캐폴딩(`apex_infra/k8s/log-svc/`)과 정확히 일치. 들여쓰기도 형제 항목들과 동일하다.

---

### S-3: 완료 보고서 커밋 해시 보완 -- PASS

**파일:** `apex_docs/progress/20260307_192555_docs-path-update-and-scaffolding.md` (line 4)

수정 전:
```
**커밋:** `720b27d`, `4e7222c`
```

수정 후:
```
**커밋:** `720b27d` (경로 갱신), `4e7222c` (스캐폴딩), `5eb0e36` (계획서+완료보고서), `5af8b2f` (리뷰 보고서)
```

전체 커밋이 역할 설명과 함께 나열되어 추적이 훨씬 용이하다. 단순히 해시를 추가하는 것을 넘어서 각 커밋의 역할까지 병기한 것은 좋은 판단이다.

---

## 새로 발견된 이슈

| # | 등급 | 분류 | 요약 |
|---|------|------|------|
| N-1 | Suggestion | 설계 문서 | auth-svc include 라인 들여쓰기 오류 |

---

### N-1: auth-svc include 라인 트리 들여쓰기 오류 [Suggestion]

**파일:** `apex_docs/plans/20260307_174528_directory-structure-design.md` (line 55)

**현재 (잘못됨):**
```
|   +-- auth-svc/                       <- 인증 서비스
|   |   |   +-- include/apex/auth_svc/
|   |   +-- src/
```

**올바른 형태:**
```
|   +-- auth-svc/                       <- 인증 서비스
|   |   +-- include/apex/auth_svc/
|   |   +-- src/
```

gateway(line 48)와 chat-svc(line 62) 모두 `|   |   +--` 패턴인데, auth-svc만 `|   |   |   +--`로 pipe가 하나 더 들어가 있다. S-1 수정 시 `auth/`를 `auth_svc/`로 치환하면서 의도치 않게 `|` 하나가 추가된 것으로 추정된다.

기능 영향은 전혀 없으나, 설계 문서의 트리가 시각적으로 auth-svc의 include가 하위 디렉토리처럼 보이게 되어 구조 파악에 혼란을 줄 수 있다.

**권장 조치:** line 55의 `|   |   |   +--`를 `|   |   +--`로 수정 (pipe 하나 제거).

---

## 설계 문서 vs 실제 스캐폴딩 일치 검증

| 설계 문서 항목 | 실제 디렉토리 | 일치 |
|---|---|---|
| `apex_services/gateway/include/apex/gateway/` | 존재 | OK |
| `apex_services/auth-svc/include/apex/auth_svc/` | 존재 | OK |
| `apex_services/chat-svc/include/apex/chat_svc/` | 존재 | OK |
| `apex_services/log-svc/include/apex/log_svc/` | 존재 | OK |
| `apex_shared/schemas/` | 존재 | OK |
| `apex_shared/lib/include/apex/shared/` | 존재 | OK |
| `apex_infra/k8s/gateway/` | 존재 | OK |
| `apex_infra/k8s/auth-svc/` | 존재 | OK |
| `apex_infra/k8s/chat-svc/` | 존재 | OK |
| `apex_infra/k8s/log-svc/` | 존재 | OK |
| `apex_tools/` | 존재 (빈 디렉토리 + README) | OK |

N-1의 들여쓰기 오류를 제외하면, 설계 문서의 디렉토리 구조와 실제 스캐폴딩이 완전히 일치한다.

---

## 잘한 점

1. **I-1 수정의 품질이 높다.** 단순 문자열 치환이 아닌, `find` 명령어의 Expected 출력 블록을 현재 구조에 맞게 전면 재작성했다. `core` -> `.` 치환, `./` prefix 추가, 누락된 `./docs` 등 새 디렉토리 반영까지 세심하게 처리했다.
2. **I-2의 역할 주석이 적절하다.** "루트 오케스트레이션" / "apex_core 라이브러리 빌드"라는 설명이 정확하고 간결하다.
3. **S-3에서 단순 해시 추가를 넘어 역할 설명을 병기한 것이 좋다.** 향후 히스토리 추적 시 유용하다.
4. **커밋 메시지가 명확하다.** "I-1,I-2,S-1,S-2,S-3 전건 수정"으로 어떤 이슈를 수정했는지 한눈에 파악 가능하다.

---

## 판정

| 등급 | 이전 리뷰 | 이번 리뷰 |
|------|-----------|-----------|
| Critical | 0 | 0 |
| Important | 2 | 0 |
| Suggestion | 3 | 1 (신규 N-1) |

이전 리뷰의 Important 2건이 모두 해소되었고, Suggestion 3건도 올바르게 수정되었다. 수정 과정에서 발생한 신규 이슈는 Suggestion 등급 1건(트리 들여쓰기)뿐이다.

**머지 판단: 머지 가능**

N-1은 설계 문서 트리의 시각적 일관성 문제로, 기능 영향이 전혀 없다. 머지 후 후속 패치로 처리하거나, 머지 전에 1줄 수정으로 해결할 수 있다.
