#pragma once

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <fstream>
#include <mutex>
#include <algorithm>
#include <nlohmann/json.hpp>
#include "common/types.h"
#include "common/logging.h"

namespace Trading {

/**
 * @brief Structure to hold ticker information for Binance
 */
struct BinanceTickerInfo {
    Common::TickerId ticker_id;
    std::string symbol;
    std::string base_asset;
    std::string quote_asset;
    double min_qty;
    double max_qty;
    double step_size;
    double min_notional;
    int price_precision;
    int qty_precision;
    double test_price;
    double test_qty;
};

/**
 * @brief Configuration for Binance API connections
 */
class BinanceConfig {
public:
    /**
     * @brief Constructor with parameters loaded from config file
     * @param logger Reference to a logger instance
     * @param config_path Path to the configuration file
     */
    BinanceConfig(Common::Logger& logger, 
                 const std::string& config_path = "/home/praveen/omlaxmiquant/ida/config/BinanceConfig.json");
    
    /**
     * @brief Load configuration from file
     * @return true if loaded successfully, false otherwise
     */
    bool loadConfig();
    
    // API Keys
    std::string api_key;
    std::string api_secret;
    
    // Environment settings
    bool use_testnet = true;
    
    // Connection settings
    int max_reconnect_attempts = 10;
    int connect_timeout_ms = 5000;
    int read_timeout_ms = 5000;
    
    // Market data settings
    std::vector<std::string> symbols;
    int order_book_depth = 20;
    bool subscribe_to_trades = true;
    
    // Rate limit settings
    int order_rate_limit_per_second = 10;
    int request_rate_limit_per_minute = 1200;
    
    // WebSocket URLs
    std::string getWsBaseUrl() const {
        return use_testnet ? "stream.testnet.binance.vision" : "stream.binance.com";
    }
    
    // REST API URLs
    std::string getRestBaseUrl() const {
        return use_testnet ? "testnet.binance.vision" : "api.binance.com";
    }
    
    // Create URL for depth stream
    std::string getDepthStreamUrl(const std::string& symbol) const {
        std::string lower_symbol = symbol;
        std::transform(lower_symbol.begin(), lower_symbol.end(), lower_symbol.begin(), ::tolower);
        return "/ws/" + lower_symbol + "@depth";
    }
    
    // Create URL for trade stream
    std::string getTradeStreamUrl(const std::string& symbol) const {
        std::string lower_symbol = symbol;
        std::transform(lower_symbol.begin(), lower_symbol.end(), lower_symbol.begin(), ::tolower);
        return "/ws/" + lower_symbol + "@trade";
    }
    
    // Create URL for depth snapshot
    std::string getDepthSnapshotUrl(const std::string& symbol, int limit) const {
        return "/api/v3/depth?symbol=" + symbol + "&limit=" + std::to_string(limit);
    }
    
    /**
     * @brief Check if using testnet
     * @return true if using testnet, false if using mainnet
     */
    bool isUsingTestnet() const { return use_testnet; }
    
    /**
     * @brief Get ticker information by ticker ID
     * @param ticker_id The ticker ID to look up
     * @return BinanceTickerInfo structure for the ticker
     */
    BinanceTickerInfo getTickerInfo(Common::TickerId ticker_id) const;
    
    /**
     * @brief Get ticker information by symbol
     * @param symbol The symbol to look up
     * @return BinanceTickerInfo structure for the ticker
     */
    BinanceTickerInfo getTickerInfoBySymbol(const std::string& symbol) const;
    
    /**
     * @brief Get the symbol for a ticker ID
     * @param ticker_id The ticker ID to look up
     * @return The symbol for the ticker ID
     */
    std::string getSymbolForTickerId(Common::TickerId ticker_id) const;
    
    /**
     * @brief Get the ticker ID for a symbol
     * @param symbol The symbol to look up
     * @return The ticker ID for the symbol
     */
    Common::TickerId getTickerIdForSymbol(const std::string& symbol) const;
    
    /**
     * @brief Get all ticker IDs
     * @return Vector of all ticker IDs
     */
    std::vector<Common::TickerId> getAllTickerIds() const;
    
    /**
     * @brief Get all symbols
     * @return Vector of all symbols
     */
    std::vector<std::string> getAllSymbols() const;
    
    /**
     * @brief Get the client ID
     * @return The configured client ID
     */
    Common::ClientId getClientId() const { return client_id_; }
    
    /**
     * @brief Get the default test order ID
     * @return The default test order ID
     */
    Common::OrderId getDefaultTestOrderId() const { return default_test_order_id_; }
    
    /**
     * @brief Get the default test side
     * @return The default test side
     */
    Common::Side getDefaultTestSide() const { return default_test_side_; }
    
    /**
     * @brief Get the test price multiplier
     * @return The test price multiplier
     */
    double getTestPriceMultiplier() const { return test_price_multiplier_; }
    
    /**
     * @brief Get the test quantity
     * @return The test quantity
     */
    double getTestQty() const { return test_qty_; }
    
    /**
     * @brief Get the symbol info cache duration in minutes
     * @return The symbol info cache duration in minutes
     */
    int getSymbolInfoCacheMinutes() const { return symbol_info_cache_minutes_; }
    
    /**
     * @brief Get the account info cache duration in minutes
     * @return The account info cache duration in minutes
     */
    int getAccountInfoCacheMinutes() const { return account_info_cache_minutes_; }
    
    /**
     * @brief Get the quote asset (base currency for portfolio valuation)
     * @return The quote asset string (e.g., "USDT")
     */
    std::string getQuoteAsset() const { 
        // Look through symbols to find the most common quote asset
        if (!tickers_.empty()) {
            return tickers_[0].quote_asset; 
        }
        return "USDT"; // Default
    }
    
    /**
     * @brief Check if an asset is actively used in our trading
     * @param asset The asset symbol to check
     * @return True if the asset is used in any trading pair
     */
    bool isActiveAsset(const std::string& asset) const {
        for (const auto& ticker : tickers_) {
            if (ticker.base_asset == asset || ticker.quote_asset == asset) {
                return true;
            }
        }
        return false;
    }
    
    /**
     * @brief Get minimum balance threshold for warnings
     * @param asset The asset to get threshold for
     * @return The minimum balance threshold
     */
    double getMinBalanceThreshold(const std::string& asset) const {
        // Default minimum thresholds by asset
        const static std::unordered_map<std::string, double> DEFAULT_MIN_BALANCES = {
            {"BTC", 0.001},
            {"ETH", 0.01},
            {"USDT", 10.0},
            {"BNB", 0.1}
        };
        
        // Check for default threshold
        auto it = DEFAULT_MIN_BALANCES.find(asset);
        if (it != DEFAULT_MIN_BALANCES.end()) {
            return it->second;
        }
        
        // Default fallback - if it's a quote asset, use higher threshold
        if (getQuoteAsset() == asset) {
            return 10.0;
        }
        
        // For other assets, use a small value
        return 0.0001;
    }
    
private:
    /**
     * @brief Create a default ticker info with all fields initialized
     * @return A default BinanceTickerInfo with safe default values
     */
    BinanceTickerInfo createDefaultTickerInfo() const {
        BinanceTickerInfo empty;
        empty.ticker_id = Common::TickerId_INVALID;
        empty.symbol = "";
        empty.base_asset = "";
        empty.quote_asset = "";
        empty.min_qty = 0.00001;
        empty.max_qty = 9000.0;
        empty.step_size = 0.00001;
        empty.min_notional = 5.0;
        empty.price_precision = 2;
        empty.qty_precision = 5;
        empty.test_price = 100000.0;
        empty.test_qty = 0.001;
        return empty;
    }

    // Logger reference
    Common::Logger& logger_;
    std::string time_str_;
    
    // Config file path
    std::string config_path_;
    
    // Ticker information
    std::vector<BinanceTickerInfo> tickers_;
    std::unordered_map<Common::TickerId, size_t> ticker_id_to_index_;
    std::unordered_map<std::string, size_t> symbol_to_index_;
    
    // Order gateway settings
    Common::ClientId client_id_ = 1;
    Common::OrderId default_test_order_id_ = 1001;
    Common::Side default_test_side_ = Common::Side::BUY;
    double test_price_multiplier_ = 0.995;
    double test_qty_ = 0.001;
    
    // Cache settings
    int symbol_info_cache_minutes_ = 60;
    int account_info_cache_minutes_ = 5;
    
    // Mutex for thread safety
    mutable std::mutex mutex_;
    
    // Load status
    bool config_loaded_ = false;
};

} // namespace Trading