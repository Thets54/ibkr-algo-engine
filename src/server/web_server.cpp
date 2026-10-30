#include "algo/server/web_server.hpp"

#include <httplib.h>

#include <nlohmann/json.hpp>

#include "algo/data/csv_feed.hpp"
#include "algo/web_assets.hpp"
#include "templates.hpp"

namespace algo {

using nlohmann::json;

namespace {

constexpr const char* kJson = "application/json";
constexpr const char* kStrategyPattern = R"(/api/strategies/([A-Za-z0-9_.\-]+))";

void send_json(httplib::Response& res, const json& j, int status = 200) {
  res.status = status;
  res.set_content(j.dump(), kJson);
}

void send_error(httplib::Response& res, const std::string& msg, int status = 400) {
  send_json(res, json{{"error", msg}}, status);
}

std::string host_without_port(std::string host) {
  if (!host.empty() && host.front() == '[') {
    auto end = host.find(']');
    return end == std::string::npos ? host : host.substr(0, end + 1);
  }
  auto colon = host.rfind(':');
  return colon == std::string::npos ? host : host.substr(0, colon);
}

}  // namespace

WebServer::WebServer(SessionManager& sessions, std::string host, int port)
    : sessions_(sessions), host_(std::move(host)), port_(port), server_(std::make_unique<httplib::Server>()) {
  register_routes();
}

WebServer::~WebServer() { stop(); }

bool WebServer::bind() {
  if (port_ == 0) {
    port_ = server_->bind_to_any_port(host_);
    return port_ > 0;
  }
  return server_->bind_to_port(host_, port_);
}

void WebServer::start_background() {
  thread_ = std::thread([this] { server_->listen_after_bind(); });
  server_->wait_until_ready();
}

void WebServer::run() { server_->listen_after_bind(); }

void WebServer::stop() {
  if (server_) server_->stop();
  if (thread_.joinable()) thread_.join();
}

void WebServer::register_routes() {
  auto& s = *server_;
  s.set_payload_max_length(8 * 1024 * 1024);
  s.set_default_headers({{"X-Content-Type-Options", "nosniff"}, {"Cache-Control", "no-store"}});

  // ---- security gate ------------------------------------------------------
  const std::string configured_host = host_;
  s.set_pre_routing_handler([configured_host](const httplib::Request& req, httplib::Response& res) {
    const std::string h = host_without_port(req.get_header_value("Host"));
    const bool host_ok = h.empty() || h == "localhost" || h == "127.0.0.1" || h == "[::1]" ||
                         h == configured_host;
    if (!host_ok) {
      send_error(res, "forbidden host", 403);
      return httplib::Server::HandlerResponse::Handled;
    }
    if (req.method != "GET" && req.method != "HEAD" && !req.has_header("X-Algo-Client")) {
      send_error(res, "missing X-Algo-Client header", 403);
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  s.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      send_error(res, e.what(), 500);
    } catch (...) {
      send_error(res, "internal error", 500);
    }
  });

  // ---- dashboard ----------------------------------------------------------
  s.Get("/", [](const httplib::Request&, httplib::Response& res) {
    res.set_content(std::string(web::kIndexHtml), "text/html; charset=utf-8");
  });

  s.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
    send_json(res, {{"ok", true}, {"python", python_strategies_supported()},
#if defined(ALGO_WITH_IBKR)
                    {"ibkr", true}
#else
                    {"ibkr", false}
#endif
                   });
  });

  s.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
    send_json(res, sessions_.status());
  });

  s.Get("/api/config", [this](const httplib::Request&, httplib::Response& res) {
    send_json(res, config_to_json(sessions_.config()));
  });

  // ---- strategies ---------------------------------------------------------
  s.Get("/api/strategies", [this](const httplib::Request&, httplib::Response& res) {
    json arr = json::array();
    for (const auto& i : sessions_.list_strategies()) {
      arr.push_back({{"name", i.name}, {"kind", i.kind}, {"size", i.size}, {"modified", i.modified}});
    }
    send_json(res, arr);
  });

  s.Get(kStrategyPattern, [this](const httplib::Request& req, httplib::Response& res) {
    try {
      res.set_content(sessions_.read_strategy(req.matches[1]), "text/plain; charset=utf-8");
    } catch (const std::exception& e) {
      send_error(res, e.what(), 404);
    }
  });

  s.Post("/api/strategies", [this](const httplib::Request& req, httplib::Response& res) {
    std::string name, content;
    if (req.is_multipart_form_data()) {
      if (!req.has_file("file")) return send_error(res, "multipart field 'file' missing");
      const auto f = req.get_file_value("file");
      name = f.filename;
      content = f.content;
    } else {
      json j = json::parse(req.body, nullptr, false);
      if (j.is_discarded() || !j.is_object()) return send_error(res, "expected JSON {name, content}");
      name = j.value("name", "");
      content = j.value("content", "");
    }
    if (content.empty()) return send_error(res, "empty file");
    if (!is_valid_strategy_filename(name)) {
      return send_error(res, "invalid file name '" + name +
                                 "': letters, digits, '_', '-', '.' and a .py/.cpp/.cc/.so extension");
    }
    const std::string err = sessions_.save_strategy(name, content);
    send_json(res, {{"name", name}, {"saved", true}, {"valid", err.empty()}, {"error", err}});
  });

  s.Post(std::string(kStrategyPattern) + "/validate",
         [this](const httplib::Request& req, httplib::Response& res) {
           const std::string err = sessions_.validate_strategy(req.matches[1]);
           send_json(res, {{"name", req.matches[1]}, {"valid", err.empty()}, {"error", err}});
         });

  s.Delete(kStrategyPattern, [this](const httplib::Request& req, httplib::Response& res) {
    if (!sessions_.delete_strategy(req.matches[1])) {
      return send_error(res, "cannot delete (missing, or currently running)");
    }
    send_json(res, {{"deleted", true}});
  });

  s.Get("/api/template", [](const httplib::Request& req, httplib::Response& res) {
    const std::string lang = req.has_param("lang") ? req.get_param_value("lang") : "py";
    res.set_content(lang == "cpp" ? templates::kCpp : templates::kPython, "text/plain; charset=utf-8");
  });

  // ---- data -----------------------------------------------------------------
  s.Get("/api/datasets", [this](const httplib::Request&, httplib::Response& res) {
    send_json(res, {{"dir", sessions_.config().paths.data_dir},
                    {"symbols", list_csv_symbols(sessions_.config().paths.data_dir)}});
  });

  // ---- runs -------------------------------------------------------------------
  s.Post("/api/run", [this](const httplib::Request& req, httplib::Response& res) {
    json j = json::parse(req.body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return send_error(res, "expected a JSON object");
    RunRequest r;
    try {
      r = run_request_from_json(j);
    } catch (const std::exception& e) {
      return send_error(res, std::string("bad request: ") + e.what());
    }
    // The dashboard never sets data_dir: backtests read the configured folder.
    r.data_dir.clear();
    const std::string err = sessions_.start(r);
    if (!err.empty()) return send_error(res, err, 409);
    send_json(res, {{"started", true}});
  });

  s.Post("/api/stop", [this](const httplib::Request&, httplib::Response& res) {
    sessions_.stop();
    send_json(res, {{"stopping", true}});
  });

  s.Post("/api/kill", [this](const httplib::Request& req, httplib::Response& res) {
    json j = json::parse(req.body, nullptr, false);
    std::string reason = "manual kill switch (dashboard)";
    if (!j.is_discarded() && j.is_object()) reason = j.value("reason", reason);
    sessions_.kill(reason);
    send_json(res, {{"killed", true}});
  });
}

}  // namespace algo
