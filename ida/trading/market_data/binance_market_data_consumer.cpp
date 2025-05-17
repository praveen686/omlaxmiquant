#include "binance_market_data_consumer.h"
#include "binance_config.h"
#include "binance_http_client.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <nlohmann/json.hpp>

// Use the fully qualified namespace to avoid ambiguity
namespace json_lib = nlohmann;

namespace Trading {

BinanceMarketDataConsumer::BinanceMarketDataConsumer(Common::ClientId client_id, 
                                                   Exchange::MEMarketUpdateLFQueue *market_updates,
                                                   const std::vector<std::string>& symbols,
                                                   bool use_testnet)
    : client_id_(client_id),
      incoming_md_updates_(market_updates),
      symbols_(symbols),
      use_testnet_(use_testnet),
      logger_("/home/praveen/omlaxmiquant/ida/logs/trading_binance_market_data_" + std::to_string(client_id) + ".log") {
    
    // Initialize URL endpoints based on testnet setting
    base_ws_url_ = use_testnet_ ? "stream.testnet.binance.vision" : "stream.binance.com";
    base_rest_url_ = use_testnet_ ? "testnet.binance.vision" : "api.binance.com";
    
    // Map symbols to ticker IDs
    initializeSymbolToTickerMap();
    
    // Create order books for each symbol
    for (const auto& symbol : symbols_) {
        auto ticker_id = symbol_to_ticker_id_[symbol];
        order_books_[symbol] = std::make_unique<BinanceOrderBook>(symbol, ticker_id, logger_);
    }
    
    logger_.log("%:% %() % Initialized BinanceMarketDataConsumer with % symbols, testnet: %\n", 
                __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                symbols_.size(), use_testnet_);
}

BinanceMarketDataConsumer::~BinanceMarketDataConsumer() {
    stop();
}

void BinanceMarketDataConsumer::start() {
    if (run_) {
        logger_.log("%:% %() % Already running\n", 
                    __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        return;
    }
    
    run_ = true;
    
    // Connect to WebSockets for all symbols
    connectToWebSockets();
    
    // Start the snapshot refresh thread
    startSnapshotRefreshThread();
    
    logger_.log("%:% %() % BinanceMarketDataConsumer started\n", 
                __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
}

void BinanceMarketDataConsumer::stop() {
    if (!run_) {
        return;
    }
    
    run_ = false;
    
    // Stop the snapshot refresh thread
    if (snapshot_thread_running_) {
        snapshot_thread_running_ = false;
        snapshot_cv_.notify_all();
        if (snapshot_thread_.joinable()) {
            snapshot_thread_.join();
        }
    }
    
    // Disconnect all WebSocket clients
    for (auto& [symbol, client] : depth_ws_clients_) {
        client->disconnect();
    }
    depth_ws_clients_.clear();
    
    for (auto& [symbol, client] : trade_ws_clients_) {
        client->disconnect();
    }
    trade_ws_clients_.clear();
    
    logger_.log("%:% %() % BinanceMarketDataConsumer stopped\n", 
                __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
}

bool BinanceMarketDataConsumer::isOrderBookValid(const std::string& symbol) const {
    auto it = order_books_.find(symbol);
    if (it == order_books_.end()) {
        return false;
    }
    return it->second->isValid();
}

Common::Price BinanceMarketDataConsumer::getBestBidPrice(const std::string& symbol) const {
    auto it = order_books_.find(symbol);
    if (it == order_books_.end()) {
        return Common::Price_INVALID;
    }
    return it->second->getBestBidPrice();
}

Common::Price BinanceMarketDataConsumer::getBestAskPrice(const std::string& symbol) const {
    auto it = order_books_.find(symbol);
    if (it == order_books_.end()) {
        return Common::Price_INVALID;
    }
    return it->second->getBestAskPrice();
}

void BinanceMarketDataConsumer::initializeSymbolToTickerMap() {
    // In a real implementation, this would map from external symbol names to
    // internal ticker IDs based on configuration. For demo, we'll use simple mapping.
    for (size_t i = 0; i < symbols_.size(); ++i) {
        symbol_to_ticker_id_[symbols_[i]] = static_cast<Common::TickerId>(i + 1);
    }
}

void BinanceMarketDataConsumer::connectToWebSockets() {
    for (const auto& symbol : symbols_) {
        // Create WebSocket client for depth stream
        auto depth_client = std::make_unique<BinanceWebSocketClient>(logger_);
        depth_client->setMaxReconnectAttempts(0);  // Unlimited reconnect attempts
        
        // Create WebSocket client for trade stream
        auto trade_client = std::make_unique<BinanceWebSocketClient>(logger_);
        trade_client->setMaxReconnectAttempts(0);  // Unlimited reconnect attempts
        
        // Convert symbol to lowercase for stream name
        std::string lower_symbol = symbol;
        std::transform(lower_symbol.begin(), lower_symbol.end(), lower_symbol.begin(), 
                      [](unsigned char c) { return std::tolower(c); });
        
        // Connect to depth stream
        std::string depth_target = "/ws/" + lower_symbol + "@depth";
        depth_client->connect(
            base_ws_url_,
            "443",
            depth_target,
            [this, symbol](const std::string& message) {
                this->handleDepthMessage(symbol, message);
            },
            [this, symbol](bool connected) {
                this->handleWebSocketStatus(symbol, connected);
            }
        );
        
        // Connect to trade stream
        std::string trade_target = "/ws/" + lower_symbol + "@trade";
        trade_client->connect(
            base_ws_url_,
            "443",
            trade_target,
            [this, symbol](const std::string& message) {
                this->handleTradeMessage(symbol, message);
            }
        );
        
        // Store the clients
        depth_ws_clients_[symbol] = std::move(depth_client);
        trade_ws_clients_[symbol] = std::move(trade_client);
        
        logger_.log("%:% %() % Connected WebSocket streams for symbol: %\n", 
                    __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                    symbol);
        
        // Initial order book snapshot
        refreshOrderBookSnapshot(symbol);
    }
}

void BinanceMarketDataConsumer::startSnapshotRefreshThread() {
    if (snapshot_thread_running_) {
        return;
    }
    
    snapshot_thread_running_ = true;
    snapshot_thread_ = std::thread([this]() {
        this->snapshotRefreshThreadFunc();
    });
}

void BinanceMarketDataConsumer::snapshotRefreshThreadFunc() {
    logger_.log("%:% %() % Snapshot refresh thread started\n", 
                __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    
    while (snapshot_thread_running_) {
        // Check all order books for refresh needs
        for (const auto& symbol : symbols_) {
            if (!run_) {
                break;
            }
            
            auto book_it = order_books_.find(symbol);
            if (book_it != order_books_.end() && book_it->second->needsRefresh()) {
                logger_.log("%:% %() % Refreshing order book for %\n", 
                            __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                            symbol);
                
                refreshOrderBookSnapshot(symbol);
            }
        }
        
        // Wait for next check interval or notification
        std::unique_lock<std::mutex> lock(snapshot_mutex_);
        snapshot_cv_.wait_for(lock, SNAPSHOT_REFRESH_INTERVAL, [this]() {
            return !snapshot_thread_running_;
        });
    }
    
    logger_.log("%:% %() % Snapshot refresh thread stopped\n", 
                __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
}

void BinanceMarketDataConsumer::refreshOrderBookSnapshot(const std::string& symbol) {
    try {
        // Fetch order book snapshot using REST API
        std::string response = fetchOrderBookSnapshot(symbol, BOOK_UPDATE_DEPTH);
        
        // Parse the response
        json_lib::json snapshot_json = json_lib::json::parse(response);
        
        // Process the order book snapshot
        processOrderBookSnapshot(symbol, snapshot_json);
    }
    catch (const std::exception& e) {
        logger_.log("%:% %() % Error refreshing order book for %: %\n", 
                    __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                    symbol, e.what());
    }
}

void BinanceMarketDataConsumer::handleDepthMessage(const std::string& symbol, const std::string& message) {
    try {
        // Parse the message
        json_lib::json depth_json = json_lib::json::parse(message);
        
        // Process the depth update
        processDepthUpdate(symbol, depth_json);
    }
    catch (const std::exception& e) {
        logger_.log("%:% %() % Error processing depth message for %: %\n", 
                    __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                    symbol, e.what());
    }
}

void BinanceMarketDataConsumer::handleTradeMessage(const std::string& symbol, const std::string& message) {
    try {
        // Parse the message
        json_lib::json trade_json = json_lib::json::parse(message);
        
        // Process the trade update
        processTradeUpdate(symbol, trade_json);
    }
    catch (const std::exception& e) {
        logger_.log("%:% %() % Error processing trade message for %: %\n", 
                    __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                    symbol, e.what());
    }
}

void BinanceMarketDataConsumer::handleWebSocketStatus(const std::string& symbol, bool connected) {
    logger_.log("%:% %() % WebSocket connection status for %: %\n", 
                __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                symbol, (connected ? "Connected" : "Disconnected"));
    
    if (!connected) {
        // Mark order book as needing refresh
        auto book_it = order_books_.find(symbol);
        if (book_it != order_books_.end()) {
            // Wake up the snapshot thread immediately
            std::unique_lock<std::mutex> lock(snapshot_mutex_);
            snapshot_cv_.notify_one();
        }
    }
}

void BinanceMarketDataConsumer::processDepthUpdate(const std::string& symbol, const json_lib::json& json_data) {
    auto book_it = order_books_.find(symbol);
    if (book_it == order_books_.end()) {
        logger_.log("%:% %() % Order book not found for symbol: %\n", 
                    __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                    symbol);
        return;
    }
    
    // Extract data from the message
    uint64_t first_update_id = json_data["U"].get<uint64_t>();
    uint64_t final_update_id = json_data["u"].get<uint64_t>();
    
    // Parse price levels
    std::vector<PriceLevel> bids = parsePriceLevels(json_data["b"]);
    std::vector<PriceLevel> asks = parsePriceLevels(json_data["a"]);
    
    // Apply the update to the order book
    bool success = book_it->second->applyDepthUpdate(
        first_update_id,
        final_update_id,
        bids,
        asks
    );
    
    if (success) {
        // Generate market updates from the updated order book
        std::vector<Exchange::MEMarketUpdate> updates;
        book_it->second->generateMarketUpdates(updates);
        
        // Push updates to the market data queue
        for (const auto& update : updates) {
            auto next_write = incoming_md_updates_->getNextToWriteTo();
            *next_write = update;
            incoming_md_updates_->updateWriteIndex();
        }
    }
}

void BinanceMarketDataConsumer::processTradeUpdate(const std::string& symbol, const json_lib::json& json_data) {
    // Get ticker ID for the symbol
    auto ticker_id_it = symbol_to_ticker_id_.find(symbol);
    if (ticker_id_it == symbol_to_ticker_id_.end()) {
        logger_.log("%:% %() % Ticker ID not found for symbol: %\n", 
                    __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                    symbol);
        return;
    }
    Common::TickerId ticker_id = ticker_id_it->second;
    
    // Extract trade data
    bool is_buyer_maker = json_data["m"].get<bool>();
    Common::Side side = is_buyer_maker ? Common::Side::SELL : Common::Side::BUY;
    std::string price_str = json_data["p"].get<std::string>();
    std::string qty_str = json_data["q"].get<std::string>();
    
    // Convert to internal format
    Common::Price price = stringPriceToInternal(price_str);
    Common::Qty qty = stringQtyToInternal(qty_str);
    
    // Create a trade market update
    Exchange::MEMarketUpdate trade_update;
    trade_update.type_ = Exchange::MarketUpdateType::TRADE;
    trade_update.ticker_id_ = ticker_id;
    trade_update.side_ = side;
    trade_update.price_ = price;
    trade_update.qty_ = qty;
    
    // Push the update to the queue
    auto next_write = incoming_md_updates_->getNextToWriteTo();
    *next_write = trade_update;
    incoming_md_updates_->updateWriteIndex();
}

void BinanceMarketDataConsumer::processOrderBookSnapshot(const std::string& symbol, const json_lib::json& json_data) {
    auto book_it = order_books_.find(symbol);
    if (book_it == order_books_.end()) {
        logger_.log("%:% %() % Order book not found for symbol: %\n", 
                    __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                    symbol);
        return;
    }
    
    // Extract data from the snapshot
    uint64_t last_update_id = json_data["lastUpdateId"].get<uint64_t>();
    
    // Parse bids and asks
    std::vector<PriceLevel> bids = parsePriceLevels(json_data["bids"]);
    std::vector<PriceLevel> asks = parsePriceLevels(json_data["asks"]);
    
    // Apply the snapshot to the order book
    bool success = book_it->second->applySnapshot(
        last_update_id,
        bids,
        asks
    );
    
    if (success) {
        // Generate market updates from the updated order book
        std::vector<Exchange::MEMarketUpdate> updates;
        book_it->second->generateMarketUpdates(updates);
        
        // Push updates to the market data queue
        for (const auto& update : updates) {
            auto next_write = incoming_md_updates_->getNextToWriteTo();
            *next_write = update;
            incoming_md_updates_->updateWriteIndex();
        }
        
        logger_.log("%:% %() % Applied snapshot for % with % bids and % asks\n", 
                    __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                    symbol, bids.size(), asks.size());
    }
}

Common::Price BinanceMarketDataConsumer::stringPriceToInternal(const std::string& price_str) {
    // Convert string price to internal price representation
    // For simplicity, multiply by 10000 to convert to ticks
    // In a real implementation, this would handle decimal places correctly
    try {
        double price_dbl = std::stod(price_str);
        return static_cast<Common::Price>(price_dbl * 10000.0);
    }
    catch (const std::exception& e) {
        logger_.log("%:% %() % Error converting price: %\n", 
                    __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                    e.what());
        return Common::Price_INVALID;
    }
}

Common::Qty BinanceMarketDataConsumer::stringQtyToInternal(const std::string& qty_str) {
    // Convert string quantity to internal quantity representation
    // For simplicity, multiply by 10000 to convert to internal units
    // In a real implementation, this would handle decimal places correctly
    try {
        double qty_dbl = std::stod(qty_str);
        return static_cast<Common::Qty>(qty_dbl * 10000.0);
    }
    catch (const std::exception& e) {
        logger_.log("%:% %() % Error converting qty: %\n", 
                    __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                    e.what());
        return 0;
    }
}

std::vector<PriceLevel> BinanceMarketDataConsumer::parsePriceLevels(const json_lib::json& levels) {
    std::vector<PriceLevel> result;
    
    for (const auto& level : levels) {
        // Each level is [price, quantity]
        std::string price_str = level[0].get<std::string>();
        std::string qty_str = level[1].get<std::string>();
        
        PriceLevel price_level;
        price_level.price = stringPriceToInternal(price_str);
        price_level.quantity = stringQtyToInternal(qty_str);
        
        if (price_level.price != Common::Price_INVALID) {
            result.push_back(price_level);
        }
    }
    
    return result;
}

std::string BinanceMarketDataConsumer::fetchOrderBookSnapshot(const std::string& symbol, int limit) {
    // Create HTTP client
    BinanceHttpClient http_client(logger_);
    
    // Build query parameters
    std::map<std::string, std::string> params = {
        {"symbol", symbol},
        {"limit", std::to_string(limit)}
    };
    
    // Make the request
    return http_client.get(base_rest_url_, "/api/v3/depth", params);
}

} // namespace Trading