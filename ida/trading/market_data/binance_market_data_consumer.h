#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

#include "common/thread_utils.h"
#include "common/lf_queue.h"
#include "common/macros.h"
#include "common/logging.h"
#include "exchange/market_data/market_update.h"
#include "binance_websocket_client.h"
#include "binance_order_book.h"
#include <nlohmann/json.hpp>

// To avoid ambiguity with other possible json namespaces
namespace json_lib = nlohmann;

namespace Trading {

/**
 * @brief Binance market data consumer
 *
 * This class consumes market data from Binance using WebSockets and maintains
 * a local order book for each subscribed symbol. It converts Binance-specific
 * data formats to the internal market update format used by the trading engine.
 */
class BinanceMarketDataConsumer {
public:
    /**
     * @brief Constructor
     * @param client_id Client ID for the trading system
     * @param market_updates Queue for publishing market updates to the trading engine
     * @param symbols List of symbols to subscribe to (e.g., "BTCUSDT")
     * @param use_testnet Whether to use the Binance testnet
     */
    BinanceMarketDataConsumer(Common::ClientId client_id, 
                             Exchange::MEMarketUpdateLFQueue *market_updates,
                             const std::vector<std::string>& symbols,
                             bool use_testnet = false);
    
    /**
     * @brief Destructor
     */
    ~BinanceMarketDataConsumer();
    
    /**
     * @brief Start the market data consumer
     */
    void start();
    
    /**
     * @brief Stop the market data consumer
     */
    void stop();
    
    /**
     * @brief Get the status of a specific symbol's order book
     * @param symbol The symbol to check
     * @return true if the order book is synchronized and valid
     */
    bool isOrderBookValid(const std::string& symbol) const;

    /**
     * @brief Get the best bid price for a symbol
     * @param symbol The symbol to query
     * @return The best bid price or Price_INVALID if not available
     */
    Common::Price getBestBidPrice(const std::string& symbol) const;
    
    /**
     * @brief Get the best ask price for a symbol
     * @param symbol The symbol to query
     * @return The best ask price or Price_INVALID if not available
     */
    Common::Price getBestAskPrice(const std::string& symbol) const;

private:
    // Client and configuration
    Common::ClientId client_id_;
    Exchange::MEMarketUpdateLFQueue *incoming_md_updates_;
    std::vector<std::string> symbols_;
    bool use_testnet_;
    std::atomic<bool> run_{false};
    
    // WebSocket clients for depth and trade streams
    std::map<std::string, std::unique_ptr<BinanceWebSocketClient>> depth_ws_clients_;
    std::map<std::string, std::unique_ptr<BinanceWebSocketClient>> trade_ws_clients_;
    
    // Order books for each symbol
    std::map<std::string, std::unique_ptr<BinanceOrderBook>> order_books_;
    std::map<std::string, Common::TickerId> symbol_to_ticker_id_;
    
    // Snapshot refresh thread and control
    std::thread snapshot_thread_;
    std::mutex snapshot_mutex_;
    std::condition_variable snapshot_cv_;
    std::atomic<bool> snapshot_thread_running_{false};
    
    // Logging
    Logger logger_;
    std::string time_str_;
    
    // Constants
    static constexpr int BOOK_UPDATE_DEPTH = 1000;  // Maximum depth for order book snapshots
    static constexpr auto SNAPSHOT_REFRESH_INTERVAL = std::chrono::seconds(30);
    
    // API endpoints
    std::string base_ws_url_;
    std::string base_rest_url_;
    
    // Internal methods
    void initializeSymbolToTickerMap();
    void connectToWebSockets();
    void startSnapshotRefreshThread();
    void snapshotRefreshThreadFunc();
    void refreshOrderBookSnapshot(const std::string& symbol);
    
    // WebSocket message handlers
    void handleDepthMessage(const std::string& symbol, const std::string& message);
    void handleTradeMessage(const std::string& symbol, const std::string& message);
    void handleWebSocketStatus(const std::string& symbol, bool connected);
    
    // JSON parsing helpers
    void processDepthUpdate(const std::string& symbol, const json_lib::json& json_data);
    void processTradeUpdate(const std::string& symbol, const json_lib::json& json_data);
    void processOrderBookSnapshot(const std::string& symbol, const json_lib::json& json_data);
    
    // Utility functions
    Common::Price stringPriceToInternal(const std::string& price_str);
    Common::Qty stringQtyToInternal(const std::string& qty_str);
    std::vector<PriceLevel> parsePriceLevels(const json_lib::json& levels);
    std::string buildWebSocketUrl(const std::string& stream_name);
    
    // REST API methods
    std::string fetchOrderBookSnapshot(const std::string& symbol, int limit);
};

} // namespace Trading