# 문서 구조 통합 설계

**날짜**: 2026-03-08
**범위**: docs/ 중앙 집중 문서 관리 구조로 전환 (apex_docs → docs 리네임 포함)

---

## 배경

프로젝트 문서가 두 곳에 분산:
- `docs/` — 프로젝트 공통 문서 (16건)
- `apex_core/docs/` — 코어 프레임워크 문서 (35건)

관리 편의와 일관성을 위해 `docs/`로 통합한다. `apex_` prefix는 코드 프로젝트 컨벤션이므로 문서 폴더는 `docs/`로 간결하게 명명.

## 설계

### 디렉토리 구조

```
docs/
├── Apex_Pipeline.md                         ← 마스터 설계서
├── plans/                                   ← 모노레포 횡단 공통 계획서
├── progress/                                ← 모노레포 횡단 공통 체크포인트
├── review/                                  ← 모노레포 횡단 공통 리뷰
├── apex_core/
│   ├── design-decisions.md                  ← 코어 설계 결정
│   ├── design-rationale.md                  ← 코어 ADR (23개)
│   ├── plans/
│   ├── progress/
│   └── review/
├── apex_infra/
│   ├── plans/
│   ├── progress/
│   └── review/
└── apex_shared/
    ├── plans/
    ├── progress/
    └── review/
```

### 분류 규칙

1. **프로젝트 전용 문서** → `docs/<project>/` 하위
2. **프로젝트 횡단 문서** → `docs/` 루트의 `plans/progress/review/`
3. **여러 프로젝트에 걸치는 문서** → 관련 프로젝트 폴더 **양쪽 모두에 복사**
4. **README.md** → 각 프로젝트 루트에 유지 (apex_docs로 이동하지 않음)
5. 문서가 없는 프로젝트(services, tools)는 폴더를 미리 만들지 않음

### 파일 이동 매핑

#### apex_core/docs/ → docs/apex_core/ (37건)

| 원본 | 대상 |
|------|------|
| `design-decisions.md` | `docs/apex_core/design-decisions.md` |
| `design-rationale.md` | `docs/apex_core/design-rationale.md` |
| `plans/*` (16건) | `docs/apex_core/plans/` |
| `progress/*` (11건) | `docs/apex_core/progress/` |
| `review/*` (9건) | `docs/apex_core/review/` |

#### docs/ 내부 재분류

| 원본 | 대상 | 이유 |
|------|------|------|
| `plans/docker-compose-design` | `apex_infra/plans/` | 인프라 전용 |
| `plans/docker-compose-implementation` | `apex_infra/plans/` | 인프라 전용 |
| `plans/monorepo-infra-and-shared` | `apex_infra/plans/` + `apex_shared/plans/` | 양쪽 걸침 → 복사 |
| `plans/apex-shared-build-infra-design` | `apex_shared/plans/` | shared 전용 |
| `plans/apex-shared-build-infra-implementation` | `apex_shared/plans/` | shared 전용 |
| `progress/apex-shared-build-infra` | `apex_shared/progress/` | shared 전용 |
| `review/v0.2.4_comprehensive-review` | `apex_core/review/` | 코어 전용 |

#### 공통 레벨 유지 (이동 없음)

| 파일 | 이유 |
|------|------|
| `Apex_Pipeline.md` | 마스터 설계서 |
| `plans/directory-structure-design` | 모노레포 전체 |
| `plans/docs-path-update-and-scaffolding` | 모노레포 전체 |
| `plans/docs-consolidation-design` (본 문서) | 모노레포 전체 |
| `progress/monorepo-restructuring` | 모노레포 전체 |
| `progress/docs-path-update-and-scaffolding` | 모노레포 전체 |
| `review/docs-path-update-*` (3건) | 모노레포 전체 |

### 추가 작업

- `apex_docs/` → `docs/` 디렉토리 리네임 (git mv)
- `apex_core/README.md` 신규 생성
- `apex_core/docs/` 디렉토리 삭제 (이동 완료 후)
- 루트 `CLAUDE.md` 문서 경로 업데이트 (apex_docs → docs 전체 반영)
- 메모리 파일 경로 업데이트

### CLAUDE.md 업데이트 내용

- 모노레포 구조에서 `apex_core/docs/` 항목 제거, `apex_docs/` → `docs/` 반영
- `docs/` 하위 구조를 새 구조로 갱신
- 설계 문서 경로: `apex_core/docs/design-decisions.md` → `docs/apex_core/design-decisions.md`
- 모든 `apex_docs` 참조를 `docs`로 변경
