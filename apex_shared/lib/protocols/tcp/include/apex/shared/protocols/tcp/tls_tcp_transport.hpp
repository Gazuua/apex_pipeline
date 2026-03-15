#pragma once

/// TLS TCP Transport — 스켈레톤.
/// 실제 구현은 Gateway MVP (v0.5.2.0)에서 완성한다.
/// 현재는 타입 정의만 제공하여 Transport concept 만족을 검증한다.
///
/// 구현 시 필요한 의존성:
///   - Boost.Asio ssl::context, ssl::stream
///   - OpenSSL (vcpkg: openssl)
///   - per-core SSL_CTX (Seastar 방식, lock contention 제거)

// TODO(v0.5.2.0): OpenSSL 의존성 추가 후 구현
// #include <boost/asio/ssl.hpp>
// #include <apex/core/transport.hpp>
//
// namespace apex::shared::protocols::tcp {
//
// struct TlsTcpTransport {
//     struct Config {
//         std::string cert_file;
//         std::string key_file;
//         std::string ca_file;
//     };
//     using Socket = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;
//     // ...
// };
//
// static_assert(apex::core::Transport<TlsTcpTransport>);
//
// } // namespace apex::shared::protocols::tcp
