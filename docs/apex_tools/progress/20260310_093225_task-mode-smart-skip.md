# auto-review v1.2: task 모드 Round 1 스마트 스킵 완료

## 작업 내용
- auto-review task 모드 Phase 1 Round 1에서 변경 파일 타입 기반 리뷰어 스마트 스킵 추가
- 파일타입→리뷰어 매핑 테이블을 공용 섹션으로 승격 (Phase 1/2 공유)
- 매핑 테이블 확장: .fbs, Dockerfile, CI yml, suppressions, .toml/.sql, .clangd/.gitattributes/.editorconfig
- Phase 2 인라인 매핑을 공용 참조로 교체
- 리뷰 보고서에 참여 현황 포맷 추가

## 변경 파일
- `apex_tools/claude-plugin/commands/auto-review.md` — 핵심 변경
- `docs/Apex_Pipeline.md` — v1.2 완료 항목 추가
- `docs/apex_tools/plans/20260310_091104_task-mode-smart-skip-design.md` — 설계서
- `docs/apex_tools/plans/20260310_091104_task-mode-smart-skip-plan.md` — 계획서
- `docs/apex_tools/review/20260310_093225_auto-review.md` — 리뷰 보고서
- `README.md` — 완료 항목 추가

## 리뷰 결과
- task 리뷰: 2 라운드 (Important 1건 수정 → Clean)
- full 리뷰: 1 라운드 (기존 코드 기술 부채만, PR 범위 내 0건)
- 스마트 스킵 실전 검증 성공: .md만 변경 시 docs+general만 디스패치

## PR
- 브랜치: feature/task-mode-smart-skip
