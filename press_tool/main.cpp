#include <async_simple/coro/Collect.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "../include/cinatra.hpp"
#include "async_simple/Try.h"
#include "cmdline.h"
#include "config.h"

press_config init_conf(const cmdline::parser& parser) {
  press_config conf{};
  conf.connections = parser.get<int>("connections");
  conf.threads_num = parser.get<int>("threads");

  std::string duration_str = parser.get<std::string>("duration");
  if (duration_str.size() < 2) {
    std::cerr << parser.usage();
    exit(1);
  }

  bool is_ms = duration_str.substr(duration_str.size() - 2) == "ms";
  std::string_view pre(duration_str.data(), duration_str.size() - 1);
  int tm = atoi(pre.data());
  if (is_ms) {
    conf.press_interval = std::chrono::milliseconds(tm);
  }
  if (duration_str.back() == 's') {
    conf.press_interval = std::chrono::seconds(tm);
  }
  else if (duration_str.back() == 'm') {
    conf.press_interval = std::chrono::minutes(tm);
  }
  else {
    conf.press_interval = std::chrono::hours(tm);
  }

  if (parser.rest().empty()) {
    std::cerr << "lack of url";
    exit(1);
  }

  conf.url = parser.rest().back();

  return conf;
}

async_simple::coro::Lazy<void> create_clients(const press_config& conf,
                                              std::vector<thread_counter>& v) {
  // create clients
  for (int i = 0; i < conf.connections; ++i) {
    size_t next = i % conf.threads_num;
    auto& thd_counter = v[next];
    auto client = std::make_shared<cinatra::coro_http_client>(
        thd_counter.ioc->get_executor());
    auto result = co_await client->async_get(conf.url);
    if (result.status != 200) {
      std::cerr << "connect " << conf.url
                << " failed: " << result.net_err.message() << "\n";
      exit(1);
    }
    thd_counter.conns.push_back(std::make_shared<cinatra::coro_http_client>(
        thd_counter.ioc->get_executor()));
  }
}

async_simple::coro::Lazy<void> press(thread_counter& counter,
                                     const std::string& url,
                                     std::atomic_bool& stop) {
  while (!stop) {
    for (auto& conn : counter.conns) {
      cinatra::resp_data result = co_await conn->async_get(url);
      counter.requests++;
      if (result.status == 200) {
        counter.complete++;
      }
      else {
        std::cerr << "request failed: " << result.net_err.message() << "\n";
        break;
      }
    }
  }
}

/*
 * eg: -c 1 -d 15s -t 1 http://localhost/
 */
int main(int argc, char* argv[]) {
  cmdline::parser parser;
  parser.add<int>(
      "connections", 'c',
      "total number of HTTP connections to keep open with"
      "                   each thread handling N = connections/threads",
      true, 0);
  parser.add<std::string>(
      "duration", 'd', "duration of the test, e.g. 2s, 2m, 2h", false, "15s");
  parser.add<int>("threads", 't', "total number of threads to use", false, 1);
  parser.add<std::string>(
      "header", 'H', "HTTP header to add to request, e.g. \"User-Agent: wrk\"",
      false, "");

  parser.parse_check(argc, argv);

  press_config conf = init_conf(parser);

  // create threads
  std::vector<thread_counter> v;
  std::vector<std::shared_ptr<asio::io_context::work>> works;
  for (int i = 0; i < conf.threads_num; ++i) {
    auto ioc = std::make_shared<asio::io_context>();
    works.push_back(std::make_shared<asio::io_context::work>(*ioc));
    std::thread thd([ioc] {
      ioc->run();
    });
    v.push_back({.thd = std::move(thd), .ioc = ioc});
  }

  // create clients
  async_simple::coro::syncAwait(create_clients(conf, v));

  // create parallel request
  std::vector<async_simple::coro::Lazy<void>> futures;
  std::atomic_bool stop = false;
  for (auto& counter : v) {
    futures.push_back(press(counter, conf.url, stop));
  }

  // start timer
  asio::io_context timer_ioc;
  asio::steady_timer timer(timer_ioc, conf.press_interval);
  timer.async_wait([&stop](std::error_code ec) {
    stop = true;
  });
  std::thread timer_thd([&timer_ioc] {
    timer_ioc.run();
  });

  // wait finish
  async_simple::coro::syncAwait(
      async_simple::coro::collectAll(std::move(futures)));

  timer_thd.join();

  // statistic
  for (auto& counter : v) {
    std::cout << counter.complete << ", " << counter.requests << "\n";
  }

  // stop and clean
  works.clear();
  for (auto& counter : v) {
    if (counter.thd.joinable())
      counter.thd.join();
  }
  return 0;
}