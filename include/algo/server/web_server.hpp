#pragma once
// Local HTTP server for the dashboard (web/index.html, embedded at build
// time) and its JSON API. Binds to 127.0.0.1 by default.
//
// Uploaded strategies are executable code, so the API is hardened against
// browser-based attacks from other websites:
//   * state-changing requests must carry the header "X-Algo-Client"
//     (cross-site forms cannot set custom headers without a CORS preflight,
//     which this server never approves)
//   * the Host header must be localhost / 127.0.0.1 / [::1] or the configured
//     host (blocks DNS rebinding)

#include <memory>
#include <string>
#include <thread>

#include "algo/engine/session_manager.hpp"

namespace httplib {
class Server;
}

namespace algo {

class WebServer {
 public:
  WebServer(SessionManager& sessions, std::string host, int port);
  ~WebServer();

  /// Binds the socket (port 0 = pick a free port). Returns false on failure.
  bool bind();
  /// Serves in a background thread (call bind() first).
  void start_background();
  /// Serves on the calling thread until stop().
  void run();
  void stop();
  [[nodiscard]] int port() const { return port_; }

 private:
  void register_routes();

  SessionManager& sessions_;
  std::string host_;
  int port_;
  std::unique_ptr<httplib::Server> server_;
  std::thread thread_;
};

}  // namespace algo
