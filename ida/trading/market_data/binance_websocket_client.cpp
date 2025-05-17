#include "binance_websocket_client.h"
#include <boost/asio/steady_timer.hpp>

namespace Trading {

BinanceWebSocketClient::BinanceWebSocketClient(Logger& logger)
    : ioc_(), ctx_(ssl::context::tlsv12_client), resolver_(ioc_), logger_(logger) {
    
    // Configure SSL context
    ctx_.set_default_verify_paths();
    ctx_.set_verify_mode(ssl::verify_peer);
}

BinanceWebSocketClient::~BinanceWebSocketClient() {
    disconnect();
}

bool BinanceWebSocketClient::connect(const std::string& host, 
                                   const std::string& port, 
                                   const std::string& target,
                                   MessageCallback message_callback,
                                   StatusCallback status_callback) {
    if (running_) {
        logger_.log("%:% %() % Already connected or connecting\n", 
                    __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        return false;
    }
    
    host_ = host;
    port_ = port;
    target_ = target;
    message_callback_ = message_callback;
    status_callback_ = status_callback;
    
    // Reset state
    reconnect_attempts_ = 0;
    reconnect_delay_ = std::chrono::milliseconds(1000);  // Reset to initial delay
    running_ = true;
    connected_ = false;
    reconnecting_ = false;
    
    // Start the IO context in its own thread
    io_thread_ = std::thread([this]() { runIoContext(); });
    
    logger_.log("%:% %() % Starting connection to %:% %\n", 
                __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                host_, port_, target_);
                
    // Start the connection process
    resolver_.async_resolve(
        host_,
        port_,
        beast::bind_front_handler(&BinanceWebSocketClient::onResolve, this)
    );
    
    return true;
}

void BinanceWebSocketClient::disconnect() {
    if (running_) {
        running_ = false;
        
        // Cancel any pending operations
        if (ws_) {
            beast::error_code ec;
            ws_->next_layer().next_layer().cancel(ec);
        }
        
        // Stop the IO context
        ioc_.stop();
        
        // Wait for the thread to exit
        if (io_thread_.joinable()) {
            io_thread_.join();
        }
        
        logger_.log("%:% %() % Disconnected from %:%\n", 
                   __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                   host_, port_);
                   
        // Update connection status
        if (connected_) {
            connected_ = false;
            if (status_callback_) {
                status_callback_(false);
            }
        }
        
        // Clear send queue
        std::queue<std::string> empty;
        std::swap(send_queue_, empty);
    }
}

bool BinanceWebSocketClient::send(const std::string& message) {
    if (!connected_) {
        logger_.log("%:% %() % Cannot send message, not connected\n", 
                   __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        return false;
    }
    
    std::lock_guard<std::mutex> lock(send_mutex_);
    send_queue_.push(message);
    
    if (send_queue_.size() == 1) {
        // If this was the only message in the queue, start the write process
        net::post(
            ioc_,
            beast::bind_front_handler(&BinanceWebSocketClient::processWriteQueue, this)
        );
    }
    
    return true;
}

void BinanceWebSocketClient::runIoContext() {
    while (running_) {
        try {
            ioc_.run();
            // If we get here, the io_context has run out of work
            if (running_) {
                // Reset the io_context so it can be used again
                ioc_.restart();
            } else {
                break;
            }
        } catch (const std::exception& e) {
            logger_.log("%:% %() % Exception in IO thread: %\n", 
                       __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                       e.what());
            
            if (running_ && !reconnecting_) {
                scheduleReconnect();
            }
        }
    }
}

void BinanceWebSocketClient::onResolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
        logger_.log("%:% %() % Resolve error: %\n", 
                   __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                   ec.message());
        
        if (running_) {
            scheduleReconnect();
        }
        return;
    }
    
    // Create the WebSocket stream with SSL
    ws_ = std::make_unique<websocket::stream<beast::ssl_stream<tcp::socket>>>(ioc_, ctx_);
    
    // Set SNI hostname for SSL
    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) {
        ec = beast::error_code(static_cast<int>(::ERR_get_error()),
                          net::error::get_ssl_category());
        logger_.log("%:% %() % SSL SNI error: %\n", 
                   __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                   ec.message());
        
        if (running_) {
            scheduleReconnect();
        }
        return;
    }
    
    // Set various options for the websocket
    ws_->set_option(websocket::stream_base::decorator(
        [](websocket::request_type& req) {
            req.set(http::field::user_agent, 
                    std::string(BOOST_BEAST_VERSION_STRING) + " Binance WebSocket Client");
        }));
    
    // For Boost 1.83+, we connect directly with a specific endpoint from the results
    auto it = results.begin();
    
    // Connect directly to the endpoint
    beast::get_lowest_layer(*ws_).async_connect(
        *it,
        [this](const beast::error_code& ec) {
            this->onConnect(ec, tcp::endpoint());
        }
    );
}

void BinanceWebSocketClient::onConnect(beast::error_code ec, tcp::endpoint endpoint) {
    if (ec) {
        logger_.log("%:% %() % Connect error: %\n", 
                   __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                   ec.message());
        
        if (running_) {
            scheduleReconnect();
        }
        return;
    }
    
    logger_.log("%:% %() % Connected to %:%\n", 
               __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
               endpoint.address().to_string(), endpoint.port());
    
    // Perform the SSL handshake
    ws_->next_layer().async_handshake(
        ssl::stream_base::client,
        beast::bind_front_handler(&BinanceWebSocketClient::onSslHandshake, this)
    );
}

void BinanceWebSocketClient::onSslHandshake(beast::error_code ec) {
    if (ec) {
        logger_.log("%:% %() % SSL handshake error: %\n", 
                   __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                   ec.message());
        
        if (running_) {
            scheduleReconnect();
        }
        return;
    }
    
    logger_.log("%:% %() % SSL handshake complete\n", 
               __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    
    // Timeout handling is done via our reconnection logic
    
    // Set suggested timeout settings for the websocket
    ws_->set_option(websocket::stream_base::timeout::suggested(
        beast::role_type::client));
    
    // Headers already set during connection setup
    
    // Perform the WebSocket handshake
    ws_->async_handshake(
        host_, 
        target_,
        beast::bind_front_handler(&BinanceWebSocketClient::onHandshake, this)
    );
}

void BinanceWebSocketClient::onHandshake(beast::error_code ec) {
    if (ec) {
        logger_.log("%:% %() % WebSocket handshake error: %\n", 
                   __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                   ec.message());
        
        if (running_) {
            scheduleReconnect();
        }
        return;
    }
    
    logger_.log("%:% %() % WebSocket handshake complete for %\n", 
               __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
               target_);
    
    // Connection established
    connected_ = true;
    reconnect_attempts_ = 0;
    reconnect_delay_ = std::chrono::milliseconds(1000);  // Reset to initial delay
    reconnecting_ = false;
    
    // Notify status change
    if (status_callback_) {
        status_callback_(true);
    }
    
    // Start reading
    doRead();
}

void BinanceWebSocketClient::doRead() {
    if (!connected_ || !running_) {
        return;
    }
    
    // Read a message
    ws_->async_read(
        buffer_,
        beast::bind_front_handler(&BinanceWebSocketClient::onRead, this)
    );
}

void BinanceWebSocketClient::onRead(beast::error_code ec, std::size_t /*bytes_transferred*/) {
    if (ec) {
        if (ec == websocket::error::closed) {
            logger_.log("%:% %() % WebSocket closed by server\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        } else {
            logger_.log("%:% %() % WebSocket read error: %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      ec.message());
        }
        
        connected_ = false;
        if (status_callback_) {
            status_callback_(false);
        }
        
        if (running_) {
            scheduleReconnect();
        }
        return;
    }
    
    // Get the message as a string
    std::string message = beast::buffers_to_string(buffer_.data());
    
    // Clear the buffer
    buffer_.consume(buffer_.size());
    
    // Process the message
    if (message_callback_) {
        try {
            message_callback_(message);
        } catch (const std::exception& e) {
            logger_.log("%:% %() % Exception in message callback: %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      e.what());
        }
    }
    
    // Continue reading
    doRead();
}

void BinanceWebSocketClient::processWriteQueue() {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (send_queue_.empty() || !connected_ || !running_) {
        return;
    }
    
    doWrite(send_queue_.front());
}

void BinanceWebSocketClient::doWrite(std::string message) {
    ws_->async_write(
        net::buffer(message),
        beast::bind_front_handler(&BinanceWebSocketClient::onWrite, this)
    );
}

void BinanceWebSocketClient::onWrite(beast::error_code ec, std::size_t /*bytes_transferred*/) {
    if (ec) {
        logger_.log("%:% %() % WebSocket write error: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  ec.message());
        
        connected_ = false;
        if (status_callback_) {
            status_callback_(false);
        }
        
        if (running_) {
            scheduleReconnect();
        }
        return;
    }
    
    std::lock_guard<std::mutex> lock(send_mutex_);
    send_queue_.pop();
    
    if (!send_queue_.empty()) {
        // There are more messages to send
        doWrite(send_queue_.front());
    }
}

void BinanceWebSocketClient::doClose() {
    if (!ws_) {
        return;
    }
    
    if (connected_) {
        connected_ = false;
        if (status_callback_) {
            status_callback_(false);
        }
        
        // Close the WebSocket connection
        ws_->async_close(
            websocket::close_code::normal,
            beast::bind_front_handler(&BinanceWebSocketClient::onClose, this)
        );
    }
}

void BinanceWebSocketClient::onClose(beast::error_code ec) {
    if (ec) {
        logger_.log("%:% %() % WebSocket close error: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  ec.message());
    } else {
        logger_.log("%:% %() % WebSocket closed normally\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    }
    
    ws_.reset();
}

void BinanceWebSocketClient::scheduleReconnect() {
    if (!running_ || reconnecting_) {
        return;
    }
    
    reconnecting_ = true;
    
    // Check if we've exceeded maximum reconnect attempts
    if (max_reconnect_attempts_ > 0 && reconnect_attempts_ >= max_reconnect_attempts_) {
        logger_.log("%:% %() % Maximum reconnect attempts (%d) exceeded\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  max_reconnect_attempts_);
        running_ = false;
        return;
    }
    
    // Close any existing connection
    if (ws_) {
        try {
            doClose();
        } catch (...) {
            // Ignore any exceptions during close
            ws_.reset();
        }
    }
    
    // Increment reconnect attempts
    reconnect_attempts_++;
    
    // Calculate exponential backoff delay (1s, 2s, 4s, 8s, 16s, max 30s)
    auto delay = std::min(reconnect_delay_, std::chrono::milliseconds(30000));
    
    logger_.log("%:% %() % Scheduling reconnect attempt %d in %d ms\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
              reconnect_attempts_, delay.count());
    
    // Create a timer for the reconnect delay
    auto timer = std::make_shared<net::steady_timer>(ioc_, delay);
    timer->async_wait([this, timer](beast::error_code ec) {
        if (ec || !running_) {
            return;
        }
        
        // Double the delay for next time
        reconnect_delay_ *= 2;
        
        reconnect();
    });
}

void BinanceWebSocketClient::reconnect() {
    if (!running_) {
        reconnecting_ = false;
        return;
    }
    
    logger_.log("%:% %() % Attempting to reconnect to %:% %\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
              host_, port_, target_);
    
    // Start the connection process again
    resolver_.async_resolve(
        host_,
        port_,
        beast::bind_front_handler(&BinanceWebSocketClient::onResolve, this)
    );
}

} // namespace Trading