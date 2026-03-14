# auto-review 감도 강화 + 코드 리뷰 이슈 수정 (v0.4.5.2)

## 작업 내용

### auto-review 시스템 개선
- 리뷰어 감도 강화: 체크리스트 도입, threshold 50%, cross-domain 관심사 확장
- cross-cutting 리뷰어 신설 → 11명에서 12명 체제로 확장
- coordinator 오버랩 정책 + 자동 시작 메커니즘 도입

### 코드 리뷰 이슈 수정 (6건)
1. **PendingCommand UAF**: SlabAllocator 전환으로 use-after-free 해결
2. **Silent disconnect 로깅**: 무음 연결 해제 시 로그 추가
3. **어댑터 init 실패 throw**: 초기화 실패 시 예외 전파 보장
4. **컨테이너 일관성**: 컨테이너 타입 사용 일관성 개선
5. **RingBuffer shrink**: 링버퍼 축소 로직 수정
6. **pgbouncer DoS 방어**: pgbouncer 설정 보안 강화

### 문서
- MIT LICENSE 추가
- README 아키텍처 섹션 개편
- BACKLOG 갱신

### 드랍
- C-2 write queue: per-core 싱글 스레드 보장으로 false positive 판정 → 드랍

## 변경 파일 (주요)
- `apex_tools/claude-plugin/` — auto-review 설정 (리뷰어 12명, 감도 강화)
- `apex_core/` — PendingCommand UAF, silent disconnect, RingBuffer shrink
- `apex_shared/` — 어댑터 init 실패 throw, 컨테이너 일관성
- `apex_infra/` — pgbouncer DoS 방어
- `LICENSE` — MIT 라이선스 추가
- `README.md` — 아키텍처 섹션 개편
- `docs/BACKLOG.md` — 갱신

## PR
- 브랜치: feature/auto-review-final
