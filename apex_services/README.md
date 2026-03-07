# apex_services

MSA 서비스 디렉토리. 각 서비스는 독립 빌드(`vcpkg.json` + `CMakeLists.txt` + `Dockerfile`)를 가진다.

| 서비스 | 역할 |
|--------|------|
| gateway | WebSocket/HTTP 게이트웨이 |
| auth-svc | 인증/인가 |
| chat-svc | 채팅 로직 |
| log-svc | 로그 수집/저장 |
