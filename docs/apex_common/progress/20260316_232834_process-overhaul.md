# 프로세스 오버홀 완료 기록

- **PR**: #31
- **설계서**: `docs/apex_common/plans/20260316_222855_process-overhaul-design.md`
- **구현 계획**: `docs/apex_common/plans/20260316_224257_process-overhaul-plan.md`

## 작업 요약

SendMessage 팀 구조(3-layer, 13 에이전트)를 Agent tool 서브에이전트(2-layer, 7 리뷰어)로 전환하였다.

### 핵심 변경

1. **에이전트 구조 전환**: SendMessage 기반 3-layer 13 에이전트 → Agent tool 기반 2-layer 7 리뷰어
2. **좀비 에이전트/데드락 해결**: Agent tool 사용으로 구조적 해결 (SendMessage의 비동기 메시지 누락 문제 제거)
3. **행동 제약 전면 제거**: 과도한 행동 제약을 제거하고 목적+범위만 남김. 에이전트 자율 판단으로 전환
4. **"메인 컨텍스트 절약" 지침 제거**: 메인이 코드를 직접 읽고 검증하는 방식으로 전환

## 변경 내역

**변경 파일 28개, +954줄 / -2,337줄**

### CLAUDE.md (루트)
- 에이전트 작업 규칙 재작성 (5→3규칙)

### 에이전트 파일
- coordinator + 10 리뷰어 삭제
- 3개 유지 + 업데이트
- 4개 신규 생성

### auto-review.md
- 전면 재작성 (368줄 → 90줄)

### config.md
- 7명 리뷰어 참고용 전환 (128줄 → 52줄)

### 기타
- `plugin.json`: v3.0.0
- `docs/BACKLOG.md`: 좀비 에이전트 이슈 완료 삭제
- `README.md`, `docs/Apex_Pipeline.md` 등 잔존 용어 수정
