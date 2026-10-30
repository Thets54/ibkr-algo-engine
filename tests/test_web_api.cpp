// HTTP API end to end, including the security gate.
#include <gtest/gtest.h>
#include <httplib.h>

#include <nlohmann/json.hpp>

#include "algo/server/web_server.hpp"
#include "test_helpers.hpp"

using namespace algo;
using nlohmann::json;
using test::TempDir;

namespace {
struct Fixture {
  TempDir dir;
  AppConfig cfg;
  std::unique_ptr<SessionManager> sessions;
  std::unique_ptr<WebServer> server;
  std::unique_ptr<httplib::Client> client;

  Fixture() {
    cfg.paths.strategies_dir = (dir.path() / "strategies").string();
    cfg.paths.data_dir = (dir.path() / "data").string();
    cfg.paths.cache_dir = (dir.path() / "cache").string();
    sessions = std::make_unique<SessionManager>(cfg);
    server = std::make_unique<WebServer>(*sessions, "127.0.0.1", 0);
    EXPECT_TRUE(server->bind());
    server->start_background();
    client = std::make_unique<httplib::Client>("127.0.0.1", server->port());
    client->set_default_headers({{"X-Algo-Client", "test"}});
  }
  ~Fixture() {
    sessions->stop();
    sessions->wait();
    server->stop();
  }
  json post_json(const std::string& path, const json& body, int expect = 200) {
    auto r = client->Post(path, body.dump(), "application/json");
    EXPECT_TRUE(r);
    if (!r) return {};
    EXPECT_EQ(r->status, expect) << r->body;
    return json::parse(r->body);
  }
  json get_json(const std::string& path) {
    auto r = client->Get(path);
    EXPECT_TRUE(r && r->status == 200);
    return r ? json::parse(r->body) : json{};
  }
};
}  // namespace

TEST(WebApi, ServesDashboardAndHealth) {
  Fixture f;
  auto r = f.client->Get("/");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 200);
  EXPECT_NE(r->body.find("<title>Algo Engine</title>"), std::string::npos);
  auto h = f.get_json("/api/health");
  EXPECT_TRUE(h["ok"].get<bool>());
}

TEST(WebApi, RejectsWritesWithoutClientHeader) {
  Fixture f;
  httplib::Client raw("127.0.0.1", f.server->port());
  auto r = raw.Post("/api/strategies", R"({"name":"x.py","content":"x"})", "application/json");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 403);
  EXPECT_TRUE(f.sessions->list_strategies().empty());
}

TEST(WebApi, RejectsForeignHostHeader) {
  Fixture f;
  httplib::Client raw("127.0.0.1", f.server->port());
  auto r = raw.Get("/api/status", {{"Host", "evil.example.com"}});
  ASSERT_TRUE(r);
  EXPECT_EQ(r->status, 403);
}

TEST(WebApi, UploadJsonAndMultipartThenValidate) {
  Fixture f;
  auto tpl = f.client->Get("/api/template?lang=py");
  ASSERT_TRUE(tpl && tpl->status == 200);
  auto res = f.post_json("/api/strategies", {{"name", "tpl.py"}, {"content", tpl->body}});
  EXPECT_TRUE(res["valid"].get<bool>()) << res.dump();

  httplib::MultipartFormDataItems items = {{"file", "bad = (\n", "bad.py", "text/x-python"}};
  auto r = f.client->Post("/api/strategies", items);
  ASSERT_TRUE(r && r->status == 200);
  auto j = json::parse(r->body);
  EXPECT_TRUE(j["saved"].get<bool>());
  EXPECT_FALSE(j["valid"].get<bool>());

  auto list = f.get_json("/api/strategies");
  EXPECT_EQ(list.size(), 2u);
  auto v = f.post_json("/api/strategies/tpl.py/validate", json::object());
  EXPECT_TRUE(v["valid"].get<bool>());
  auto src = f.client->Get("/api/strategies/tpl.py");
  EXPECT_EQ(src->body, tpl->body);
  auto del = f.client->Delete("/api/strategies/bad.py");
  EXPECT_EQ(del->status, 200);
  f.post_json("/api/strategies", {{"name", "../../etc.py"}, {"content", "x"}}, 400);
}

TEST(WebApi, CppTemplateCompiles) {
  Fixture f;
  auto tpl = f.client->Get("/api/template?lang=cpp");
  auto res = f.post_json("/api/strategies", {{"name", "tpl.cpp"}, {"content", tpl->body}});
  EXPECT_TRUE(res["valid"].get<bool>()) << res.dump();
}

TEST(WebApi, BacktestThroughApi) {
  Fixture f;
  f.dir.write("data/AAA.csv", test::to_csv(test::random_walk("AAA", 120)));
  EXPECT_EQ(f.get_json("/api/datasets")["symbols"], json::array({"AAA"}));
  auto tpl = f.client->Get("/api/template?lang=py");
  f.post_json("/api/strategies", {{"name", "s.py"}, {"content", tpl->body}});
  f.post_json("/api/run", {{"strategy", "s.py"}, {"mode", "backtest"}, {"symbols", "aaa"},
                           {"bar_seconds", 86400}, {"params", {{"fast", 5}, {"slow", 20}}}});
  ASSERT_TRUE(test::eventually([&] { return f.get_json("/api/status")["state"] == "finished"; }));
  auto st = f.get_json("/api/status");
  EXPECT_TRUE(st.contains("metrics"));
  EXPECT_GT(st["engine"]["equity_curve"].size(), 50u);
  // Live is refused unless enabled in the config.
  auto live = f.post_json("/api/run", {{"strategy", "s.py"}, {"mode", "live"}, {"symbols", "AAA"}}, 409);
  EXPECT_NE(live["error"].get<std::string>().find("allow_live"), std::string::npos);
}
