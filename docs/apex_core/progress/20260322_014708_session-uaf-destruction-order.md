# v0.5.10.2 — Session UAF 소멸 순서 수정

**브랜치**: `bugfix/session-uaf-destruction-order`
**버전**: v0.5.10.2
**완료**: 2026-03-22

## 변경 요약

Server::~Server()에 명시적 파괴 순서를 추가하여 미완료 코루틴의 `intrusive_ptr<Session>` UAF를 방지했다.

## 문제

C++ 멤버 변수는 선언 역순으로 소멸된다. 그러나 Server의 멤버 간 의존 관계가 선언 순서와 일치하지 않아,
`~io_context()`가 미완료 코루틴 프레임을 파괴할 때 코루틴 내부의 `intrusive_ptr<Session>`이 해제되면서
이미 파괴된 `per_core_`의 slab 메모리에 접근하는 UAF가 발생했다.

기존 백로그 #28은 "멤버 역순 소멸(RAII)으로 안전"이라고 WONTFIX 처리됐었으나, 실제로는 안전하지 않았다.

## 해결

Server::~Server()에서 의존성 역순으로 명시적 파괴:

1. `listeners_.clear()` — TcpAcceptor 소켓이 io_context에 등록됨, 먼저 정리
2. `per_core_[*].scheduler.reset()` — 타이머가 io_context에 등록됨
3. `core_engine_.reset()` — io_context 소유. ~io_context()가 미완료 코루틴을 파괴하며 intrusive_ptr<Session> 해제. per_core_의 slab 메모리가 유효한 상태에서 실행됨

## 변경 파일

| 파일 | 변경 |
|------|------|
| `apex_core/src/server.cpp` | Server::~Server()에 명시적 파괴 순서 추가 (14줄) |
| `CLAUDE.md` | CI 폴링 정책 추가 (`--interval 30`) |
