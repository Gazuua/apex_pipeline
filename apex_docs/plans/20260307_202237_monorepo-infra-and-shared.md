# 모노레포 인프라 및 공유 레이어 구축 계획

**작성일**: 2026-03-07
**범위**: apex_core 외 전체 (루트, apex_shared, apex_infra, apex_tools, apex_services)
**제약**: apex_core/ 디렉토리 내부 파일은 수정하지 않음 (별도 에이전트 담당)

---

## 1. ~~루트 빌드 메타데이터 버전 동기화~~ ✅ 완료

커밋: `8c64692` — CMakeLists.txt, vcpkg.json 버전 0.2.2 → 0.2.3

---

## 2. ~~설계 문서 현행화 (Apex_Pipeline.md)~~ ✅ 완료

커밋: `8c64692` — Phase 4.6-r 완료, v0.2.3 체크박스, Phase infra 추가, docker-compose 프로파일 정정

---

## 3. apex_infra — docker-compose.yml 작성

**목적**: 로컬 개발 환경에서 외부 의존성(Kafka, Redis, PostgreSQL)을 한 번에 기동

**프로파일 구조** (설계 문서 §9 기준):

| 프로파일 | 서비스 |
|----------|--------|
| `minimal` | Kafka (KRaft), Redis, PostgreSQL |
| `observability` | minimal + Prometheus + Grafana |
| `full` | observability + 서비스 컨테이너 전체 (향후) |

**주요 결정 사항**:
- Kafka: KRaft 모드 (ZooKeeper 제거), 단일 브로커
- Redis: 단일 노드 (로컬 개발용, 클러스터 불필요)
- PostgreSQL: 16+, schema-per-service 초기화 스크립트 포함
- 네트워크: `apex-net` 브릿지 네트워크로 서비스 간 통신
- 볼륨: 명명된 볼륨으로 데이터 영속화

**산출물**:
- `apex_infra/docker-compose.yml`
- `apex_infra/postgres/init.sql` (서비스별 스키마 초기화)
- `apex_infra/README.md` 갱신 (사용법 추가)

---

## 4. apex_shared — FlatBuffers 공유 메시지 스키마 정의

**목적**: 서비스 간 Kafka를 통한 통신에 사용할 공유 메시지 타입 정의

**apex_core 내장 스키마와의 차이**:
- apex_core/schemas/: 프레임워크 레벨 메시지 (echo, error_response 등)
- apex_shared/schemas/: 서비스 레벨 메시지 (인증, 채팅 등 비즈니스 도메인)

**초기 스키마 후보**:

| 도메인 | 파일 | 주요 메시지 |
|--------|------|------------|
| 공통 | `common.fbs` | Envelope (trace_id, timestamp, source_service) |
| 인증 | `auth.fbs` | AuthRequest, AuthResponse, TokenValidation |
| 채팅 | `chat.fbs` | ChatMessage, RoomEvent, JoinLeave |
| 로그 | `log.fbs` | LogEntry (구조화 로그) |

**산출물**:
- `apex_shared/schemas/*.fbs`
- `apex_shared/CMakeLists.txt` (flatc 코드젠 타겟)
- `apex_shared/README.md` 갱신

---

## 5. apex_tools — 서비스 스캐폴딩 스크립트

**목적**: 새 서비스 디렉토리를 규격에 맞게 자동 생성

**생성 구조**:
```
apex_services/<name>/
├── include/apex/<name>/
├── src/
├── tests/
├── CMakeLists.txt        ← apex_core 링크, flatc 의존
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

**각 서비스 공통 산출물**:
- `CMakeLists.txt` (apex_core 링크, apex_shared 의존)
- `vcpkg.json` (서비스별 추가 의존성)
- `Dockerfile` (멀티스테이지 빌드)

**참고**: 실제 비즈니스 로직 구현은 이 계획 범위 밖 (v0.3.0+ 이후)

---

## 작업 순서 (권장)

```
[1] 버전 동기화 ──→ [2] 문서 현행화 ──→ [3] docker-compose
                                            │
                                            ├──→ [4] 공유 스키마
                                            │
                                            └──→ [5] 스캐폴딩 스크립트 ──→ [6] 서비스 빌드 인프라
```

- 1, 2번은 즉시 완료 가능 (선행 의존성 없음)
- 3번은 이후 서비스 개발의 기반
- 4, 5번은 3번 이후 병렬 진행 가능
- 6번은 5번(스캐폴딩)의 결과물을 활용하거나 수동 작성

---

## 충돌 방지 규칙

apex_core 에이전트와의 파일 충돌을 방지하기 위해:

- **수정 금지**: `apex_core/` 하위 모든 파일
- **수정 가능**: 루트 `CMakeLists.txt`, `vcpkg.json`, `apex_docs/`, `apex_shared/`, `apex_infra/`, `apex_tools/`, `apex_services/`
- 루트 `CMakeLists.txt`의 `add_subdirectory(apex_core)` 라인은 변경하지 않음
