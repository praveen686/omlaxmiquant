#include "binance_http_client.h"
#include <sstream>
#include <boost/asio/strand.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/http/read.hpp>
#include <iostream>

namespace Trading {

BinanceHttpClient::BinanceHttpClient(Logger& logger)
    : logger_(logger) {
    
    // Configure SSL context
    ctx_.set_default_verify_paths();
    ctx_.set_verify_mode(ssl::verify_peer);
    // Allow self-signed certificates and other non-verified certs
    ctx_.set_verify_callback([](bool /*preverified*/, ssl::verify_context&) { return true; });
}

BinanceHttpClient::~BinanceHttpClient() {
    // Cleanup any resources
    ioc_.stop();
}

std::string BinanceHttpClient::get(const std::string& host,
                                 const std::string& target,
                                 const std::map<std::string, std::string>& query_params,
                                 const std::map<std::string, std::string>& headers,
                                 int timeout_ms) {
    return sendRequest(http::verb::get, host, target, query_params, headers, "", timeout_ms);
}

std::string BinanceHttpClient::post(const std::string& host,
                                  const std::string& target,
                                  const std::string& body,
                                  const std::map<std::string, std::string>& query_params,
                                  const std::map<std::string, std::string>& headers,
                                  int timeout_ms) {
    return sendRequest(http::verb::post, host, target, query_params, headers, body, timeout_ms);
}

std::string BinanceHttpClient::del(const std::string& host,
                                 const std::string& target,
                                 const std::map<std::string, std::string>& query_params,
                                 const std::map<std::string, std::string>& headers,
                                 int timeout_ms) {
    return sendRequest(http::verb::delete_, host, target, query_params, headers, "", timeout_ms);
}

std::string BinanceHttpClient::buildQueryString(const std::map<std::string, std::string>& params) {
    if (params.empty()) {
        return "";
    }
    
    std::stringstream ss;
    bool first = true;
    
    for (const auto& param : params) {
        if (!first) {
            ss << "&";
        } else {
            first = false;
        }
        
        ss << param.first << "=" << param.second;
    }
    
    return ss.str();
}

std::string BinanceHttpClient::sendRequest(http::verb method,
                                        const std::string& host,
                                        const std::string& target,
                                        const std::map<std::string, std::string>& query_params,
                                        const std::map<std::string, std::string>& headers,
                                        const std::string& body,
                                        int timeout_ms) {
    try {
        // Reset the io_context for each request to avoid resource exhaustion
        ioc_.restart();
        
        // Create a connection to the host
        auto results = resolveHost(host);
        auto stream = connectToHost(host, results, timeout_ms);
        
        // Build the target with query string
        std::string full_target = target;
        std::string query_string = buildQueryString(query_params);
        if (!query_string.empty()) {
            full_target += (target.find('?') != std::string::npos) ? "&" : "?";
            full_target += query_string;
        }
        
        // Set up an HTTP request message
        http::request<http::string_body> req{method, full_target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.set(http::field::accept, "*/*");
        req.set(http::field::connection, "close");  // Request server to close connection after response
        
        // Add headers
        for (const auto& header : headers) {
            req.set(header.first, header.second);
        }
        
        // Set the body for POST requests
        if (!body.empty()) {
            req.set(http::field::content_type, "application/json");
            req.set(http::field::content_length, std::to_string(body.size()));
            req.body() = body;
        }
        
        // Set timeouts
        beast::get_lowest_layer(*stream).expires_after(std::chrono::milliseconds(timeout_ms));
        
        // Send the HTTP request to the remote host
        http::write(*stream, req);
        
        // Receive the HTTP response
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        
        // Set a timeout for reading
        beast::get_lowest_layer(*stream).expires_after(std::chrono::milliseconds(timeout_ms));
        
        // Read the response
        http::read(*stream, buffer, res);
        
        // Log the response status and body
        logger_.log("%:% %() % HTTP response: % % - Body: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  res.result_int(), res.reason(), res.body());
        
        // Don't check shutdown errors - just log them
        beast::error_code ec;
        stream->shutdown(ec);
        
        if (ec && ec != beast::errc::not_connected) {
            logger_.log("%:% %() % Shutdown notice (non-critical): %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      ec.message());
        }
        
        // Check if we got a successful response
        if (res.result() != http::status::ok) {
            logger_.log("%:% %() % HTTP request failed: % % %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      res.result_int(), res.reason(), res.body());
            throw std::runtime_error("HTTP error: " + std::to_string(res.result_int()) + " " + std::string(res.reason()));
        }
        
        return res.body();
    }
    catch (std::exception const& e) {
        logger_.log("%:% %() % Exception during HTTP request: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  e.what());
        throw std::runtime_error("HTTP request failed: " + std::string(e.what()));
    }
}

tcp::resolver::results_type BinanceHttpClient::resolveHost(const std::string& host) {
    tcp::resolver resolver(ioc_);
    
    logger_.log("%:% %() % Resolving host: %\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
              host);
    
    beast::error_code ec;
    auto results = resolver.resolve(host, "https", ec);
    
    if (ec) {
        logger_.log("%:% %() % Failed to resolve host: % - %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  host, ec.message());
        throw std::runtime_error("Failed to resolve host: " + ec.message());
    }
    
    return results;
}

std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> BinanceHttpClient::connectToHost(
    const std::string& host, const tcp::resolver::results_type& results, int timeout_ms) {
    
    // Create the stream
    auto stream = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(ioc_, ctx_);
    
    // Set SNI hostname (required for SNI)
    if (!SSL_set_tlsext_host_name(stream->native_handle(), host.c_str())) {
        beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
        throw beast::system_error{ec};
    }
    
    // Set the timeout
    beast::get_lowest_layer(*stream).expires_after(std::chrono::milliseconds(timeout_ms));
    
    // Make the connection on the IP address we get from a lookup
    beast::error_code ec;
    beast::get_lowest_layer(*stream).connect(results, ec);
    
    if (ec) {
        logger_.log("%:% %() % Failed to connect to host: % - %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  host, ec.message());
        throw std::runtime_error("Failed to connect to host: " + ec.message());
    }
    
    // Set timeout for the handshake
    beast::get_lowest_layer(*stream).expires_after(std::chrono::milliseconds(timeout_ms));
    
    // Perform the SSL handshake
    stream->handshake(ssl::stream_base::client, ec);
    
    if (ec) {
        logger_.log("%:% %() % Failed SSL handshake with host: % - %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  host, ec.message());
        throw std::runtime_error("Failed SSL handshake: " + ec.message());
    }
    
    logger_.log("%:% %() % Connected to host: %\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
              host);
    
    return stream;
}

} // namespace Trading