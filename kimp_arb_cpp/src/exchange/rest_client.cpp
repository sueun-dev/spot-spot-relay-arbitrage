#include "kimp/exchange/exchange_base.hpp"
#include "kimp/core/logger.hpp"

#include <boost/asio/ssl/error.hpp>

namespace kimp::exchange {

HttpResponse RestClient::get(const std::string& target,
                              const std::unordered_map<std::string, std::string>& headers) {
    return do_request(http::verb::get, target, "", headers);
}

HttpResponse RestClient::post(const std::string& target,
                               const std::string& body,
                               const std::unordered_map<std::string, std::string>& headers) {
    return do_request(http::verb::post, target, body, headers);
}

HttpResponse RestClient::del(const std::string& target,
                              const std::unordered_map<std::string, std::string>& headers) {
    return do_request(http::verb::delete_, target, "", headers);
}

std::future<HttpResponse> RestClient::get_async(const std::string& target,
                                                  const std::unordered_map<std::string, std::string>& headers) {
    return std::async(std::launch::async, [this, target, headers]() {
        return get(target, headers);
    });
}

std::future<HttpResponse> RestClient::post_async(const std::string& target,
                                                   const std::string& body,
                                                   const std::unordered_map<std::string, std::string>& headers) {
    return std::async(std::launch::async, [this, target, body, headers]() {
        return post(target, body, headers);
    });
}

HttpResponse RestClient::do_request(http::verb method,
                                     const std::string& target,
                                     const std::string& body,
                                     const std::unordered_map<std::string, std::string>& headers) {
    static constexpr int MAX_RETRIES = 3;  // 1 initial + 2 retries
    HttpResponse response;

    for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
        response = {};

        // Acquire connection from pool
        network::PooledConnectionGuard conn_guard(*connection_pool_, std::chrono::milliseconds(200));

        if (!conn_guard) {
            response.error = "Failed to acquire connection from pool";
            Logger::error("Connection pool exhausted for {}", host_);
            if (attempt < MAX_RETRIES - 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            return response;
        }

        auto* conn = conn_guard.get();
        auto* stream = conn->stream();

        if (!stream) {
            response.error = "Invalid stream in pooled connection";
            conn_guard.mark_failed();
            if (attempt < MAX_RETRIES - 1) continue;
            return response;
        }

        try {
            // Build request with keep-alive
            http::request<http::string_body> req{method, target, 11};
            req.set(http::field::host, host_);
            req.set(http::field::user_agent, "KIMP-Bot/1.0");
            req.set(http::field::connection, "keep-alive");

            for (const auto& [key, value] : headers) {
                req.set(key, value);
            }

            if (!body.empty()) {
                req.body() = body;
                req.prepare_payload();
                if (headers.find("Content-Type") == headers.end()) {
                    req.set(http::field::content_type, "application/json");
                }
            }

            beast::get_lowest_layer(*stream).expires_after(std::chrono::seconds(10));
            http::write(*stream, req);

            beast::flat_buffer buffer;
            http::response<http::string_body> res;
            http::read(*stream, buffer, res);

            response.status_code = res.result_int();
            response.body = res.body();
            response.success = (res.result() == http::status::ok ||
                               res.result() == http::status::created);

            for (const auto& field : res) {
                response.headers[std::string(field.name_string())] = std::string(field.value());
            }

            auto conn_header = res[http::field::connection];
            if (conn_header == "close") {
                conn_guard.mark_failed();
            }

            if (!response.success) {
                Logger::warn("HTTP {} {} - Status: {} Body: {}",
                            method == http::verb::get ? "GET" :
                            method == http::verb::post ? "POST" : "DELETE",
                            target, response.status_code, response.body);
            }

            return response;  // Success — no retry needed

        } catch (const boost::system::system_error& e) {
            response.error = e.what();
            conn_guard.mark_failed();

            bool retriable = (e.code() == boost::asio::error::connection_reset ||
                              e.code() == boost::asio::error::broken_pipe ||
                              e.code() == boost::asio::error::eof ||
                              e.code() == boost::beast::http::error::end_of_stream ||
                              e.code() == boost::asio::ssl::error::stream_truncated);

            if (retriable && attempt < MAX_RETRIES - 1) {
                Logger::warn("Connection lost ({}), retry {}/{}...", e.what(), attempt + 2, MAX_RETRIES);
                continue;  // mark_failed already replaced connection synchronously
            }

            Logger::error("HTTP request failed: {}", e.what());

        } catch (const std::exception& e) {
            response.error = e.what();
            conn_guard.mark_failed();
            Logger::error("HTTP request failed: {}", e.what());
        }
    }

    return response;
}

} // namespace kimp::exchange
