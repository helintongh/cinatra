#include "../include/cinatra.hpp"
#include "async_simple/Try.h"
#include "async_simple/Collect.h"
#include "async_simple/Executor.h"
#include "config.h"
#include "cxxopts.hpp"
#include "utils.h"

#include <iostream>
#include <memory>
#include <vector>
#include <thread>

int parse_command(int argc, char *argv[], coro_config *out_config) {
    try {
        std::unique_ptr<cxxopts::Options> allocated(new cxxopts::Options(argv[0], " - coro_wrk command line options"));
        auto &options = *allocated;
        options
            .positional_help("[optional args]")
            .show_positional_help();
        
        options
            .set_width(70)
            .set_tab_expansion()
            .allow_unrecognised_options()
            .add_options()
            ("h,help", "Print help")
            ("c,connections", "Number of threads to use (concurrent connections)", cxxopts::value<int>()->default_value("10"))
            ("d,duration", "Duration of test in seconds", cxxopts::value<int>()->default_value("10"));

        auto result = options.parse(argc, argv);
        if (result.count("help")) {
            std::cout << options.help() << std::endl;
            return 0;
        }

        (*out_config).coroutines = result["connections"].as<int>();

        (*out_config).duration = result["duration"].as<int>();

    } catch (const cxxopts::exceptions::exception& e) {
        std::cout << "error parsing options: " << e.what() << std::endl;
        return -1;
    }
    return 0;
}

async_simple::coro::Lazy<int> task1(int x) {
    co_return x;
}

async_simple::coro::Lazy<cinatra::resp_data> do_one_request(std::string url, cinatra::coro_http_client client) {
    /*
    struct ValueAwaiter {
        cinatra::resp_data resp;
        ValueAwaiter() {}

        bool await_ready() { return false; }
        void await_suspend(std::coroutine_handle<> continuation) noexcept {
            std::thread([c = std::move(continuation)]() mutable {
                c.resume();
            }).detach();
        }
        cinatra::resp_data await_resume() noexcept { return resp; }
    };

    auto res = co_await ValueAwaiter(client.async_get(url));
    co_return res;
    */
    
    cinatra::resp_data resp;
    resp = async_simple::coro::syncAwait(client->async_get(url));
    co_return resp;
}

async_simple::coro::Lazy<void> run_request_loop(std::string url, const std::vector<cinatra::coro_http_client*>& clients) {

    std::vector<async_simple::coro::Lazy<cinatra::resp_data>> input;
    input.push_back(clients[0]->async_get(url));
    input.push_back(clients[1]->async_get(url));
    input.push_back(clients[2]->async_get(url));
    /*
    auto it = clients.begin();
    for (it; it != clients.end(); it++) {
        input.push_back(do_one_request(url, *it));
    }*/

    std::vector<async_simple::Try<cinatra::resp_data>> out = co_await async_simple::collectAll(std::move(input));

    if (out.begin() != out.end()) {
        std::cout << out[0].value().status << std::endl;
    }

}

int main(int argc,  char* argv[])
{
    std::string test_url = "";

    request_stats g_req_stats = {
        .total_resp_size = 0,
        .number_requests = 0,
        .number_errors = 0
    };

    coro_config g_config = {
        .coroutines = 0,
        .duration = 0,
        .timeout_duration = 0,
        .stats = &g_req_stats
    };

    if (argc < 2) {
        std::cout << "parameter should more than two" << std::endl;
        return 0;
    }
    test_url = argv[argc - 1];

    //if (!is_valid_url(test_url))
    //{
    //    std::cout << "url not valid" << std::endl;
    //    return 0;
    //}

    std::cout << test_url << std::endl;

    if (parse_command(argc, argv, &g_config) != 0) {
        return 0;
    }

    int x = 2;
    task1(x).start([](async_simple::Try<int> Result){
        std::cout << Result.value() << std::endl;
    });

    std::vector<cinatra::coro_http_client*> client_lists;

    for (int i = 0; i < g_config.coroutines; i++) {
        cinatra::coro_http_client* temp_client = new cinatra::coro_http_client;
        client_lists.push_back(temp_client);
    }
    //async_simple::executors::SimpleExecutor e1(10);

    async_simple::coro::syncAwait(run_request_loop(test_url, client_lists));
    int i = 10;
    // while condiion change to timer
    while (i > 0) {
        /*
        auto it = client_lists.begin();
        for (it; it != client_lists.end(); it++) {
        }*/
        
        i--;
    }
/*
    for (int i = 0; i < g_config.coroutines; i++) {
        cinatra::coro_http_client *temp = client_lists[i];
        delete temp;
    }
    client_lists.clear();
*/   

    return 0;
}