#pragma once

#include <atomic>
#include <charconv>
#include <memory>
#include <string_view>
#include <thread>

#include "asio/io_context.hpp"
#include "asio/ip/tcp.hpp"
#include "asio_util/asio_coro_util.hpp"
#include "async_simple/coro/Lazy.h"
#include "cinatra/define.h"
#include "cinatra/utils.hpp"
#include "http_parser.hpp"
#include "modern_callback.h"
#include "response_cv.hpp"
#include "uri.hpp"

namespace cinatra {
struct resp_data {
  std::error_code net_err;
  status_type status;
  std::string_view resp_body;
  std::vector<std::pair<std::string, std::string>> resp_headers;
};

class coro_http_client {
 public:
  coro_http_client() : socket_(io_ctx_) {
    work_ = std::make_unique<asio::io_context::work>(io_ctx_);
    io_thd_ = std::thread([this] {
      io_ctx_.run();
    });
  }

  ~coro_http_client() {
    close();
    work_ = nullptr;
    if (io_thd_.joinable()) {
      io_thd_.join();
    }

    std::cout << "client quit\n";
  }

  void close() {
    if (has_closed_)
      return;

    io_ctx_.post([this] {
      close_socket();
    });
  }

  bool has_closed() { return has_closed_; }

  void add_header(std::string key, std::string val) {
    if (key.empty())
      return;

    if (key == "Host")
      return;

    req_headers_.emplace_back(std::move(key), std::move(val));
  }

  async_simple::coro::Lazy<bool> async_ping(std::string uri) {
    resp_data data{};
    auto [r, u] = handle_uri(data, uri);
    if (!r) {
      std::cout << "url error";
      co_return false;
    }

    auto ec = co_await asio_util::async_connect(io_ctx_, socket_, u.get_host(),
                                                u.get_port());

    if (ec) {
      std::cout << ec.message() << "\n";
    }

    co_return !ec;
  }

  async_simple::coro::Lazy<resp_data> async_get(std::string uri) {
    return async_request(std::move(uri), http_method::GET, "");
  }

  resp_data get(std::string uri) {
    return async_simple::coro::syncAwait(async_get(std::move(uri)));
  }

  async_simple::coro::Lazy<resp_data> async_post(
      std::string uri, std::string content, req_content_type content_type) {
    return async_request(std::move(uri), http_method::POST, std::move(content),
                         content_type);
  }

  resp_data post(std::string uri, std::string content,
                 req_content_type content_type) {
    return async_simple::coro::syncAwait(
        async_post(std::move(uri), std::move(content), content_type));
  }

  async_simple::coro::Lazy<resp_data> async_request(
      std::string uri, http_method method, std::string content,
      req_content_type conten_type = req_content_type::none, bool is_download_ = false) {
    resp_data data{};
    if (has_closed_) {
      data.net_err = std::make_error_code(std::errc::not_connected);
      data.status = status_type::not_found;
      co_return data;
    }

    std::error_code ec{};
    size_t size = 0;
    http_parser parser;
    bool is_keep_alive = false;

    do {
      if (auto [ok, u] = handle_uri(data, uri); !ok) {
        break;
      }
      else {
        if (ec = co_await asio_util::async_connect(io_ctx_, socket_,
                                                   u.get_host(), u.get_port());
            ec) {
          break;
        }

        std::string write_msg =
            prepare_request_str(u, method, content, conten_type);
        if (std::tie(ec, size) = co_await asio_util::async_write(
                socket_, asio::buffer(write_msg));
            ec) {
          break;
        }
      }

      if (std::tie(ec, size) = co_await asio_util::async_read_until(
              socket_, read_buf_, TWO_CRCF);
          ec) {
        break;
      }

      if (ec = handle_header(data, parser, size); ec) {
        break;
      }

      is_keep_alive = parser.keep_alive();

      size_t content_len = (size_t)parser.body_len();

      if ((size_t)parser.body_len() <= read_buf_.size()) {
        // Now get entire content, additional data will discard.
        handle_entire_content(data, content_len);
        break;
      }

      // read left part of content.
      size_t size_to_read = content_len - read_buf_.size();
      if (std::tie(ec, size) =
              co_await asio_util::async_read(socket_, read_buf_, size_to_read);
          ec) {
        break;
      }

      if (is_download_) {
        auto data_ptr = asio::buffer_cast<const char *>(read_buf_.data());
        download_file_->write(data_ptr, read_buf_.size());
      }

      // Now get entire content, additional data will discard.
      handle_entire_content(data, content_len);
    } while (0);

    handle_result(data, ec, is_keep_alive);

    co_return data;
  }

  async_simple::coro::Lazy<resp_data> async_download(std::string src_file, std::string dest_file, int64_t size) {
    auto parant_path = fs::absolute(dest_file).parent_path();
    std::error_code code;
    fs::create_directories(parant_path, code);
    resp_data data{};
    if (code) {
      data.net_err = std::make_error_code(std::errc::no_such_file_or_directory);
      data.status = status_type::ok;
    }

    download_file_ = std::make_shared<std::ofstream>(
        dest_file, std::ios::binary | std::ios::app);
    if (!download_file_->is_open()) {
      data.net_err = std::make_error_code(std::errc::invalid_argument);
      data.status = status_type::ok;     
    }

    if (size > 0) {
      char buffer[20];
      auto p = i64toa_jeaiii(size, buffer);
      add_header("cinatra_start_pos", std::string(buffer, p - buffer));
    }
    else {
      char buffer[20];
      int64_t file_size = fs::file_size(dest_file, code);
      auto p = i64toa_jeaiii(file_size, buffer);
      add_header("cinatra_start_pos", std::string(buffer, p - buffer));
    }
    return async_request(std::move(src_file), http_method::GET, "",  req_content_type::none, true);
  }

 private:
  std::pair<bool, uri_t> handle_uri(resp_data &data, const std::string &uri) {
    uri_t u;
    if (!u.parse_from(uri.data())) {
      if (!u.schema.empty()) {
        auto new_uri = url_encode(uri);

        if (!u.parse_from(new_uri.data())) {
          data.net_err = std::make_error_code(std::errc::protocol_error);
          data.status = status_type::not_found;
          return {false, {}};
        }
      }
    }

    if (u.schema == "https"sv) {
#ifdef CINATRA_ENABLE_SSL
      // upgrade_to_ssl();
#else
      // please open CINATRA_ENABLE_SSL before request https!
      assert(false);
#endif
    }

    return {true, u};
  }

  std::string prepare_request_str(const uri_t &u, http_method method,
                                  std::string content,
                                  req_content_type content_type) {
    std::string req_str(method_name(method));
    req_str.append(" ").append(u.get_path());
    if (!u.query.empty()) {
      req_str.append("?").append(u.query);
    }
    req_str.append(" HTTP/1.1\r\nHost:").append(u.host).append("\r\n");
    auto type_str = get_content_type_str(content_type);
    if (!type_str.empty()) {
      req_headers_.emplace_back("Content-Type", std::move(type_str));
    }

    bool has_connection = false;
    // add user headers
    if (!req_headers_.empty()) {
      for (auto &pair : req_headers_) {
        if (pair.first == "Connection") {
          has_connection = true;
        }
        req_str.append(pair.first)
            .append(": ")
            .append(pair.second)
            .append("\r\n");
      }
    }

    if (!has_connection) {
      req_str.append("Connection: keep-alive\r\n");
    }

    // add content
    size_t content_len = content.size();
    bool should_add = false;
    if (content_len > 0) {
      should_add = true;
    }
    else {
      if (method == http_method::POST)
        should_add = true;
    }

    if (should_add) {
      char buf[32];
      auto [ptr, ec] = std::to_chars(buf, buf + 32, content_len);
      req_str.append("Content-Length: ")
          .append(std::string_view(buf, ptr - buf))
          .append("\r\n");
    }

    req_str.append("\r\n");

    if (content_len > 0)
      req_str.append(std::move(content));

    return req_str;
  }

  std::error_code handle_header(resp_data &data, http_parser &parser,
                                size_t header_size) {
    // parse header
    const char *data_ptr = asio::buffer_cast<const char *>(read_buf_.data());
    int parse_ret = parser.parse_response(data_ptr, header_size, 0);
    if (parse_ret < 0) {
      return std::make_error_code(std::errc::protocol_error);
    }
    read_buf_.consume(header_size);  // header size
    data.resp_headers = get_headers(parser);
    return {};
  }

  void handle_entire_content(resp_data &data, size_t content_len) {
    if (content_len > 0) {
      assert(content_len == read_buf_.size());
      auto data_ptr = asio::buffer_cast<const char *>(read_buf_.data());
      std::string_view reply(data_ptr, content_len);
      data.resp_body = reply;
      read_buf_.consume(content_len);
    }

    data.status = status_type::ok;
  }

  void handle_result(resp_data &data, std::error_code ec, bool is_keep_alive) {
    if (ec) {
      close_socket();
      data.net_err = ec;
      data.status = status_type::not_found;
      std::cout << ec.message() << "\n";
    }
    else {
      if (!is_keep_alive) {
        close_socket();
      }
    }
  }

  std::vector<std::pair<std::string, std::string>> get_headers(
      http_parser &parser) {
    std::vector<std::pair<std::string, std::string>> resp_headers;

    auto [headers, num_headers] = parser.get_headers();
    for (size_t i = 0; i < num_headers; i++) {
      resp_headers.emplace_back(
          std::string(headers[i].name, headers[i].name_len),
          std::string(headers[i].value, headers[i].value_len));
    }

    return resp_headers;
  }

  void close_socket() {
    std::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);
    has_closed_ = true;
  }

  asio::io_context io_ctx_;
  asio::ip::tcp::socket socket_;
  std::unique_ptr<asio::io_context::work> work_;
  std::thread io_thd_;

  std::atomic<bool> has_closed_;
  asio::streambuf read_buf_;

  std::shared_ptr<std::ofstream> download_file_ = nullptr;

  std::vector<std::pair<std::string, std::string>> req_headers_;
};
}  // namespace cinatra