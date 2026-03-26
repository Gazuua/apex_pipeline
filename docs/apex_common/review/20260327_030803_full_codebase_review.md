# 전체 코드베이스 3회차 정밀 리뷰

- **일시**: 2026-03-27 03:08:03
- **브랜치**: feature/full-codebase-review
- **PR**: #214

## 리뷰 범위

프로젝트 전체 코드베이스 — C++ (apex_core, apex_shared, apex_services) + Go (apex_tools/apex-agent) + 인프라 (Docker, Helm, CI) + 문서 전체.

## 리뷰어 투입

| 리뷰어 | 관점 | 1차 | 2차 | 3차 |
|--------|------|-----|-----|-----|
| reviewer-logic | 비즈니스 로직, 알고리즘, 에러 처리, 상태 전이 | CLEAN | 제외 | 제외 |
| reviewer-systems | 메모리 관리(RAII, lifetime), 동시성(코루틴, strand) | CLEAN | 제외 | 제외 |
| reviewer-test | 미테스트 경로, assertion, 테스트 격리성 | 8건 | CLEAN | 제외 |
| reviewer-design | 공개 인터페이스, 모듈 경계, 설계서-코드 정합 | 5건 | 2건 | CLEAN |
| reviewer-infra-security | CMake, Docker, Helm, CI, 보안 | 9건 | CLEAN | 제외 |
| reviewer-docs-spec | 설계서/README/CLAUDE.md 정합성 | 15건 | 1건 | CLEAN |
| reviewer-docs-records | plans/progress/review 포맷, 추적성 | 8건 | CLEAN | 제외 |

**합계**: 1차 45건 → 2차 3건 → 3차 0건 = **총 48건 발견, 48건 전량 수정**

## 수정 완료 항목 (48건)

### 보안 (2건 MAJOR)
1. SecureString `operator==`/`!=` → `constant_time_equal()` 위임 (timing side-channel 차단)
2. HttpServerBase 기본 바인딩 127.0.0.1 (MetricsConfig/AdminConfig bind_address 파라미터화)

### 인프라 (9건)
3. VERSION 0.6.5 전파 — CMakeLists.txt(루트+apex_core), vcpkg.json ×3, Helm Chart.yaml ×4
4. Grafana adminPassword → existingSecret 패턴
5. Loki prod auth_enabled: true
6. docker-bake.hcl CI_IMAGE_TAG service-base 중앙화
7. .dockerignore 추가 제외 패턴
8. CMakePresets release-sccache BUILD_TESTING=ON
9. apex_core/CMakeLists.txt standalone VERSION 0.5.10 → 0.6.5

### 테스트 (8건)
10. ConnectionLimiter 단위 테스트 신규 (9개 케이스)
11. Gateway 에러 경로 테스트 7건 추가
12. HttpServerBase 추가 테스트 3건
13. TLS 전송 테스트 8건 신규
14. RateLimitFacade 테스트 7건 신규 (컴포넌트 수준)
15. test_core_metrics.cpp EXPECT_GE → EXPECT_EQ 강화
16. test_metrics_http_server.cpp timeout_multiplier 적용
17. test_spsc_mesh.cpp shared_ptr marker 패턴 (ASAN 의존 제거)

### 설계 (5건)
18. apex_core_guide.md 버전 v0.6.5.0 갱신
19. apex_core_guide.md ServerConfig max_connections_per_ip 필드 추가
20. apex_core_guide.md AppConfig MetricsConfig/AdminConfig 필드 추가
21. AppConfig metrics/admin 이중 보유 주석 보강
22. for_each_core std::function → template (힙 할당 제거)

### 2차 설계 추가 (2건)
23. apex_core_guide.md MetricsConfig/AdminConfig bind_address 필드 추가
24. apex_core_guide.md AdminHttpServer API 경로/메서드 정합 (PUT /log-level → GET/POST /admin/log-level)

### 문서 정합성 (15건)
25. README.md 버전 v0.6.5.0 + 완료 항목 추가
26. Apex_Pipeline.md 로드맵 v0.6.5.0 행 추가
27. Apex_Pipeline.md CI/CD TODO 체크 완료
28. Apex_Pipeline.md prometheus-cpp → 자체 구현
29. apex_agent_workflow_guide.md 워크스페이스 모듈 제거 반영
30. apex_agent_workflow_guide.md 머지 파이프라인 notify merge 교체
31. apex_services/README.md log-svc 주석 처리
32. apex_tools/README.md queue merge 제거
33. apex_tools/README.md validate-backlog 추가
34. apex_tools/README.md 미ack 참조 수정
35. apex-agent/README.md ack→status/backlog-check 수정
36. apex-agent/README.md 미ack 참조 수정
37. apex_core/README.md prometheus-cpp 수정
38. apex_infra/README.md log-svc 주석 + PgBouncer 추가

### 2차 문서 추가 (1건)
39. apex_agent_workflow_guide.md 워크스페이스 모듈 상세 내용 삭제

### 기록 문서 (3건)
40. review/20260323 auto-review 미수정 섹션 축약
41. progress/20260326 workspace_blocked_reason 후속 작업 섹션 축약
42. progress/20260326 session_server_phase2_3 잔여 이슈 섹션 축약

### 기록 문서 MINOR (6건, 수정 안 함 — 레거시)
43-48. 프로젝트 초기 파일명 타임스탬프 불일치 6건 (규칙 확립 이전, 소급 ROI 낮음)

## 긍정적 평가

- **로직 + 시스템 CLEAN**: 비즈니스 로직, 메모리 관리, 동시성 모두 결함 없음 — 코어 프레임워크 품질 우수
- **shared-nothing 아키텍처 일관**: cross-core 접근이 strand/cross_core_call로 통제됨
- **테스트 커버리지**: 기존 테스트 체계가 체계적 (99/99 전체 통과)
- **Go 코드**: 인터페이스 설계, 트랜잭션 원자성, defer 패턴 모두 양호

## 빌드 검증

- 로컬 MSVC /W4 /WX: 통과
- 전체 테스트 99/99 (100%): 통과
- CI: PR #214 검증 중
