#include "binance_user_data_stream.h"
#include <nlohmann/json.hpp>
#include <chrono>

namespace Trading {

BinanceUserDataStream::BinanceUserDataStream(
    Common::Logger& logger,
    std::shared_ptr<BinanceAuthenticator> authenticator,
    const BinanceConfig& config,
    std::function<void(const std::string&)> callback)
    : logger_(logger),
      authenticator_(authenticator),
      config_(config),
      user_data_callback_(callback),
      running_(false),
      reconnect_attempts_(0),
      max_reconnect_attempts_(config.max_reconnect_attempts),
      keep_alive_interval_ms_(30 * 60 * 1000) { // 30 minutes (Binance requires keepalive every 60 minutes)
    
    // Create HTTP client for REST API calls
    http_client_ = std::make_unique<BinanceHttpClient>(logger_);
    
    // Create WebSocket client for user data stream
    ws_client_ = std::make_unique<BinanceWebSocketClient>(logger_);
}

BinanceUserDataStream::~BinanceUserDataStream() {
    stop();
}

bool BinanceUserDataStream::start() {
    if (running_) {
        logger_.log("%:% %() % User data stream already running\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        return true;
    }
    
    // Get a listen key from Binance
    listen_key_ = createListenKey();
    if (listen_key_.empty()) {
        logger_.log("%:% %() % Failed to create listen key\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        return false;
    }
    
    logger_.log("%:% %() % Created listen key: %\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
              listen_key_);
    
    // Construct the WebSocket URL and parameters
    std::string host = config_.getWsBaseUrl();
    std::string port = "443"; // Default HTTPS port
    std::string target = "/ws/" + listen_key_;
    
    // Connect to the WebSocket with our callbacks
    if (!ws_client_->connect(
            host, port, target,
            std::bind(&BinanceUserDataStream::onMessage, this, std::placeholders::_1),
            std::bind(&BinanceUserDataStream::onConnectionStateChange, this, std::placeholders::_1))) {
        logger_.log("%:% %() % Failed to connect to user data stream WebSocket\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        return false;
    }
    
    // Start the keep-alive thread
    running_ = true;
    keep_alive_thread_ = std::thread(&BinanceUserDataStream::keepAliveThread, this);
    
    logger_.log("%:% %() % User data stream started\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    
    return true;
}

void BinanceUserDataStream::stop() {
    if (!running_) {
        return;
    }
    
    // Stop the keep-alive thread
    {
        std::lock_guard<std::mutex> lock(keep_alive_mutex_);
        running_ = false;
        keep_alive_cv_.notify_all();
    }
    
    // Wait for the keep-alive thread to finish
    if (keep_alive_thread_.joinable()) {
        keep_alive_thread_.join();
    }
    
    // Close the listen key
    closeListenKey();
    
    // Disconnect the WebSocket
    ws_client_->disconnect();
    
    logger_.log("%:% %() % User data stream stopped\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
}

std::string BinanceUserDataStream::createListenKey() {
    // Create a listen key using the Binance API
    try {
        // Prepare headers with API key
        std::map<std::string, std::string> headers;
        authenticator_->addAuthHeaders(headers);
        
        // userDataStream endpoint uses API key but doesn't require a signature
        std::string response = http_client_->post(
            authenticator_->getRestBaseUrl(),
            "/api/v3/userDataStream",
            "",  // No body
            {},  // No query parameters
            headers
        );
        
        // Parse the response
        try {
            // Parse the response to get the listen key
            nlohmann::json json_response = nlohmann::json::parse(response);
            return json_response["listenKey"].get<std::string>();
        } catch (const std::exception& e) {
            logger_.log("%:% %() % Failed to parse listen key response: %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      e.what());
            return "";
        }
        
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Exception while creating listen key: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), e.what());
        return "";
    }
}

bool BinanceUserDataStream::keepAliveListenKey() {
    // Keep-alive the listen key using the Binance API
    try {
        // Prepare headers with API key
        std::map<std::string, std::string> headers;
        authenticator_->addAuthHeaders(headers);
        
        // Prepare query parameters
        std::map<std::string, std::string> query_params = {
            {"listenKey", listen_key_}
        };
        
        // userDataStream endpoint uses API key but doesn't require a signature
        std::string response = http_client_->put(
            authenticator_->getRestBaseUrl(),
            "/api/v3/userDataStream",
            "",  // No body
            query_params,
            headers
        );
        
        // Success is indicated by an empty JSON object response
        try {
            nlohmann::json json_response = nlohmann::json::parse(response);
            if (!json_response.empty() && json_response.contains("code")) {
                logger_.log("%:% %() % Failed to keep-alive listen key: % %\n", 
                          __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                          json_response["code"].get<int>(), json_response["msg"].get<std::string>());
                return false;
            }
        } catch (const std::exception& e) {
            logger_.log("%:% %() % Failed to parse keep-alive response: %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      e.what());
            return false;
        }
        
        logger_.log("%:% %() % Successfully extended listen key validity\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        
        return true;
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Exception while keeping listen key alive: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), e.what());
        return false;
    }
}

bool BinanceUserDataStream::closeListenKey() {
    // Close the listen key using the Binance API
    try {
        std::lock_guard<std::mutex> lock(listen_key_mutex_);
        
        if (listen_key_.empty()) {
            return true; // Nothing to close
        }
        
        // Prepare headers with API key
        std::map<std::string, std::string> headers;
        authenticator_->addAuthHeaders(headers);
        
        // Prepare query parameters
        std::map<std::string, std::string> query_params = {
            {"listenKey", listen_key_}
        };
        
        // userDataStream endpoint uses API key but doesn't require a signature
        std::string response = http_client_->del(
            authenticator_->getRestBaseUrl(),
            "/api/v3/userDataStream",
            query_params,
            headers
        );
        
        // Success is indicated by an empty JSON object response
        try {
            nlohmann::json json_response = nlohmann::json::parse(response);
            if (!json_response.empty() && json_response.contains("code")) {
                logger_.log("%:% %() % Failed to close listen key: % %\n", 
                          __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                          json_response["code"].get<int>(), json_response["msg"].get<std::string>());
                return false;
            }
        } catch (const std::exception& e) {
            logger_.log("%:% %() % Failed to parse close listen key response: %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      e.what());
            return false;
        }
        
        logger_.log("%:% %() % Successfully closed listen key\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        
        listen_key_ = "";
        return true;
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Exception while closing listen key: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), e.what());
        return false;
    }
}

void BinanceUserDataStream::keepAliveThread() {
    logger_.log("%:% %() % Keep-alive thread started\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    
    while (running_) {
        // Wait for the keep-alive interval or until we're told to stop
        {
            std::unique_lock<std::mutex> lock(keep_alive_mutex_);
            keep_alive_cv_.wait_for(lock, std::chrono::milliseconds(keep_alive_interval_ms_), 
                                [this] { return !running_; });
            
            if (!running_) {
                break;
            }
        }
        
        // Extend the listen key validity
        if (!keepAliveListenKey()) {
            logger_.log("%:% %() % Failed to keep listen key alive, reconnecting...\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
            
            // Try to reconnect
            reconnect_attempts_++;
            if (reconnect_attempts_ > max_reconnect_attempts_) {
                logger_.log("%:% %() % Max reconnection attempts reached, stopping user data stream\n", 
                          __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
                running_ = false;
                break;
            }
            
            // Reconnect to the user data stream
            ws_client_->disconnect();
            
            // Get a new listen key
            std::string new_key = createListenKey();
            if (new_key.empty()) {
                // Failed to get a new listen key, try again in the next iteration
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }
            
            {
                std::lock_guard<std::mutex> lock(listen_key_mutex_);
                listen_key_ = new_key;
            }
            
            // Connect to the WebSocket with the new listen key
            std::string host = config_.getWsBaseUrl();
            std::string port = "443"; // Default HTTPS port
            std::string target = "/ws/" + listen_key_;
            
            // Connect to the WebSocket with our callbacks
            if (!ws_client_->connect(
                    host, port, target,
                    std::bind(&BinanceUserDataStream::onMessage, this, std::placeholders::_1),
                    std::bind(&BinanceUserDataStream::onConnectionStateChange, this, std::placeholders::_1))) {
                logger_.log("%:% %() % Failed to reconnect to user data stream WebSocket\n", 
                          __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
                // Try again in the next iteration
                std::this_thread::sleep_for(std::chrono::seconds(5));
                continue;
            }
            
            logger_.log("%:% %() % Successfully reconnected to user data stream\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        } else {
            // Reset reconnection attempts on successful keep-alive
            reconnect_attempts_ = 0;
        }
    }
    
    logger_.log("%:% %() % Keep-alive thread stopped\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
}

void BinanceUserDataStream::onConnectionStateChange(bool connected) {
    if (connected) {
        logger_.log("%:% %() % Connected to user data stream WebSocket\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
                  
        // Reset reconnection attempts on successful connection
        reconnect_attempts_ = 0;
        
        // Schedule a keep-alive right away to ensure listen key is valid
        // This is useful especially after reconnection
        if (running_) {
            std::thread([this]() {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (running_) {
                    keepAliveListenKey();
                }
            }).detach();
        }
    } else {
        logger_.log("%:% %() % Disconnected from user data stream WebSocket\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        
        // Only attempt to reconnect if we're supposed to be running
        if (running_) {
            reconnect_attempts_++;
            
            // Use exponential backoff for reconnection attempts
            int backoff_seconds = std::min(30, static_cast<int>(std::pow(2, reconnect_attempts_ - 1)));
            
            if (reconnect_attempts_ > max_reconnect_attempts_) {
                logger_.log("%:% %() % Max reconnection attempts reached, stopping user data stream\n", 
                          __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
                running_ = false;
                
                // Notify client application about critical connection failure
                if (user_data_callback_) {
                    nlohmann::json failure_notification = {
                        {"event", "connection_failure"},
                        {"error", "Max reconnection attempts reached"},
                        {"reconnect_attempts", reconnect_attempts_},
                        {"max_attempts", max_reconnect_attempts_}
                    };
                    try {
                        user_data_callback_(failure_notification.dump());
                    } catch (const std::exception& e) {
                        logger_.log("%:% %() % Exception in failure notification callback: %\n", 
                                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), e.what());
                    }
                }
                return;
            }
            
            logger_.log("%:% %() % Attempting to reconnect to user data stream (attempt %/%) after backoff of %s\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      reconnect_attempts_, max_reconnect_attempts_, backoff_seconds);
            
            // Use a detached thread for reconnection with backoff
            std::thread([this, backoff_seconds]() {
                std::this_thread::sleep_for(std::chrono::seconds(backoff_seconds));
                
                // Check if we're still supposed to be running after the backoff
                if (!running_) {
                    return;
                }
                
                // Get a new listen key
                std::string new_key = createListenKey();
                if (new_key.empty()) {
                    logger_.log("%:% %() % Failed to create new listen key for reconnection\n", 
                              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
                    
                    // Trigger another connection state change to retry
                    onConnectionStateChange(false);
                    return;
                }
                
                {
                    std::lock_guard<std::mutex> lock(listen_key_mutex_);
                    listen_key_ = new_key;
                }
                
                // Connect to the WebSocket with the new listen key
                std::string host = config_.getWsBaseUrl();
                std::string port = "443"; // Default HTTPS port
                std::string target = "/ws/" + listen_key_;
                
                // Connect to the WebSocket with our callbacks
                if (!ws_client_->connect(
                        host, port, target,
                        std::bind(&BinanceUserDataStream::onMessage, this, std::placeholders::_1),
                        std::bind(&BinanceUserDataStream::onConnectionStateChange, this, std::placeholders::_1))) {
                    logger_.log("%:% %() % Failed to reconnect to user data stream WebSocket\n", 
                              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
                    
                    // Trigger another connection state change to retry
                    onConnectionStateChange(false);
                    return;
                }
            }).detach();
        }
    }
}

void BinanceUserDataStream::onMessage(const std::string& message) {
    // Process the user data stream message
    try {
        logger_.log("%:% %() % Received user data: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  message);
        
        // Forward the message to the callback
        if (user_data_callback_) {
            user_data_callback_(message);
        }
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Exception while processing user data message: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), e.what());
    }
}

} // namespace Trading