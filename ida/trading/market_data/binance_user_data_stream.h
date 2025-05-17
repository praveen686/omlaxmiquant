#pragma once

#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "binance_websocket_client.h"
#include "binance_http_client.h"
#include "binance_authenticator.h"
#include "binance_config.h"
#include "common/logging.h"
#include "common/types.h"

namespace Trading {

/**
 * @brief Class for handling Binance user data stream via WebSocket
 *
 * This class manages the WebSocket connection to Binance's user data stream,
 * which provides real-time updates for account and order status changes.
 */
class BinanceUserDataStream {
public:
    /**
     * @brief Constructor for BinanceUserDataStream
     * @param logger Reference to the logger instance
     * @param authenticator Pointer to the authenticator for API authentication
     * @param config Binance configuration
     * @param callback Function to handle user data updates
     */
    BinanceUserDataStream(
        Common::Logger& logger,
        std::shared_ptr<BinanceAuthenticator> authenticator,
        const BinanceConfig& config,
        std::function<void(const std::string&)> callback);

    /**
     * @brief Destructor - ensures proper cleanup of resources
     */
    ~BinanceUserDataStream();

    /**
     * @brief Start the user data stream
     * @return True if started successfully, false otherwise
     */
    bool start();

    /**
     * @brief Stop the user data stream
     */
    void stop();

    /**
     * @brief Check if the user data stream is running
     * @return True if running, false otherwise
     */
    bool isRunning() const { return running_; }

private:
    /**
     * @brief Creates a new listen key for the user data stream
     * @return The listen key string
     */
    std::string createListenKey();

    /**
     * @brief Extends the validity of the current listen key
     * @return True if successful, false otherwise
     */
    bool keepAliveListenKey();

    /**
     * @brief Close the current listen key
     * @return True if successful, false otherwise
     */
    bool closeListenKey();

    /**
     * @brief Thread function for listen key keep-alive
     */
    void keepAliveThread();

    /**
     * @brief Handle WebSocket connection state change
     * @param connected True if connected, false if disconnected
     */
    void onConnectionStateChange(bool connected);

    /**
     * @brief Handle WebSocket message
     * @param message The message received from the WebSocket
     */
    void onMessage(const std::string& message);

    // Logger reference
    Common::Logger& logger_;
    std::string time_str_;

    // Authenticator for API calls
    std::shared_ptr<BinanceAuthenticator> authenticator_;

    // Configuration
    const BinanceConfig& config_;

    // HTTP client for REST API calls
    std::unique_ptr<BinanceHttpClient> http_client_;

    // WebSocket client for user data stream
    std::unique_ptr<BinanceWebSocketClient> ws_client_;

    // Current listen key
    std::string listen_key_;
    std::mutex listen_key_mutex_;

    // User data callback
    std::function<void(const std::string&)> user_data_callback_;

    // Keep-alive thread and control
    std::thread keep_alive_thread_;
    std::atomic<bool> running_;
    std::mutex keep_alive_mutex_;
    std::condition_variable keep_alive_cv_;

    // Reconnection attempt tracking
    int reconnect_attempts_;
    const int max_reconnect_attempts_;
    const int keep_alive_interval_ms_;
};

} // namespace Trading