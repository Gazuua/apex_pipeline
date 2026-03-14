# 코드 리뷰: 문서 경로 갱신 + 디렉토리 스캐폴딩

**리뷰어:** Claude Opus 4.6 (Senior Code Reviewer)
**일자:** 2026-03-07
**Base SHA:** `4e0d145`
**Head SHA:** `5eb0e36`
**커밋:** `720b27d` (경로 갱신), `4e7222c` (스캐폴딩), `5eb0e36` (계획서+완료보고서)
**계획서:** `docs/apex_common/plans/20260307_192618_docs-path-update-and-scaffolding.md`

---

## 총평

전체적으로 계획서에 따라 잘 수행된 작업이다. 22개 파일 534건의 경로 치환이 일괄 처리되었고, 4개 루트 디렉토리 스캐폴딩도 설계 문서 기준에 맞게 생성되었다. 커밋 분리(경로 갱신 / 스캐폴딩 / 문서)도 깔끔하고, 설계 문서의 디렉토리 트리와 CMake 참조 경로 수동 갱신도 정확하다.

다만, sed 치환 규칙이 **후행 슬래시(`/`)를 필수로 요구하는 패턴**이었기 때문에, 줄 끝에 슬래시 없이 끝나는 경로(`core/src`, `core/examples` 등)가 치환되지 않고 잔존하는 케이스가 있다. 또한 설계 문서와 실제 스캐폴딩 사이에 2건의 불일치가 확인된다.

---

## 이슈 목록

| # | 등급 | 분류 | 요약 |
|---|------|------|------|
| 1 | Important | 경로 치환 | 후행 슬래시 없는 패턴 미치환 (1개 파일, 10건+) |
| 2 | Important | 경로 치환 | `core/CMakeLists.txt` 치환 부작용 -- 중복 파일 참조 |
| 3 | Suggestion | 스캐폴딩 | 설계 문서 vs 구현: auth-svc 네임스페이스 불일치 |
| 4 | Suggestion | 스캐폴딩 | 설계 문서 vs 구현: `k8s/log-svc/` 추가 생성 |
| 5 | Suggestion | 완료보고서 | 커밋 해시 불완전 표기 |

---

### I-1: 후행 슬래시 없는 패턴 미치환 [Important]

**파일:** `apex_core/docs/plans/20260306_154038_v0.1.0_phase-1-2-setup-and-foundations.md`

치환 규칙이 `core/src/` -> `src/` 처럼 후행 슬래시를 포함하는 패턴이었기 때문에, 줄 끝에서 슬래시 없이 끝나는 경로가 누락되었다.

**미치환 잔존 목록:**

| 라인 | 내용 | 원인 |
|------|------|------|
| 35 | `mkdir -p core/src` | 후행 슬래시 없음 |
| 39 | `mkdir -p core/examples` | 후행 슬래시 없음 |
| 53 | `find core -type d \| sort` | 치환 규칙 범위 밖 |
| 58 | `core` (단독) | 치환 규칙 범위 밖 |
| 59 | `core/examples` | 후행 슬래시 없음 |
| 60 | `core/include` | 후행 슬래시 없음 |
| 63 | `core/src` | 후행 슬래시 없음 |
| 64 | `core/tests` | 후행 슬래시 없음 |
| 213 | `add_subdirectory(core)` | 치환 규칙 범위 밖 |
| 283 | `git add ... core/ docs/` | 단독 `core/`는 규칙에 없음 |

또한 같은 코드 블록 내에서 일부 줄만 치환되고 일부는 그대로여서, 블록 전체의 일관성이 깨졌다. 예를 들어 line 33-47의 mkdir 블록에서:

```
mkdir -p include/apex/core    # 치환됨 (core/include/apex/core/ 패턴 매칭)
mkdir -p core/src              # 미치환 (core/src -- 슬래시 없음)
mkdir -p tests/unit            # 치환됨 (core/tests/ 패턴 매칭)
mkdir -p core/examples         # 미치환 (core/examples -- 슬래시 없음)
```

**권장 조치:** 해당 파일에서 잔존하는 `core/src`, `core/examples`, `core/include`, `core/tests` (슬래시 없는 변형)를 수동 치환하고, `find core`, `add_subdirectory(core)`, `git add ... core/` 같은 맥락 의존 참조도 함께 정리한다. 과거 계획서라 역사적 기록이지만, 반만 치환된 상태보다는 전체 치환이 일관성 있다.

---

### I-2: `core/CMakeLists.txt` 치환 부작용 [Important]

**파일:** `apex_core/docs/plans/20260306_154038_v0.1.0_phase-1-2-setup-and-foundations.md` (line 190-191)

치환 전:
```
- Create: `CMakeLists.txt` (최상위)
- Create: `core/CMakeLists.txt`
```

치환 후:
```
- Create: `CMakeLists.txt` (최상위)
- Create: `CMakeLists.txt`
```

`core/CMakeLists.txt` -> `CMakeLists.txt` 치환으로 인해 두 항목이 동일해져서 의미가 모호해졌다. 루트 CMakeLists.txt와 apex_core CMakeLists.txt의 구분이 사라진 상태다.

**권장 조치:** 과거 계획서의 역사적 맥락에서 이런 모호함이 발생한다는 점을 인지하되, 수정 시 `apex_core/CMakeLists.txt` 같은 절대 경로 기반 표현으로 명확화하는 것을 권장한다.

---

### S-1: auth-svc 네임스페이스 디렉토리 불일치 [Suggestion]

**설계 문서** (`directory-structure-design.md` line 55):
```
auth-svc/
  +-- include/apex/auth/       <-- "auth"
```

**실제 스캐폴딩:**
```
auth-svc/
  +-- include/apex/auth_svc/   <-- "auth_svc"
```

계획서의 mkdir 스크립트가 `${svc//-/_}` 변환을 사용해서 디렉토리명 `auth-svc`를 네임스페이스 `auth_svc`로 자동 변환한 결과다. 설계 문서 원본에는 `include/apex/auth/`로 되어 있다.

이것이 **문제인지 개선인지는 설계 판단**이 필요하다:
- `auth_svc`: 디렉토리명과 네임스페이스가 1:1 매핑되어 규칙이 명확
- `auth`: 설계 문서 원안. 더 짧고 간결하지만, chat-svc/log-svc에 대해서도 동일 패턴인지 명시되어 있지 않음

**권장 조치:** 설계 결정을 확정하고, 설계 문서 또는 스캐폴딩 중 하나를 다른 쪽에 맞춘다. 개인적으로는 `auth_svc` 방식(디렉토리명과 네임스페이스 1:1 매핑)이 규칙이 명확해서 더 낫다고 본다. 설계 문서를 갱신하는 것을 권장한다.

---

### S-2: `k8s/log-svc/` 설계 문서에 없는 디렉토리 추가 [Suggestion]

**설계 문서** (`directory-structure-design.md` line 79-82):
```
k8s/
  +-- gateway/
  +-- auth-svc/
  +-- chat-svc/
```

**실제 스캐폴딩:**
```
k8s/
  +-- gateway/
  +-- auth-svc/
  +-- chat-svc/
  +-- log-svc/       <-- 설계 문서에 없음
```

계획서(implementation plan)에서 의도적으로 log-svc를 추가했다. apex_services에 log-svc가 있으므로 k8s에도 있는 것이 자연스럽고, 설계 문서의 누락으로 보인다. **이것은 유익한 편차(beneficial deviation)이다.**

**권장 조치:** 설계 문서에 `k8s/log-svc/`를 추가하여 실제 구조와 일치시킨다.

---

### S-3: 완료 보고서 커밋 해시 [Suggestion]

**파일:** `docs/apex_common/progress/20260307_192618_docs-path-update-and-scaffolding.md` (line 4)

```markdown
**커밋:** `720b27d`, `4e7222c`
```

3번째 커밋(`5eb0e36` -- 계획서+완료보고서)이 누락되어 있다. 물론 완료 보고서 자체가 3번째 커밋에 포함되므로 작성 시점에는 해시를 알 수 없었다. 이것은 자연스러운 한계이지만, 커밋 후 보고서를 amend하거나, "계획서/보고서 커밋은 별도"라는 메모를 추가하면 더 명확하다.

---

## 잘한 점

1. **커밋 분리가 깔끔하다.** 경로 치환 / 스캐폴딩 / 문서를 별도 커밋으로 분리해서 revert이나 cherry-pick이 용이하다.
2. **설계 문서 수동 갱신이 정확하다.** `directory-structure-design.md`의 디렉토리 트리 플래트닝, CMake `add_subdirectory` 경로 수정, `bin/` 추가가 현재 실제 구조와 정확히 일치한다.
3. **진행 현황 문서가 잘 관리되고 있다.** 미완료 항목을 취소선 처리하고 완료 설명을 추가한 방식이 깔끔하다.
4. **스캐폴딩 파일 구조가 계획서 예상 결과와 정확히 일치한다.** `find` 출력 비교 시 파일 목록이 완벽히 매칭된다.
5. **README 내용이 설계 문서와 일관된다.** 각 디렉토리의 역할 설명이 설계 문서의 결정 사항과 일치한다.
6. **커밋 메시지가 한국어 컨벤션에 맞고 내용이 명확하다.**

---

## 판정

| 등급 | 건수 |
|------|------|
| Critical | 0 |
| Important | 2 |
| Suggestion | 3 |

**Important 2건은 모두 과거 계획서(`20260306_154038_...`)의 일관성 문제**로, 현재 소스 코드나 활성 설계 문서에는 영향 없다. apex_core 소스 코드 변경이 없으므로 빌드/기능 회귀 위험도 없다.

**머지 판단: 머지 가능 (Important 건은 후속 패치로 처리 권장)**

Important 2건이 과거 계획서의 역사적 기록에만 해당하고 현재 코드/설계에 영향이 없으므로, 이 상태로 머지해도 무방하다. 다만 I-1의 반만 치환된 코드 블록은 나중에 문서를 참조할 때 혼란을 줄 수 있으므로, 가능하면 후속 패치로 정리하는 것을 권장한다.
