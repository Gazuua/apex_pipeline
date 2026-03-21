# BACKLOG-52: 디버깅/운영 흐름 로깅 대폭 추가 — 완료

## 결과 요약

ServiceBase 로깅 헬퍼와 standalone 헬퍼를 도입하고, 코어 핫패스·어댑터·서비스 전반에 debug/trace 로그를 배치하여 운영 서버 디버깅 기반을 확보했다.

## 변경 내역

### 헬퍼 인프라
- `apex_core/include/apex/core/log_helpers.hpp` (신규): standalone 로깅 헬퍼 5레벨
- `apex_core/include/apex/core/service_base.hpp`: ServiceBase 매크로 5레벨 × 3오버로드 = 15개 메서드

### 로그 배치 (18 files, +327/-18)
- **코어**: core_engine, session_manager, spsc_mesh, server, message_dispatcher, core_engine.hpp
- **어댑터**: Redis multiplexer, PG connection/pool, Kafka producer/consumer
- **서비스**: Gateway pipeline, Auth service, Chat service

## 설계 결정
- Named logger 탈락: MSA 프로세스 분리로 per-service 레벨 제어 이미 가능
- MDC/TLS 탈락: 헬퍼가 동일한 컨텍스트 자동 주입 달성

## auto-review 결과
- CRITICAL 1건 수정 (Redis 명령어 전체 노출 → verb만 출력)
- MAJOR 1건 수정 (core_id_for_log_ → core_id_for_log 네이밍 컨벤션)
- MINOR 1건 수정 (삭제된 주석 복원)
