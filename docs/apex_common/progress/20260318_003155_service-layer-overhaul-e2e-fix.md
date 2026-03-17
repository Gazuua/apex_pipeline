# 서비스 레이어 오버홀 — E2E 회귀 수정 완료

**브랜치**: `feature/service-layer-overhaul`
**PR**: #37
**결과**: 71/71 유닛 + 11/11 E2E + CI 전체 통과 (linux-gcc, linux-asan, linux-tsan, root-linux-gcc, windows-msvc)

## 수정 내역

### 1. Server::run() sync_default_handler 타이밍 수정
- **문제**: Phase 0(서비스 생성 직후)에서 호출 → on_start() 전이라 default_handler가 null → TCP 리스너에 전파 안 됨
- **원인**: ServiceBase 마이그레이션(623bd75)에서 핸들러 등록을 post_init_callback → on_start()로 이동했지만, sync 타이밍은 그대로
- **수정**: Phase 0에서 제거, Phase 3.5(on_start 이후)에 재배치
- **파일**: `apex_core/src/server.cpp`

### 2. E2E response_topic 정합성 복구
- **문제**: Auth/Chat 서비스가 자체 config의 response_topic(`auth.responses`/`chat.responses`)으로 응답 → Gateway는 `gateway.responses`만 구독
- **수정**: E2E TOML 설정에서 response_topic을 `gateway.responses`로 통일
- **파일**: `apex_services/tests/e2e/auth_svc_e2e.toml`, `chat_svc_e2e.toml`
- **참고**: Chat은 `[chat]` 섹션에서, Auth는 `[kafka]` 섹션에서 response_topic을 읽는 차이 확인

### 3. test_redis_adapter ASAN heap-use-after-free 수정
- **문제**: adapter를 engine보다 먼저 선언 → C++ 소멸 역순으로 engine(io_context)이 먼저 파괴 → adapter 소멸자에서 UAF
- **수정**: 선언 순서를 engine → adapter로 변경하여 소멸 순서 보장
- **파일**: `apex_shared/tests/unit/test_redis_adapter.cpp`
