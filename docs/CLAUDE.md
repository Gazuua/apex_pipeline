# docs — 문서 작성 & 리뷰 가이드

## 문서 규칙

> 루트 `CLAUDE.md` § 문서/프로세스 규칙은 파일명·백로그 포인터만 보유. 경로·작성·리뷰·백로그 운영 규칙의 단일 권위 출처는 이 파일.

- **필수 작성**: 계획서(`plans/`), 완료 기록(`progress/`), 리뷰 보고서(`review/`)
- **작성 타이밍**: plans → 구현 전, review → 리뷰 완료 후, progress → CI 통과 후 merge 전
- **문서 위치**: 프로젝트 전용 → `docs/<project>/`, 공통 → `docs/apex_common/`, 걸치는 문서 → 양쪽에 관점 조정하여 작성 (단순 복사 금지)
- **progress 문서**: 작업 결과 요약 필수 — 빈 껍데기 금지

- 리뷰·progress 문서에서 발견된 TODO/백로그 → `backlog add` CLI로 등록
- 원본 문서(review/progress)에 TODO·백로그·향후 과제 섹션 잔류 금지 — 발견 즉시 `backlog add`로 이전 후 원본에서 제거

## 코드 리뷰

- review 문서 생성 시 반드시 리뷰 항목 상세 포함 — 헤더/통계만 있는 빈 껍데기 금지. 상세 내용 없이 생성된 review 파일은 auto-review에서 Critical로 플래그
- **clangd LSP + superpowers:code-reviewer 병행** — LSP 정적 분석(타입/참조/호출 추적)과 AI 코드 리뷰를 함께 사용해야 품질이 높아진다
- **clangd LSP 효율 전략**: `documentSymbol` 병렬 → 핵심 API `hover` → 의심 패턴 `findReferences`/`incomingCalls`. 전수 분석 금지, 10분 타임아웃.
- **설계 문서 정합성**: 아키텍처 영향 변경 시 `Apex_Pipeline.md` 일치 확인 필수

## 브레인스토밍

- 브레인스토밍 시 `docs/Apex_Pipeline.md`, `backlog list` 사전 확인 필수

## 백로그 운영

> **이 섹션이 백로그 운영의 단일 권위 출처.** 루트 `CLAUDE.md`는 포인터만 보유.

### 2축 분류 체계

**시간축 (1차 분류)** — 문서 내 섹션 순서: `## NOW` → `## IN VIEW` → `## DEFERRED`

| 등급 | 판단 질문 | 기준 |
|------|-----------|------|
| **NOW** | 이번 마일스톤에서 안 하면 다음 단계 진행이 막히는가? 또는 ROI가 극단적으로 높은가? | 현재 마일스톤의 선행 조건/블로커. 이미 착수 결정된 항목. 또는 작업 소규모 + 이후 전체 작업에 반복 효과가 있는 경우 |
| **IN VIEW** | 다음 1-2 마일스톤 내에 필요해질 게 확실한가? | 가까운 미래에 필요. 구체적 트리거 조건 있음 |
| **DEFERRED** | 위 둘 다 아닌 경우 | 외부 조건 충족 시 재평가 |

승격 규칙: 마일스톤 전환 시 IN VIEW 재평가 → NOW 승격 여부 판단. DEFERRED는 트리거 조건 발생 시에만 재평가.

**내용축 (2차 분류)** — 각 항목의 `등급` 필드

| 등급 | 판단 질문 | 기준 |
|------|-----------|------|
| **CRITICAL** | 안 고치면 시스템이 깨지거나 데이터가 위험한가? | 크래시, 데이터 손실/손상, 보안 취약점 |
| **MAJOR** | 안 고치면 이후 작업 비용이 눈에 띄게 늘어나는가? | 설계 부채, 테스트 부재, 문서 부재, 확장성 병목 |
| **MINOR** | 위 둘 다 아닌 경우 | 기능·안정성에 영향 없음. 코드 위생, 오타, 탐색적 최적화 |

경계 케이스: 고민되면 높은 쪽으로.

### 항목 템플릿

```
### #{ID}. 이슈 제목
- **등급**: CRITICAL | MAJOR | MINOR
- **스코프**: {모듈 태그, 복수 가능}
- **타입**: bug | design-debt | test | docs | perf | security | infra
- **연관**: #{ID}, #{ID}  (선택 — 관련 항목이 있을 때만 기재)
- **설명**: 이슈 상세.
```

**연관 필드 규칙**: 다방향 링킹 필수 — A가 B를 참조하면 B도 A를 참조. 3개 이상 그룹이면 모든 멤버가 나머지 전원을 참조.

**스코프 태그**: `CORE | SHARED | GATEWAY | AUTH_SVC | CHAT_SVC | INFRA | CI | DOCS | TOOLS` (서비스 추가 시 확장)
**타입 태그**: `BUG | DESIGN_DEBT | TEST | DOCS | PERF | SECURITY | INFRA`

### 항목 ID

- Auto-increment 정수. 자릿수 패딩 없음 (`#1`, `#12`, `#100`)
- 한 번 발번된 ID는 재사용하지 않음. ID 갭 허용
- `다음 발번` 카운터는 DB(SQLite)가 관리 — `docs/BACKLOG.json`의 `next_id` 필드는 export 시 자동 반영
- ID 충돌 시: DB의 AUTOINCREMENT가 자동 처리

### 섹션 내 우선순위

각 섹션(NOW/IN VIEW/DEFERRED) 안에서 **위에 있는 항목이 먼저 착수**. 배치 기준 (위에서 아래 순):

1. **등급 우선**: CRITICAL > MAJOR > MINOR (동일 등급 내에서 아래 기준 적용)
2. **코드 위생 선행**: 포맷팅·경고 소탕 등 전체 코드에 영향 주는 작업은 코드량 증가 전에 처리
3. **파이프라인**: CI 커버리지 갭은 코드 변경 전에 확보 — 변경 후 검증 불가 방지
4. **보안**: 배포 전제 조건. 연관 항목끼리 묶어서 일괄 처리
5. **운영 가시성**: 로깅·모니터링·크래시 진단 — 이후 디버깅 비용 절감
6. **설계·프로세스**: 가드레일, 자동화 — 반복 실수 방지
7. **테스트 갭**: 기능 자체는 동작하므로 후순위
8. **기타**: 벤치마크, 문서, DevX — 기능·안정성 영향 없는 항목 최하단

동일 계층 내에서는 ROI(작업 크기 대비 효과) 높은 항목을 상단에 배치. 연관 항목(#A↔#B)은 인접 배치하여 일괄 착수 유도.

### 접근 규칙

BACKLOG 항목을 추가·수정·착수·리뷰할 때, 반드시 실제 구현 상태(코드베이스, git 이력, 테스트 결과 등)를 검증하여 사실관계를 확인한다. 문서에 적힌 상태를 그대로 신뢰하지 않는다.

### 히스토리 운영

- 완료된 항목: `backlog resolve ID --resolution FIXED` → DB에서 RESOLVED 처리
- `backlog export` 시 RESOLVED 항목도 `docs/BACKLOG.json`에 포함 (단일 파일)
- 해결 방식: FIXED | DOCUMENTED | WONTFIX | DUPLICATE | SUPERSEDED
- **별도 히스토리 파일 없음** — 단일 `BACKLOG.json`에 전체 이력 보존

### 파일 접근 정책

- `docs/BACKLOG.json` (및 레거시 `BACKLOG.md`, `BACKLOG_HISTORY.md`): **Read/Edit/Write 모두 차단** (validate-backlog hook)
- 조회: `backlog list`, `backlog show <ID>`
- 수정: `backlog add/update/resolve/release/export`
