# Apex Pipeline - 디렉토리 구조 설계

**작성일:** 2026-03-07
**상태:** 승인됨

---

## 배경

BoostAsioCore(현 apex_core) 프레임워크가 v0.2.1까지 완성된 상태에서,
나머지 프로젝트 구조(서비스, 인프라, 공유 코드 등)의 디렉토리를 확정한다.

---

## 결정 사항 요약

| 항목 | 결정 | 근거 |
|------|------|------|
| Git 전략 | 모노레포 | 1인 개발, 원자적 커밋, 포트폴리오 일체성 |
| 코어 참조 방식 | `add_subdirectory` | 소스 수정 즉시 반영, 디버깅 step-in 가능 |
| 서비스 독립성 | 서비스별 독립 `vcpkg.json` + `CMakeLists.txt` | Docker 빌드 독립성, 의존성 명확화 |
| 내부 구조 통일 | 느슨한 가이드 | 공통 패턴은 있되 서비스 특성에 따라 변형 가능 |
| 서비스 배치 | 균일 구조 (`services/` 하위) | Gateway도 서비스의 하나, 예외 없는 규칙 |
| `shared/` 범위 | 스키마 + 공유 코드 | 실무 관행, FlatBuffers + 공용 유틸리티 |
| 루트 디렉토리 네이밍 | `apex_` 프리픽스 | 프로젝트 소속 명확화 |

---

## 디렉토리 구조

```
D:/.workspace/                          <- 모노레포 루트 (apex-pipeline)
|
+-- apex_core/                          <- 프레임워크 (기존, 자체 빌드 독립)
|   +-- core/
|   |   +-- include/apex/core/
|   |   +-- src/
|   |   +-- tests/
|   |   +-- examples/
|   |   +-- schemas/                    <- 프레임워크 내장 스키마
|   |   +-- CMakeLists.txt
|   +-- docs/
|   +-- vcpkg.json
|   +-- CMakeLists.txt
|   +-- CMakePresets.json
|
+-- apex_services/
|   +-- gateway/                        <- Gateway 서비스
|   |   +-- include/apex/gateway/
|   |   +-- src/
|   |   +-- tests/
|   |   +-- CMakeLists.txt
|   |   +-- vcpkg.json
|   |   +-- Dockerfile
|   +-- auth-svc/                       <- 인증 서비스
|   |   +-- include/apex/auth/
|   |   +-- src/
|   |   +-- tests/
|   |   +-- CMakeLists.txt
|   |   +-- vcpkg.json
|   |   +-- Dockerfile
|   +-- chat-svc/                       <- 채팅 서비스
|   |   +-- (동일 패턴)
|   +-- log-svc/                        <- 로그 서비스
|       +-- (동일 패턴)
|
+-- apex_shared/
|   +-- schemas/                        <- 공유 FlatBuffers (.fbs)
|   |   +-- common.fbs
|   |   +-- auth.fbs
|   |   +-- chat.fbs
|   |   +-- gateway.fbs
|   +-- lib/                            <- 공유 C++ 코드
|       +-- include/apex/shared/
|       +-- src/
|       +-- CMakeLists.txt
|
+-- apex_infra/
|   +-- docker-compose.yml              <- 프로파일: minimal / observability / full
|   +-- k8s/                            <- Helm charts
|       +-- gateway/
|       +-- auth-svc/
|       +-- chat-svc/
|
+-- apex_tools/
|   +-- new-service.sh                  <- 서비스 스캐폴딩 스크립트
|
+-- apex_docs/
|   +-- plans/                          <- 설계/구현 계획
|   +-- progress/                       <- 진행 완료 기록
|   +-- review/                         <- 코드 리뷰 기록
|   +-- Apex_Pipeline.md
|
+-- CMakeLists.txt                      <- 루트 CMake (전체 오케스트레이션)
+-- vcpkg.json                          <- 루트 vcpkg (전체 빌드 편의용)
+-- CMakePresets.json
+-- .gitignore
+-- README.md
```

---

## 서비스가 코어를 참조하는 방식

```cmake
# 서비스 CMakeLists.txt 내부
find_package(apex QUIET)
if(NOT apex_FOUND)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../apex_core/core apex_core)
endif()

target_link_libraries(my_service PRIVATE apex::core)
```

- 로컬 개발: `add_subdirectory`로 소스 직접 참조
- 외부 사용자: `find_package`로 설치된 라이브러리 사용
- 두 방식이 동일한 CMake 타겟(`apex::core`)으로 수렴

---

## 미래 고려 사항

- **apex_core 독립 배포**: `git subtree split`으로 히스토리 보존하며 분리 가능
- **서비스 추가**: `apex_services/` 아래에 동일 패턴으로 추가
- **루트 디렉토리 이름 변경**: 코드/빌드에 "BoostAsioCore" 참조 없음, 변경 공수 최소
