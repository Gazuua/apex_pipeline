# Apex Pipeline

C++23 코루틴 기반 고성능 서버 프레임워크 모노레포.

## 현재 상태

| Phase | 내용 | 상태 |
|-------|------|------|
| Phase 1~4.7 | 코어 프레임워크 기초 | 완료 |
| Phase 5 | 기반 정비 (TOML/spdlog/Shutdown/CI) | 완료 (PR #1 merged) |
| **auto-review v1.1** | 2-tier 병렬 리뷰 + confidence ≥40 + Smart Skip | PR #2 CI 대기 |
| Phase 6 | Kafka 체인 + KafkaSink | 다음 |
| Phase 7 | Gateway 서비스 | 예정 |
| Phase 8a | WebSocket (Boost.Beast) | 예정 |

## 프로젝트 구조

```
apex_core/      — 코어 프레임워크 (C++23, Boost.Asio 코루틴)
apex_services/  — MSA 서비스
apex_shared/    — FlatBuffers 스키마 + 공유 라이브러리
apex_infra/     — Docker, K8s 인프라
apex_tools/     — CLI, 스크립트, git-hooks, auto-review 플러그인
docs/           — 설계서, 계획서, 리뷰 보고서
```

## 빌드

```bash
# Windows (MSYS bash)
cmd.exe //c "D:\\.workspace\\apex_core\\build.bat debug"

# Linux
./apex_core/build.sh debug
```

변형: `debug` / `asan` / `tsan`

## auto-review 플러그인

`/auto-review [task|full]` — 5개 전문 리뷰어 에이전트가 병렬 리뷰 → 수정 → Clean까지 반복 → PR + CI 자동화.

상세: `apex_tools/claude-plugin/`
