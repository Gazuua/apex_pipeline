# Auto-Review 개선 계획

**작성일**: 2026-03-14
**적용 시점**: auto-review full 실행 전 (v0.4.5 마무리 단계)
**관련 논의**: v0.4.5 auto-review task 실행 중 도출

---

## 1. 컨텍스트 전달 구조 개선

### 문제
- 리뷰어 5개 결과가 메인 컨텍스트에 누적 → 컴팩션 조기 발생 → 정보 손실
- 수정 에이전트에 이슈 전달 시에도 메인 컨텍스트 소비

### 해결: 파일 기반 컨텍스트 전달
- 리뷰어가 **구조화된 JSON**으로 결과 출력 → 자동 dedup + 자동 그룹핑 가능
- 수정 에이전트가 JSON 파일을 직접 읽고 작업
- 메인은 요약만 확인

### 디렉토리 구조

```
apex_tools/auto-review/
├── log/                               ← 실행 이력 (전부 커밋)
│   └── {branch}_{timestamp}/          ← 실행 단위
│       ├── config.json                ← 모드, 대상 파일, 리뷰어 목록
│       ├── round_1/
│       │   ├── findings.json          ← 리뷰어별 발견사항 (구조화 JSON)
│       │   ├── fix_plan.json          ← 파일별 그룹핑된 수정 지시
│       │   └── summary.md            ← 라운드 요약
│       ├── round_2/
│       │   └── ...
│       ├── tracking.json              ← 이슈 추적 맵 (dedup, 반복 감지)
│       └── report.md                  ← 최종 보고서 (전체 과정 포함)
└── templates/                         ← 리뷰어 프롬프트 등 (향후)
```

- **log/ 는 .gitignore**: 라운드별 중간 결과는 임시 파일 — 최종 보고서로 충분히 추적 가능
- **최종 보고서만 커밋**: `docs/{project}/review/`에 최종 report.md 복사하여 커밋

---

## 2. 리뷰어 역할 재설계 (5 → 7)

### 문제
- `reviewer-code`가 버그+보안+성능+설계를 전부 담당 → 깊이 부족
- PgTransaction BEGIN 누락(설계)과 alignment UB(안전성)이 같은 Critical로 분류됨
- 이 프로젝트의 핵심 도메인(메모리 안전성, 코루틴 동시성)에 전문 리뷰어 없음

### 해결: reviewer-code를 4개로 분화

| # | 리뷰어 | 전문 영역 | 기존 대비 |
|---|--------|----------|----------|
| 1 | **reviewer-correctness** | 로직 버그, 제어 흐름, 반환값, 엣지 케이스, UB | code에서 분리 |
| 2 | **reviewer-memory** | C++ 메모리 안전성: 수명, RAII, dangling, 정렬, 할당기 | code에서 분리 (프로젝트 핵심) |
| 3 | **reviewer-concurrency** | 코루틴 프레임 수명, executor 안전, race condition, shared-nothing 위반 | code에서 분리 (프로젝트 핵심) |
| 4 | **reviewer-api** | API 설계 일관성, concept 충족, 네이밍, 오용 방지, 인터페이스 계약 | code+structure에서 분리 |
| 5 | **reviewer-test** | 커버리지, 엣지 케이스, assertion 품질, 테스트 격리 | 유지 |
| 6 | **reviewer-docs** | 문서-코드 정합성, 설계서 반영, README | 유지 |
| 7 | **reviewer-build** | CMake, CI/CD, 의존성, 라이선싱 | general 리네임 |

### 설계 원칙
- **스코프 겹침 허용**: memory와 concurrency가 둘 다 코루틴 프레임 수명을 볼 수 있음 → 신뢰성 향상
- **Critical 판정 기준 강화**: "현재 호출자 존재 여부" 등 실용적 기준 추가
- **JSON 출력 필수**: 모든 리뷰어가 구조화된 findings.json 포맷으로 결과 출력
- **크로스 컴파일러 체크**: GCC/MSVC 차이 검증 필수 (예: `<cstdint>` 명시적 include, `SIZE_MAX` 선언 등 — MSVC는 transitively include되어 빌드되지만 GCC에서 실패하는 패턴). reviewer-build의 필수 체크리스트에 포함

---

## 3. 구현 순서

1. `apex_tools/auto-review/log/` 디렉토리 생성
2. findings.json 스키마 정의
3. 리뷰어 7개 프롬프트 작성 (`apex_tools/auto-review/templates/` 또는 플러그인)
4. 오케스트레이터(auto-review 커맨드) 프롬프트 업데이트
5. auto-review full에서 첫 적용
