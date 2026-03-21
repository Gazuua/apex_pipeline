# session-context 활성 브랜치 표시 오류 수정

## 변경 요약

`apex_tools/session-context.sh`의 "Other Active Branches" 출력 로직 버그 수정.

## 문제

- scopes가 비어있으면 실제 status/summary를 무시하고 `[착수 단계] — 설계 알림 수신 시 자동 통보` 하드코딩 메시지 출력
- 다른 브랜치의 status 필드(implementing, started 등)가 표시되지 않음
- 활성 브랜치 수를 한눈에 파악할 수 없음

## 수정 내용

- status를 항상 표시: `[implementing]`, `[started]` 등 실제 상태
- summary를 항상 표시: scopes 유무와 무관하게 실제 요약문 출력
- scopes는 있을 때만 괄호로 표시, 없으면 생략
- 헤더에 활성 브랜치 개수 표시: `(N개 진행 중)`

## 변경 파일

- `apex_tools/session-context.sh` — Other Active Branches 출력 로직 (1파일)
