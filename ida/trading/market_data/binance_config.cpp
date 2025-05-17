#include "binance_config.h"
#include <stdexcept>

namespace Trading {

BinanceConfig::BinanceConfig(Common::Logger& logger, const std::string& config_path)
    : logger_(logger), config_path_(config_path) {
    
    // Load the configuration from the file
    loadConfig();
}

bool BinanceConfig::loadConfig() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        logger_.log("%:% %() % Loading Binance configuration from: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  config_path_);
        
        // Open the config file
        std::ifstream config_file(config_path_);
        if (!config_file.is_open()) {
            logger_.log("%:% %() % Failed to open config file: %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      config_path_);
            return false;
        }
        
        // Parse the JSON
        nlohmann::json config_json;
        config_file >> config_json;
        
        // Check if Binance configuration exists
        if (!config_json.contains("binance")) {
            logger_.log("%:% %() % Config file does not contain 'binance' section\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
            return false;
        }
        
        // Get the Binance configuration section
        const auto& binance_config = config_json["binance"];
        
        // Load use_testnet flag
        if (binance_config.contains("use_testnet")) {
            use_testnet = binance_config["use_testnet"].get<bool>();
        }
        
        // Load tickers
        if (binance_config.contains("tickers") && binance_config["tickers"].is_array()) {
            // Clear any existing ticker information
            tickers_.clear();
            ticker_id_to_index_.clear();
            symbol_to_index_.clear();
            symbols.clear();
            
            // Process each ticker
            for (const auto& ticker_json : binance_config["tickers"]) {
                BinanceTickerInfo ticker_info;
                
                // Load required fields
                ticker_info.ticker_id = ticker_json["ticker_id"].get<Common::TickerId>();
                ticker_info.symbol = ticker_json["symbol"].get<std::string>();
                ticker_info.base_asset = ticker_json["base_asset"].get<std::string>();
                ticker_info.quote_asset = ticker_json["quote_asset"].get<std::string>();
                
                // Load optional fields with defaults
                ticker_info.min_qty = ticker_json.value("min_qty", 0.00001);
                ticker_info.max_qty = ticker_json.value("max_qty", 9000.0);
                ticker_info.step_size = ticker_json.value("step_size", 0.00001);
                ticker_info.min_notional = ticker_json.value("min_notional", 5.0);
                ticker_info.price_precision = ticker_json.value("price_precision", 2);
                ticker_info.qty_precision = ticker_json.value("qty_precision", 5);
                ticker_info.test_price = ticker_json.value("test_price", 100000.0);
                ticker_info.test_qty = ticker_json.value("test_qty", 0.001);
                
                // Add to our collections
                size_t index = tickers_.size();
                tickers_.push_back(ticker_info);
                ticker_id_to_index_[ticker_info.ticker_id] = index;
                symbol_to_index_[ticker_info.symbol] = index;
                symbols.push_back(ticker_info.symbol);
                
                logger_.log("%:% %() % Loaded ticker: id=%, symbol=%, base=%, quote=%\n", 
                          __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                          ticker_info.ticker_id, ticker_info.symbol, 
                          ticker_info.base_asset, ticker_info.quote_asset);
            }
        }
        
        // Load order gateway settings
        if (binance_config.contains("order_gateway")) {
            const auto& gw_config = binance_config["order_gateway"];
            
            if (gw_config.contains("client_id")) {
                client_id_ = gw_config["client_id"].get<Common::ClientId>();
            }
            
            if (gw_config.contains("default_test_order_id")) {
                default_test_order_id_ = gw_config["default_test_order_id"].get<Common::OrderId>();
            }
            
            if (gw_config.contains("default_test_side")) {
                std::string side_str = gw_config["default_test_side"].get<std::string>();
                if (side_str == "BUY") {
                    default_test_side_ = Common::Side::BUY;
                } else if (side_str == "SELL") {
                    default_test_side_ = Common::Side::SELL;
                }
            }
            
            if (gw_config.contains("test_price_multiplier")) {
                test_price_multiplier_ = gw_config["test_price_multiplier"].get<double>();
            }
            
            if (gw_config.contains("test_qty")) {
                test_qty_ = gw_config["test_qty"].get<double>();
            }
        }
        
        // Load cache settings
        if (binance_config.contains("cache_settings")) {
            const auto& cache_config = binance_config["cache_settings"];
            
            if (cache_config.contains("symbol_info_cache_minutes")) {
                symbol_info_cache_minutes_ = cache_config["symbol_info_cache_minutes"].get<int>();
            }
            
            if (cache_config.contains("account_info_cache_minutes")) {
                account_info_cache_minutes_ = cache_config["account_info_cache_minutes"].get<int>();
            }
        }
        
        config_loaded_ = true;
        logger_.log("%:% %() % Successfully loaded Binance configuration with % tickers\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  tickers_.size());
        
        return true;
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Exception while loading config: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  e.what());
        config_loaded_ = false;
        return false;
    }
}

BinanceTickerInfo BinanceConfig::getTickerInfo(Common::TickerId ticker_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = ticker_id_to_index_.find(ticker_id);
    if (it != ticker_id_to_index_.end() && it->second < tickers_.size()) {
        return tickers_[it->second];
    }
    
    // Return a default ticker info if not found
    return createDefaultTickerInfo();
}

BinanceTickerInfo BinanceConfig::getTickerInfoBySymbol(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = symbol_to_index_.find(symbol);
    if (it != symbol_to_index_.end() && it->second < tickers_.size()) {
        return tickers_[it->second];
    }
    
    // Return a default ticker info if not found
    return createDefaultTickerInfo();
}

std::string BinanceConfig::getSymbolForTickerId(Common::TickerId ticker_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = ticker_id_to_index_.find(ticker_id);
    if (it != ticker_id_to_index_.end() && it->second < tickers_.size()) {
        return tickers_[it->second].symbol;
    }
    
    return "BTCUSDT"; // Default to BTCUSDT if not found
}

Common::TickerId BinanceConfig::getTickerIdForSymbol(const std::string& symbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = symbol_to_index_.find(symbol);
    if (it != symbol_to_index_.end() && it->second < tickers_.size()) {
        return tickers_[it->second].ticker_id;
    }
    
    return Common::TickerId_INVALID;
}

std::vector<Common::TickerId> BinanceConfig::getAllTickerIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<Common::TickerId> ticker_ids;
    ticker_ids.reserve(tickers_.size());
    
    for (const auto& ticker : tickers_) {
        ticker_ids.push_back(ticker.ticker_id);
    }
    
    return ticker_ids;
}

std::vector<std::string> BinanceConfig::getAllSymbols() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::string> symbols;
    symbols.reserve(tickers_.size());
    
    for (const auto& ticker : tickers_) {
        symbols.push_back(ticker.symbol);
    }
    
    return symbols;
}

} // namespace Trading