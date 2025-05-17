#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <chrono>

// Boost.Beast includes
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>

#include "common/logging.h"

using Logger = Common::Logger;

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace Trading {

/**
 * @brief Callback type for WebSocket message handling
 */
using MessageCallback = std::function<void(const std::string&)>;

/**
 * @brief Callback type for WebSocket connection status changes
 */
using StatusCallback = std::function<void(bool connected)>;

/**
 * @brief WebSocket client for Binance market data streams
 *
 * This class manages WebSocket connections to Binance using Boost.Beast,
 * providing automatic reconnection with exponential backoff and
 * asynchronous message processing.
 */
class BinanceWebSocketClient {
public:
    /**
     * @brief Constructor
     * @param logger Reference to the logger
     */
    BinanceWebSocketClient(Logger& logger);
    
    /**
     * @brief Destructor
     */
    ~BinanceWebSocketClient();
    
    /**
     * @brief Connect to a Binance WebSocket stream
     * @param host Host name (e.g., "stream.binance.com")
     * @param port Port number (e.g., "443")
     * @param target Path and query for the WebSocket (e.g., "/ws/btcusdt@depth")
     * @param message_callback Callback for received messages
     * @param status_callback Callback for connection status changes
     * @return true if connection process started successfully
     */
    bool connect(const std::string& host, 
                const std::string& port, 
                const std::string& target,
                MessageCallback message_callback,
                StatusCallback status_callback = nullptr);
    
    /**
     * @brief Disconnect from the WebSocket server
     */
    void disconnect();
    
    /**
     * @brief Check if client is connected
     * @return true if connected
     */
    bool isConnected() const { return connected_; }
    
    /**
     * @brief Set the max reconnect attempts
     * @param attempts Maximum number of reconnect attempts (0 = unlimited)
     */
    void setMaxReconnectAttempts(int attempts) { max_reconnect_attempts_ = attempts; }
    
    /**
     * @brief Send a message to the WebSocket server
     * @param message Message to send
     * @return true if message was queued for sending
     */
    bool send(const std::string& message);

private:
    // Boost.ASIO context and objects
    net::io_context ioc_;
    ssl::context ctx_{ssl::context::tlsv12_client};
    std::unique_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;
    tcp::resolver resolver_;
    
    // Connection parameters
    std::string host_;
    std::string port_;
    std::string target_;
    
    // Message and status callbacks
    MessageCallback message_callback_;
    StatusCallback status_callback_;
    
    // Connection management
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> reconnecting_{false};
    int reconnect_attempts_{0};
    int max_reconnect_attempts_{10};  // 0 = unlimited
    std::chrono::milliseconds reconnect_delay_{1000};  // Initial delay in ms
    
    // Threading
    std::thread io_thread_;
    std::mutex send_mutex_;
    std::queue<std::string> send_queue_;
    
    // Receive buffer for WebSocket
    beast::flat_buffer buffer_;
    
    // Logging
    Logger& logger_;
    std::string time_str_;
    
    // Internal methods for connection handling
    void runIoContext();
    void onResolve(beast::error_code ec, tcp::resolver::results_type results);
    void onConnect(beast::error_code ec, tcp::endpoint endpoint);
    void onSslHandshake(beast::error_code ec);
    void onHandshake(beast::error_code ec);
    void doRead();
    void onRead(beast::error_code ec, std::size_t bytes_transferred);
    void processWriteQueue();
    void doWrite(std::string message);
    void onWrite(beast::error_code ec, std::size_t bytes_transferred);
    void doClose();
    void onClose(beast::error_code ec);
    void scheduleReconnect();
    void reconnect();
};

} // namespace Trading