# apex_shared 빌드 인프라 설계

**작성일**: 2026-03-07
**상태**: 승인됨

---

## 목적

서비스 간 공유 FlatBuffers 스키마 + 공유 C++ 코드를 위한 빌드 인프라 세팅.
스키마 내용 정의는 v0.3.0 Kafka 어댑터 구현 시 진행 (현재 범위 밖).

## 배경: 왜 스키마는 지금 안 쓰는가

- Kafka 어댑터(v0.3.0)가 없어 produce/consume 패턴이 미확정
- 어댑터의 envelope 구조(헤더 vs 페이로드 분리)가 스키마 설계에 직접 영향
- 소비자 없는 스키마는 실제 구현 시 거의 확실하게 변경됨
- 빌드 파이프라인은 스키마 내용과 무관하므로 선행 세팅 가능

## 산출물

```
apex_shared/
├── CMakeLists.txt              ← flatc 코드젠 + STATIC 라이브러리 타겟
├── schemas/                    ← .fbs 파일 (현재 비어있음, .gitkeep 유지)
├── lib/
│   ├── include/apex/shared/    ← 공유 헤더
│   └── src/
│       └── placeholder.cpp     ← 빌드 통과용 빈 소스
└── README.md                   ← 갱신
```

## CMake 구조

- **타겟**: `apex_shared` (STATIC 라이브러리)
- **FlatBuffers 코드젠**: apex_core와 동일한 `foreach + add_custom_command` 패턴
- **의존성**: `flatbuffers::flatbuffers` (vcpkg)
- **include 전파**: `target_include_directories(PUBLIC)` — 서비스에서 `target_link_libraries`만 하면 헤더 경로 자동 전파

## FlatBuffers 네임스페이스 규칙

| 레이어 | 네임스페이스 | 예시 |
|--------|-------------|------|
| 프레임워크 (apex_core) | `apex.messages` | EchoRequest, Heartbeat, ErrorResponse |
| 서비스 공유 (apex_shared) | `apex.shared.<domain>` | apex.shared.auth, apex.shared.chat, apex.shared.log |

## apex_core와의 관계

| 구분 | apex_core | apex_shared |
|------|-----------|-------------|
| 스키마 | 프레임워크 레벨 | 서비스 레벨 (비즈니스 도메인) |
| 빌드 | 독립 | flatbuffers만 의존 (apex_core 의존 X) |
| 코드젠 패턴 | foreach + add_custom_command | 동일 |

## 향후 확장

- `.fbs` 파일 추가 → `APEX_SHARED_SCHEMAS` 리스트에 등록 → 자동 코드젠
- 공유 유틸 코드 → `lib/src/`에 추가, CMakeLists.txt 소스 리스트에 등록
