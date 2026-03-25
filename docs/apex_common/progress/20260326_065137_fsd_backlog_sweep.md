# FSD 백로그 소탕 완료

- **일시**: 2026-03-26
- **브랜치**: feature/fsd-backlog-20260326_054351
- **PR**: #175

## 해결 (10건)

| 백로그 | 타입 | 스코프 | 제목 |
|--------|------|--------|------|
| BACKLOG-207 | BUG | GATEWAY | Gateway silent failure 에러 피드백 추가 |
| BACKLOG-209 | BUG | GATEWAY | PubSubListener SUBSCRIBE/UNSUBSCRIBE 응답 명시적 소비 |
| BACKLOG-210 | SECURITY | CORE | ServerConfig max_connections 기본값 0→10000 |
| BACKLOG-213 | DESIGN_DEBT | CORE | ServiceRegistry for_each/size map_ 기반 전환 |
| BACKLOG-228 | TEST | CORE | cross_core_call void 반환형 타임아웃 테스트 |
| BACKLOG-229 | TEST | CORE | SharedPayload 혼합 refcount 테스트 |
| BACKLOG-219 | TEST | CORE | MetricsHttpServer malformed HTTP 요청 테스트 |
| BACKLOG-220 | TEST | CORE | TimingWheel 콜백 exception 안정성 테스트 |
| BACKLOG-201 | DESIGN_DEBT | SHARED | AdapterBase close timeout 파라미터화 |
| BACKLOG-225 | DESIGN_DEBT | TOOLS | IPC Client checkVersion sleep 파라미터화 |

## 드롭 (3건)

| 백로그 | 사유 |
|--------|------|
| BACKLOG-142 | CrashHandler — subprocess 기반 크로스플랫폼 설계 필요 |
| BACKLOG-218 | MetricsHttpServer in-flight cancel — flaky race condition |
| BACKLOG-172 | tcp_binary_protocol 참조 정리 — 요구사항 불명확 |

## Auto-review 수정 (2건)

1. handle_unsubscribe_channel 에러 응답 누락 추가
2. register_ref 핵심 계약 테스트 추가
