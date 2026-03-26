# HttpServerBase 추출 + AdminHttpServer + 런타임 로그 레벨 (BACKLOG-179)

**날짜**: 2026-03-26
**브랜치**: `feature/log-level-and-log-svc`
**스코프**: CORE

## 목표

1. 기존 `MetricsHttpServer`에서 공통 HTTP 서버 로직을 `HttpServerBase`로 추출
2. `AdminHttpServer` 신규 — 별도 포트, 런타임 관리 엔드포인트 전용
3. `POST /admin/log-level` — spdlog 로거 레벨 동적 전환 (재시작 불필요)

## 1. HttpServerBase

기존 `MetricsHttpServer`의 accept loop / session handling / HTTP parsing을 추출한 베이스 클래스.

### 파일
- `apex_core/include/apex/core/http_server_base.hpp` (신규)
- `apex_core/src/http_server_base.cpp` (신규)

### 인터페이스

```cpp
namespace apex::core {

struct HttpResponse {
    unsigned status_code;       // 200, 400, 404, 405, 500
    std::string content_type;   // "application/json", "text/plain"
    std::string body;
};

class HttpServerBase {
public:
    HttpServerBase() = default;
    virtual ~HttpServerBase() = default;

    // Non-copyable, non-movable (io_context 참조 보유)
    HttpServerBase(const HttpServerBase&) = delete;
    HttpServerBase& operator=(const HttpServerBase&) = delete;

    void start(boost::asio::io_context& io, uint16_t port);
    void stop();
    [[nodiscard]] uint16_t local_port() const;

protected:
    // 파생 클래스가 override. method/path/query/body 파싱은 베이스가 처리.
    [[nodiscard]] virtual HttpResponse handle_request(
        std::string_view method,
        std::string_view path,
        std::string_view query,
        std::string_view body) = 0;

    ScopedLogger logger_;  // 파생 클래스가 생성자에서 초기화

private:
    void do_accept();
    void handle_session(boost::asio::ip::tcp::socket socket);

    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    std::shared_ptr<SessionTracker> tracker_;  // in-flight session 관리
};

} // namespace apex::core
```

### 기존 MetricsHttpServer에서 이동하는 로직
- TCP accept loop (`do_accept`)
- HTTP request 파싱 (method, path, query 추출)
- HTTP response 직렬화 (`HTTP/1.1 {status} ...`)
- `SessionTracker` (in-flight 카운터 + graceful stop)
- `Connection: close` 단일 요청 처리 패턴 유지

## 2. MetricsHttpServer 리팩터

### 파일
- `apex_core/include/apex/core/metrics_http_server.hpp` (수정)
- `apex_core/src/metrics_http_server.cpp` (수정)

### 변경
- `HttpServerBase` 상속
- `handle_request` override — `/metrics`, `/health`, `/ready` 라우팅
- `start()` 시그니처 변경: `start(io, port, registry, running)` → 생성자에서 registry/running 바인딩, `start(io, port)` 호출
- 기존 외부 API (`start`, `stop`, `local_port`) 동작 100% 보존
- `Server`에서의 호출 패턴 변경 최소화

## 3. AdminHttpServer (신규)

### 파일
- `apex_core/include/apex/core/admin_http_server.hpp` (신규)
- `apex_core/src/admin_http_server.cpp` (신규)

### 엔드포인트

#### POST /admin/log-level
런타임 로그 레벨 변경.

- **Query params**: `logger` (apex|app), `level` (trace|debug|info|warn|error|critical)
- **동작**: `spdlog::get(logger)->set_level(spdlog::level::from_str(level))`
- **응답**:
  - `200 OK` — `{"logger":"apex","level":"debug","previous":"info"}`
  - `400 Bad Request` — `{"error":"unknown logger: foo"}` 또는 `{"error":"invalid level: xyz"}`

#### GET /admin/log-level
현재 로그 레벨 조회.

- **Query params**: `logger` (apex|app, 생략 시 둘 다 반환)
- **응답**: `200 OK` — `{"apex":"info","app":"debug"}` 또는 `{"logger":"apex","level":"info"}`

#### 기타
- 알 수 없는 경로 → `404 Not Found`
- 잘못된 method → `405 Method Not Allowed`

### 보안
- `control_io_` 위에서 동작 (Server 내부 io_context)
- localhost 바인딩 전용 — 외부 접근 불가 (K8s에서 kubectl port-forward 또는 exec로 접근)
- 인증 미구현 (localhost 전용이므로 불필요, BACKLOG-237에서 토큰 인증 추가 가능)

## 4. Server 통합

### ServerConfig 변경

```cpp
struct ServerConfig {
    // ... 기존 필드 ...
    uint16_t admin_port = 0;  // 0 = 비활성 (기존 metrics_port 패턴과 동일)
};
```

### Server::run() 변경

```
기존: metrics_port > 0 → MetricsHttpServer::start(control_io_, port, registry, running)
추가: admin_port > 0 → AdminHttpServer::start(control_io_, port)
```

- `AdminHttpServer`는 `Server` 멤버로 보유
- `Server::stop()`에서 `admin_http_.stop()` 추가
- `control_io_`에서 두 서버가 공존 (각각 독립 acceptor, 독립 포트)

### TOML 설정

```toml
[server]
admin_port = 8082   # 0이면 비활성
```

## 5. 테스트

### test_http_server_base.cpp (신규)
- 베이스 클래스의 accept/parse/response 동작 검증
- 테스트용 파생 클래스 (echo handler) 사용

### test_admin_http_server.cpp (신규)
- `POST /admin/log-level?logger=apex&level=debug` → 200 + 레벨 변경 확인
- `GET /admin/log-level` → 현재 레벨 반환
- 잘못된 logger → 400
- 잘못된 level → 400
- 존재하지 않는 경로 → 404
- 잘못된 method (DELETE 등) → 405

### test_metrics_http_server.cpp (기존)
- 리팩터 후 기존 테스트 전부 통과 확인
- `start()` 시그니처 변경에 따른 테스트 코드 갱신

## 6. CMake

- `apex_core/CMakeLists.txt`에 `http_server_base.cpp`, `admin_http_server.cpp` 추가
- 테스트 타겟 등록

## 7. 의존성 / 위험

- spdlog `set_level()`은 thread-safe (내부 atomic) — 동시성 이슈 없음
- `HttpServerBase` 추출 시 기존 MetricsHttpServer 동작이 깨지지 않도록 주의
- Beast 미사용 — 기존 직접 HTTP 파싱 패턴 유지 (경량)
