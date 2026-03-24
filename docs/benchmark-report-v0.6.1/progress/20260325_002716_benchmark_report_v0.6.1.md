# v0.6.1 벤치마크 보고서 작성 + 디렉토리 구조 재편

## 작업 요약

i7-14700(20C/28T) 환경에서 수집된 v0.6.1 벤치마크 데이터를 기반으로 전체 보고서를 재작성하고,
벤치마크 디렉토리를 버전/하드웨어 2차원 구조로 재편했다.

## 산출물

### 보고서
- `docs/apex_core/benchmark/v0.6.1.0/i7-14700-20C28T/benchmark_report.html` — 11섹션, Plotly.js 차트 8개
  - 아키텍처 비교(Per-Core vs Shared vs Lock-Free) 독립 섹션으로 승격
  - 해설 전면 재작성: Shared-nothing 아키텍처 우수성 중심 톤

### 디렉토리 구조 재편
- `v0.5.10.0/i5-9300H-4C8T/` — 기존 데이터+보고서+차트 이동 (보존)
- `v0.6.1.0/i7-14700-20C28T/` — 신규 데이터 15종 + 보고서
- 동일 하드웨어 기준 버전 간 비교를 위한 2차원(버전/하드웨어) 분류 체계

### README + 차트
- 아키텍처 테이블: 1~28워커 수치 교체 (Per-Core 피크 7.65M, 최대 5.3x)
- `architecture_scaling.png`: v0.6.1 데이터 기반 차트 재생성

### 백로그
- BACKLOG-40: MINOR→MAJOR 승격, i7-14700 벤치마크 근거 상세 보충
- BACKLOG-114: RESOLVED (프로덕션급 벤치마크 목적 달성)
- BACKLOG-200: 신규 등록 (intrusive_ptr 멀티스레드 경합 벤치마크)

## 핵심 수치

| 항목 | 수치 |
|------|------|
| Per-Core 피크 (8워커) | 7.65M msg/s |
| Per-Core/Shared 최대 배수 (28워커) | 5.29x |
| SPSC 레이턴시 | 1.89ns (531M ops/s) |
| FlatBuffers Read 4KB | 870 GB/s |
| Frame Pipeline E2E | 2.8μs |
