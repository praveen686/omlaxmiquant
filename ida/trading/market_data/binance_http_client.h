#pragma once

#include <string>
#include <map>
#include <memory>
#include <functional>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>

#include "common/logging.h"

using Logger = Common::Logger;

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace Trading {

/**
 * @brief HTTP client for Binance REST API
 *
 * This class handles HTTP requests to the Binance REST API, managing connection
 * pooling, request queuing, and error handling.
 */
class BinanceHttpClient {
public:
    /**
     * @brief Constructor
     * @param logger Reference to the logger instance
     */
    BinanceHttpClient(Logger& logger);
    
    /**
     * @brief Destructor
     */
    ~BinanceHttpClient();
    
    /**
     * @brief Perform a GET request
     * @param host The host to connect to (e.g., "api.binance.com")
     * @param target The request target (e.g., "/api/v3/depth")
     * @param query_params Map of query parameters
     * @param headers Additional HTTP headers
     * @param timeout_ms Request timeout in milliseconds
     * @return The response body as a string
     * @throws std::runtime_error if the request fails
     */
    std::string get(const std::string& host,
                   const std::string& target,
                   const std::map<std::string, std::string>& query_params = {},
                   const std::map<std::string, std::string>& headers = {},
                   int timeout_ms = 5000);
    
    /**
     * @brief Perform a POST request
     * @param host The host to connect to (e.g., "api.binance.com")
     * @param target The request target (e.g., "/api/v3/order")
     * @param body The request body
     * @param query_params Map of query parameters
     * @param headers Additional HTTP headers
     * @param timeout_ms Request timeout in milliseconds
     * @return The response body as a string
     * @throws std::runtime_error if the request fails
     */
    std::string post(const std::string& host,
                    const std::string& target,
                    const std::string& body,
                    const std::map<std::string, std::string>& query_params = {},
                    const std::map<std::string, std::string>& headers = {},
                    int timeout_ms = 5000);
    
    /**
     * @brief Perform a DELETE request
     * @param host The host to connect to (e.g., "api.binance.com")
     * @param target The request target (e.g., "/api/v3/order")
     * @param query_params Map of query parameters
     * @param headers Additional HTTP headers
     * @param timeout_ms Request timeout in milliseconds
     * @return The response body as a string
     * @throws std::runtime_error if the request fails
     */
    std::string del(const std::string& host,
                   const std::string& target,
                   const std::map<std::string, std::string>& query_params = {},
                   const std::map<std::string, std::string>& headers = {},
                   int timeout_ms = 5000);

private:
    // IO context and SSL context
    net::io_context ioc_;
    ssl::context ctx_{ssl::context::tlsv12_client};
    
    // Logging
    Logger& logger_;
    std::string time_str_;
    
    // Helper methods
    std::string buildQueryString(const std::map<std::string, std::string>& params);
    std::string sendRequest(http::verb method,
                          const std::string& host,
                          const std::string& target,
                          const std::map<std::string, std::string>& query_params,
                          const std::map<std::string, std::string>& headers,
                          const std::string& body,
                          int timeout_ms);
    
    // Connection handling
    tcp::resolver::results_type resolveHost(const std::string& host);
    std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> connectToHost(
        const std::string& host, const tcp::resolver::results_type& results, int timeout_ms);
};

} // namespace Trading