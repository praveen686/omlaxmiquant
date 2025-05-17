#pragma once

#include <functional>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

#include "common/thread_utils.h"
#include "common/macros.h"
#include "common/logging.h"
#include "common/types.h"
#include "common/lf_queue.h"

#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "market_data/binance_authenticator.h"
#include "market_data/binance_http_client.h"
#include "market_data/binance_config.h"
#include "exchange/market_data/market_update.h"

// Import Common namespace types for convenience
using Common::ClientId;
using Common::OrderId;
using Common::Price;
using Common::Qty;
using Common::Side;
using Common::TickerId;

namespace Trading {

/**
 * @brief Gateway for communicating with Binance for order management
 * 
 * This class handles order submission, cancellation, and status checks
 * with the Binance API via REST.
 */
class BinanceOrderGateway {
public:
    /**
     * @brief Constructor
     * @param client_id Client identifier
     * @param client_requests Queue for outgoing client requests
     * @param client_responses Queue for incoming client responses
     * @param market_data_updates Queue for market data updates
     * @param authenticator Reference to the Binance authenticator
     * @param config Reference to the Binance configuration
     */
    BinanceOrderGateway(ClientId client_id,
                       Exchange::ClientRequestLFQueue *client_requests,
                       Exchange::ClientResponseLFQueue *client_responses,
                       Exchange::MEMarketUpdateLFQueue *market_data_updates,
                       BinanceAuthenticator& authenticator,
                       BinanceConfig& config);
    
    /**
     * @brief Destructor
     */
    ~BinanceOrderGateway();
    
    /**
     * @brief Start the order gateway processing thread
     */
    void start();
    
    /**
     * @brief Stop the order gateway processing thread
     */
    void stop();
    
    // Deleted default, copy & move constructors and assignment operators
    BinanceOrderGateway() = delete;
    BinanceOrderGateway(const BinanceOrderGateway&) = delete;
    BinanceOrderGateway(BinanceOrderGateway&&) = delete;
    BinanceOrderGateway& operator=(const BinanceOrderGateway&) = delete;
    BinanceOrderGateway& operator=(BinanceOrderGateway&&) = delete;
    
private:
    // Client identifier
    const ClientId client_id_;
    
    // Lock-free queues for communication with the trade engine
    Exchange::ClientRequestLFQueue *outgoing_requests_ = nullptr;
    Exchange::ClientResponseLFQueue *incoming_responses_ = nullptr;
    Exchange::MEMarketUpdateLFQueue *market_data_updates_ = nullptr;
    
    // Authenticator for API calls
    BinanceAuthenticator& authenticator_;
    
    // Configuration
    BinanceConfig& config_;
    
    // Logging
    std::string time_str_;
    Common::Logger logger_;
    
    // HTTP client for API calls
    BinanceHttpClient http_client_;
    
    // Thread control
    std::atomic<bool> run_{false};
    std::thread processing_thread_;
    
    // Sequence number tracking
    size_t next_outgoing_seq_num_ = 1;
    size_t next_exp_seq_num_ = 1;
    
    // Order ID mapping
    std::unordered_map<OrderId, std::string> order_id_to_binance_id_;
    std::mutex order_map_mutex_;
    
    // Main processing function
    void processLoop();
    
    // Request handling
    void handleRequest(const Exchange::MEClientRequest& request);
    void handleNewOrderRequest(const Exchange::MEClientRequest& request);
    void handleCancelOrderRequest(const Exchange::MEClientRequest& request);
    
    // Response generation
    void generateAndEnqueueResponse(OrderId order_id, Exchange::ClientResponseType status,
                                  TickerId ticker_id = 0, Side side = Side::INVALID,
                                  Price price = 0, Qty exec_qty = 0, Qty leaves_qty = 0);
                                  
    // Price protection methods
    bool validateOrderPrice(const std::string& symbol, Price price, Side side);
    double getLatestMarketPrice(const std::string& symbol);
    std::string getSymbolForTickerId(TickerId ticker_id);
    nlohmann::json getSymbolInfo(const std::string& symbol);
    
    // Account balance methods
    double getAccountBalance(const std::string& asset);
    double calculateOrderQuantity(const std::string& symbol, double price, Side side);
    
    // Symbol information cache
    std::unordered_map<std::string, nlohmann::json> symbol_info_cache_;
    std::chrono::time_point<std::chrono::system_clock> last_symbol_info_refresh_;
    std::mutex symbol_info_mutex_;
};

} // namespace Trading