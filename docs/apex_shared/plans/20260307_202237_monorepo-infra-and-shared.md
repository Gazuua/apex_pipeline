# 모노레포 공유 레이어 구축 계획 — 공유 라이브러리 관점

**작성일**: 2026-03-07
**범위**: apex_shared (FlatBuffers 공유 스키마 + 공유 C++ 라이브러리)
**관련 문서**: `docs/apex_infra/plans/20260307_202237_monorepo-infra-and-shared.md` (인프라 관점)

---

## 1. ~~루트 빌드 메타데이터 버전 동기화~~ ✅ 완료

커밋: `8c64692` — CMakeLists.txt, vcpkg.json 버전 0.2.2 → 0.2.3

---

## 2. ~~설계 문서 현행화 (Apex_Pipeline.md)~~ ✅ 완료

커밋: `8c64692` — apex_shared 구조 및 역할 명확화

---

## 3. apex_shared — FlatBuffers 공유 메시지 스키마 정의 (핵심 산출물)

**목적**: 서비스 간 Kafka를 통한 통신에 사용할 공유 메시지 타입 정의

**apex_core 내장 스키마와의 차이**:
- `apex_core/schemas/`: 프레임워크 레벨 메시지 (echo, error_response 등 — 와이어 프로토콜용)
- `apex_shared/schemas/`: 서비스 레벨 메시지 (인증, 채팅 등 비즈니스 도메인 — Kafka 메시지용)

**설계 원칙**:
- 모든 Kafka 메시지는 공통 Envelope으로 래핑 (trace_id, timestamp, source_service)
- FlatBuffers zero-copy 특성을 활용하여 직렬화/역직렬화 비용 제로 유지
- 하위 호환성: 새 필드 추가 시 기본값으로 자동 호환 (FlatBuffers 특성)

**초기 스키마 후보**:

| 도메인 | 파일 | 주요 메시지 | 용도 |
|--------|------|------------|------|
| 공통 | `common.fbs` | Envelope (trace_id, timestamp, source_service) | 모든 Kafka 메시지의 공통 래퍼 |
| 인증 | `auth.fbs` | AuthRequest, AuthResponse, TokenValidation | Gateway ↔ Auth 서비스 통신 |
| 채팅 | `chat.fbs` | ChatMessage, RoomEvent, JoinLeave | 채팅 서비스 도메인 메시지 |
| 로그 | `log.fbs` | LogEntry (구조화 로그) | KafkaSink → Log 서비스 |

**산출물**:
- `apex_shared/schemas/*.fbs` — FlatBuffers IDL 파일
- `apex_shared/CMakeLists.txt` — flatc 코드젠 타겟 (자동 헤더 생성)
- `apex_shared/lib/` — 공유 C++ 유틸리티 (Envelope 헬퍼 등)
- `apex_shared/README.md` — 스키마 규칙 및 사용법

---

## 4. 공유 라이브러리 (apex::shared namespace)

**목적**: 서비스 간 공통으로 사용하는 C++ 코드 제공

**초기 라이브러리 후보**:
- Envelope 빌더/파서 유틸리티
- trace_id 생성기 (UUID v7 기반)
- 공통 에러 코드 정의

**apex_core와의 경계**:
- apex_core: 프레임워크 코어 (네트워크, 코루틴, 메모리 관리)
- apex_shared: 비즈니스 도메인 공통 코드 (메시지 타입, 유틸리티)
- 서비스는 apex_core를 링크하고, apex_shared를 의존으로 추가

---

## 5. apex_tools — 서비스 스캐폴딩 스크립트

**목적**: 새 서비스 디렉토리를 규격에 맞게 자동 생성 (apex_shared 의존 포함)

**생성 구조**:
```
apex_services/<name>/
├── include/apex/<name>/
├── src/
├── tests/
├── CMakeLists.txt        ← apex_core 링크, apex_shared 의존, flatc 코드젠
├── vcpkg.json            ← 서비스별 독립 의존성
├── Dockerfile            ← 멀티스테이지 빌드
└── README.md
```

**산출물**:
- `apex_tools/new-service.sh`
- `apex_tools/README.md` 갱신

---

## 6. apex_services — 서비스별 빌드 인프라

**목적**: 각 서비스의 독립 빌드 + Docker 이미지 구성

**대상**: gateway, auth-svc, chat-svc, log-svc

**각 서비스에서의 apex_shared 사용**:
- `CMakeLists.txt`에서 `target_link_libraries(... apex_shared)` 추가
- FlatBuffers 생성 헤더를 include하여 타입 안전한 Kafka 메시지 송수신
- 서비스별 vcpkg.json에는 추가 의존성만 선언 (apex_shared 의존은 CMake에서 해결)

**참고**: 실제 비즈니스 로직 구현은 이 계획 범위 밖 (v0.3.0+ 이후)

---

## 작업 순서 (권장)

```
[1] 버전 동기화 ──→ [2] 문서 현행화 ──→ [4] 공유 스키마 정의
                                            │
                                            └──→ [5] 스캐폴딩 스크립트 ──→ [6] 서비스 빌드 인프라
```

- 인프라(docker-compose) 작업은 `docs/apex_infra/plans/` 참조
- 공유 스키마(4)는 인프라와 병렬 진행 가능
- 서비스 빌드 인프라(6)는 스키마 + 스캐폴딩 완료 후 진행

---

## 충돌 방지 규칙

apex_core 에이전트와의 파일 충돌을 방지하기 위해:

- **수정 금지**: `apex_core/` 하위 모든 파일
- **수정 가능**: `apex_shared/`, `apex_tools/new-service.sh`, `apex_services/`
- 루트 `CMakeLists.txt`의 `add_subdirectory(apex_core)` 라인은 변경하지 않음
