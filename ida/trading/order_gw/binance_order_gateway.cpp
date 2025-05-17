#include "binance_order_gateway.h"
#include "market_data/binance_types.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>  // For std::setprecision

namespace Trading {

BinanceOrderGateway::BinanceOrderGateway(ClientId client_id,
                                       Exchange::ClientRequestLFQueue *client_requests,
                                       Exchange::ClientResponseLFQueue *client_responses,
                                       Exchange::MEMarketUpdateLFQueue *market_data_updates,
                                       BinanceAuthenticator& authenticator,
                                       BinanceConfig& config)
    : client_id_(client_id),
      outgoing_requests_(client_requests),
      incoming_responses_(client_responses),
      market_data_updates_(market_data_updates),
      authenticator_(authenticator),
      config_(config),
      logger_("/home/praveen/omlaxmiquant/ida/logs/trading_binance_order_gateway_" + std::to_string(client_id) + ".log"),
      http_client_(logger_) {
    
    logger_.log("%:% %() % Initialized BinanceOrderGateway for client % using %\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
              client_id_, config.isUsingTestnet() ? "testnet" : "mainnet");
}

BinanceOrderGateway::~BinanceOrderGateway() {
    stop();
}

void BinanceOrderGateway::start() {
    if (run_) {
        logger_.log("%:% %() % Already running\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        return;
    }
    
    // Verify that we have valid credentials
    if (!authenticator_.hasValidCredentials()) {
        logger_.log("%:% %() % ERROR: Cannot start - no valid API credentials\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        return;
    }
    
    // Start the user data stream
    startUserDataStream();
    
    run_ = true;
    processing_thread_ = std::thread(&BinanceOrderGateway::processLoop, this);
    
    logger_.log("%:% %() % Started BinanceOrderGateway for client %\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
              client_id_);
}

void BinanceOrderGateway::stop() {
    if (!run_) {
        return;
    }
    
    run_ = false;
    
    // Stop the user data stream
    stopUserDataStream();
    
    if (processing_thread_.joinable()) {
        processing_thread_.join();
    }
    
    logger_.log("%:% %() % Stopped BinanceOrderGateway for client %\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
              client_id_);
}

void BinanceOrderGateway::processLoop() {
    logger_.log("%:% %() % Processing loop started for client %\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
              client_id_);
    
    while (run_) {
        // Process outgoing requests from the trade engine
        for (auto client_request = outgoing_requests_->getNextToRead();
             client_request;
             client_request = outgoing_requests_->getNextToRead()) {
            
            logger_.log("%:% %() % Processing request: %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      client_request->toString());
            
            // Forward the request to the appropriate handler
            handleRequest(*client_request);
            
            outgoing_requests_->updateReadIndex();
        }
        
        // Sleep a bit to prevent high CPU usage
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void BinanceOrderGateway::handleRequest(const Exchange::MEClientRequest& request) {
    switch (request.type_) {
        case Exchange::ClientRequestType::NEW:
            handleNewOrderRequest(request);
            break;
            
        case Exchange::ClientRequestType::CANCEL:
            handleCancelOrderRequest(request);
            break;
            
        default:
            logger_.log("%:% %() % Unsupported request type: %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      static_cast<int>(request.type_));
            generateAndEnqueueResponse(request.order_id_, Exchange::ClientResponseType::CANCEL_REJECTED);
            break;
    }
}

void BinanceOrderGateway::handleNewOrderRequest(const Exchange::MEClientRequest& request) {
    logger_.log("%:% %() % Handling new order request: %\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
              request.toString());
    
    try {
        // Get the symbol from ticker_id
        std::string symbol = getSymbolForTickerId(request.ticker_id_);
        
        // Convert internal scaled values to actual decimal values for Binance
        // Convert internal price to Binance decimal format
        double price_decimal = Binance::internalPriceToBinance(request.price_);
        
        // Get a suitable quantity for the order based on account balance
        // instead of using the quantity from the request
        double qty_decimal = calculateOrderQuantity(symbol, price_decimal, request.side_);
        
        // Original quantity from request (for logging)
        double original_qty_decimal = Binance::internalQtyToBinance(request.qty_);
        
        logger_.log("%:% %() % Converting internal values: price=%d -> %f, original qty=%d -> %f, calculated qty=%f\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  request.price_, price_decimal, request.qty_, original_qty_decimal, qty_decimal);
        
        // Validate the order price against current market conditions and exchange rules
        if (!validateOrderPrice(symbol, request.price_, request.side_)) {
            logger_.log("%:% %() % Order price validation failed for symbol=%, price=%, side=%\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      symbol, price_decimal, Common::sideToString(request.side_));
            
            // Generate rejection response
            generateAndEnqueueResponse(
                request.order_id_,
                Exchange::ClientResponseType::CANCEL_REJECTED,
                request.ticker_id_,
                request.side_
            );
            
            return;
        }
        
        // Format the price and quantity according to Binance's requirements
        // Get symbol info to check price precision requirements
        auto symbol_info = getSymbolInfo(symbol);
        std::string formatted_price = std::to_string(price_decimal);
        std::string formatted_qty = std::to_string(qty_decimal);
        
        // Get tick size from PRICE_FILTER to format price correctly
        if (!symbol_info.empty() && symbol_info.contains("filters") && symbol_info["filters"].is_array()) {
            for (const auto& filter : symbol_info["filters"]) {
                if (filter.contains("filterType") && filter["filterType"] == "PRICE_FILTER") {
                    // Get the tick size to determine decimal precision
                    std::string tick_size_str;
                    if (filter["tickSize"].is_string()) {
                        tick_size_str = filter["tickSize"].get<std::string>();
                    } else if (filter["tickSize"].is_number()) {
                        tick_size_str = std::to_string(filter["tickSize"].get<double>());
                    }
                    
                    // Parse tick size to determine decimal places
                    if (!tick_size_str.empty()) {
                        // Count decimal places in tick size
                        size_t decimal_pos = tick_size_str.find('.');
                        if (decimal_pos != std::string::npos) {
                            size_t decimal_places = tick_size_str.size() - decimal_pos - 1;
                            while (decimal_places > 0 && tick_size_str.back() == '0') {
                                tick_size_str.pop_back();
                                decimal_places--;
                            }
                            
                            // Format price with appropriate precision
                            std::ostringstream price_stream;
                            price_stream << std::fixed << std::setprecision(decimal_places) << price_decimal;
                            formatted_price = price_stream.str();
                            
                            logger_.log("%:% %() % Using tick size % to format price to % decimal places: %\n", 
                                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                                      tick_size_str, decimal_places, formatted_price);
                        }
                    }
                    
                    // Get min and max price for validation
                    double min_price = 0.0;
                    double max_price = 0.0;
                    
                    if (filter.contains("minPrice")) {
                        if (filter["minPrice"].is_string()) {
                            min_price = std::stod(filter["minPrice"].get<std::string>());
                        } else if (filter["minPrice"].is_number()) {
                            min_price = filter["minPrice"].get<double>();
                        }
                    }
                    
                    if (filter.contains("maxPrice")) {
                        if (filter["maxPrice"].is_string()) {
                            max_price = std::stod(filter["maxPrice"].get<std::string>());
                        } else if (filter["maxPrice"].is_number()) {
                            max_price = filter["maxPrice"].get<double>();
                        }
                    }
                    
                    // Validate price against min and max
                    if (price_decimal < min_price) {
                        logger_.log("%:% %() % Price % is below minimum allowed price %\n", 
                                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                                  price_decimal, min_price);
                        price_decimal = min_price;
                    }
                    
                    if (max_price > 0 && price_decimal > max_price) {
                        logger_.log("%:% %() % Price % is above maximum allowed price %\n", 
                                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                                  price_decimal, max_price);
                        price_decimal = max_price;
                    }
                }
                
                if (filter.contains("filterType") && filter["filterType"] == "LOT_SIZE") {
                    // Handle quantity precision similarly
                    std::string step_size_str;
                    if (filter["stepSize"].is_string()) {
                        step_size_str = filter["stepSize"].get<std::string>();
                    } else if (filter["stepSize"].is_number()) {
                        step_size_str = std::to_string(filter["stepSize"].get<double>());
                    }
                    
                    if (!step_size_str.empty()) {
                        size_t decimal_pos = step_size_str.find('.');
                        if (decimal_pos != std::string::npos) {
                            size_t decimal_places = step_size_str.size() - decimal_pos - 1;
                            while (decimal_places > 0 && step_size_str.back() == '0') {
                                step_size_str.pop_back();
                                decimal_places--;
                            }
                            
                            std::ostringstream qty_stream;
                            qty_stream << std::fixed << std::setprecision(decimal_places) << qty_decimal;
                            formatted_qty = qty_stream.str();
                            
                            logger_.log("%:% %() % Using step size % to format quantity to % decimal places: %\n", 
                                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                                      step_size_str, decimal_places, formatted_qty);
                        }
                    }
                }
            }
        }
        
        // Prepare the parameters for the order
        // Use format "x-<order_id>" for client order ID to track our internal order ID
        std::string client_order_id = "x-" + std::to_string(request.order_id_);
        
        std::map<std::string, std::string> params = {
            {"symbol", symbol},
            {"side", request.side_ == Side::BUY ? "BUY" : "SELL"},
            {"type", "LIMIT"}, // Default to LIMIT
            {"timeInForce", "GTC"}, // Good Till Cancel
            {"quantity", formatted_qty},
            {"price", formatted_price},
            {"newClientOrderId", client_order_id} // Add our custom client order ID
        };
        
        logger_.log("%:% %() % Order parameters: symbol=%, side=%, quantity=%, price=%\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  symbol, params["side"], params["quantity"], params["price"]);
        
        // Sign the request
        std::string signed_query = authenticator_.signRequest(params);
        
        // Prepare headers with authentication
        std::map<std::string, std::string> headers;
        authenticator_.addAuthHeaders(headers);
        
        // Log the request details for debugging
        logger_.log("%:% %() % Making order request to: %/api/v3/order?%\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  authenticator_.getRestBaseUrl(), signed_query);
                  
        // Make the API call
        std::string response = http_client_.post(
            authenticator_.getRestBaseUrl(),
            "/api/v3/order?" + signed_query,
            "",  // Empty body - parameters are in the query string
            {},  // No additional query parameters
            headers
        );
        
        // Log the raw response for debugging
        logger_.log("%:% %() % Order submission response: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  response);
        
        // Parse the response
        auto json_response = nlohmann::json::parse(response);
        
        // Extract the Binance order ID and store the mapping
        // Binance returns orderId as a number, so we need to convert it to string
        std::string binance_order_id = std::to_string(json_response["orderId"].get<int64_t>());
        {
            std::lock_guard<std::mutex> lock(order_map_mutex_);
            order_id_to_binance_id_[request.order_id_] = binance_order_id;
        }
        
        // Generate success response
        generateAndEnqueueResponse(
            request.order_id_, 
            Exchange::ClientResponseType::ACCEPTED, 
            request.ticker_id_,
            request.side_,
            request.price_, 
            0,  // exec_qty (0 for new orders)
            request.qty_  // leaves_qty is initially the same as qty
        );
        
        logger_.log("%:% %() % New order accepted. Order ID: %, Binance Order ID: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  request.order_id_, binance_order_id);
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Failed to place new order: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  e.what());
        
        // Generate rejection response
        generateAndEnqueueResponse(
            request.order_id_,
            Exchange::ClientResponseType::CANCEL_REJECTED
        );
    }
}

void BinanceOrderGateway::handleCancelOrderRequest(const Exchange::MEClientRequest& request) {
    logger_.log("%:% %() % Handling cancel order request: %\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
              request.toString());
    
    try {
        // Get the symbol from ticker_id (in a real implementation, this would be a proper mapping)
        std::string symbol = "BTCUSDT";  // Default to BTCUSDT
        
        // Get the Binance order ID from our mapping
        std::string binance_order_id;
        {
            std::lock_guard<std::mutex> lock(order_map_mutex_);
            auto it = order_id_to_binance_id_.find(request.order_id_);
            if (it == order_id_to_binance_id_.end()) {
                // If we don't have a mapping, use our order_id as a fallback (this would be enhanced in a real impl)
                binance_order_id = std::to_string(request.order_id_);
            } else {
                binance_order_id = it->second;
            }
        }
        
        // Prepare the parameters for the cancellation
        std::map<std::string, std::string> params = {
            {"symbol", symbol},
            {"orderId", binance_order_id}
        };
        
        // Sign the request
        std::string signed_query = authenticator_.signRequest(params);
        
        // Prepare headers with authentication
        std::map<std::string, std::string> headers;
        authenticator_.addAuthHeaders(headers);
        
        // Make the API call
        std::string response = http_client_.del(
            authenticator_.getRestBaseUrl(),
            "/api/v3/order?" + signed_query,
            {},  // No additional query parameters
            headers
        );
        
        // Parse the response
        auto json_response = nlohmann::json::parse(response);
        
        // Log the successful cancellation response
        logger_.log("%:% %() % Cancel order response: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  response);
        
        // Generate success response
        generateAndEnqueueResponse(
            request.order_id_,
            Exchange::ClientResponseType::CANCELED,
            request.ticker_id_,
            request.side_
        );
        
        logger_.log("%:% %() % Order cancelled. Order ID: %, Binance Order ID: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  request.order_id_, binance_order_id);
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Failed to cancel order: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  e.what());
        
        // Generate rejection response
        generateAndEnqueueResponse(
            request.order_id_,
            Exchange::ClientResponseType::CANCEL_REJECTED
        );
    }
}

void BinanceOrderGateway::generateAndEnqueueResponse(OrderId order_id, 
                                                   Exchange::ClientResponseType status,
                                                   TickerId ticker_id, Side side,
                                                   Price price, Qty exec_qty, Qty leaves_qty) {
    // Create a response message
    Exchange::MEClientResponse response;
    response.type_ = status;
    response.client_id_ = client_id_;
    response.ticker_id_ = ticker_id;
    response.client_order_id_ = order_id;
    response.market_order_id_ = order_id;  // Use same ID for simplicity
    response.side_ = side;
    response.price_ = price;
    response.exec_qty_ = exec_qty;
    response.leaves_qty_ = leaves_qty;
    
    // Push to the incoming responses queue
    auto next_write = incoming_responses_->getNextToWriteTo();
    *next_write = response;
    incoming_responses_->updateWriteIndex();
    
    // Update sequence number
    next_exp_seq_num_++;
    
    logger_.log("%:% %() % Generated response: %\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
              response.toString());
}

std::string BinanceOrderGateway::getSymbolForTickerId(TickerId ticker_id) {
    // Get the symbol from configuration
    std::string symbol = config_.getSymbolForTickerId(ticker_id);
    
    // Log the mapping for debugging
    logger_.log("%:% %() % Mapping ticker ID % to symbol %\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
              ticker_id, symbol);
              
    return symbol;
}

nlohmann::json BinanceOrderGateway::getSymbolInfo(const std::string& symbol) {
    try {
        // Check if we need to refresh the symbol info cache (refresh every hour)
        std::lock_guard<std::mutex> lock(symbol_info_mutex_);
        auto now = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(now - last_symbol_info_refresh_).count();
        
        if (elapsed > 60 || symbol_info_cache_.empty()) {
            // Fetch exchange info from Binance
            std::string response = http_client_.get(
                authenticator_.getRestBaseUrl(),
                "/api/v3/exchangeInfo",
                {},  // No query parameters
                {}   // No headers needed for this endpoint
            );
            
            // Parse the JSON response
            auto exchange_info = nlohmann::json::parse(response);
            
            // Clear the cache
            symbol_info_cache_.clear();
            
            // Process all symbols and store in cache
            if (exchange_info.contains("symbols") && exchange_info["symbols"].is_array()) {
                for (const auto& symbol_info : exchange_info["symbols"]) {
                    if (symbol_info.contains("symbol")) {
                        std::string symbol_name = symbol_info["symbol"];
                        symbol_info_cache_[symbol_name] = symbol_info;
                    }
                }
            }
            
            // Update the refresh timestamp
            last_symbol_info_refresh_ = now;
            
            logger_.log("%:% %() % Refreshed symbol info cache with % symbols\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      symbol_info_cache_.size());
        }
        
        // Check if we have info for this symbol
        auto it = symbol_info_cache_.find(symbol);
        if (it != symbol_info_cache_.end()) {
            return it->second;
        } else {
            logger_.log("%:% %() % Symbol % not found in cache\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      symbol);
            return nlohmann::json::object();  // Return empty object
        }
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Failed to get symbol info: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  e.what());
        return nlohmann::json::object();  // Return empty object
    }
}

double BinanceOrderGateway::getLatestMarketPrice(const std::string& symbol) {
    // First, try to find the price in the market data updates queue
    if (market_data_updates_) {
        // Map ticker symbols to ticker IDs
        Common::TickerId ticker_id = Common::TickerId_INVALID;
        if (symbol == "BTCUSDT") {
            ticker_id = 1;
        } else if (symbol == "ETHUSDT") {
            ticker_id = 2;
        } else if (symbol == "BNBUSDT") {
            ticker_id = 3;
        }
        
        if (ticker_id != Common::TickerId_INVALID) {
            // Check the market data updates queue for the latest price
            // We'll scan through all available updates and take the most recent price
            double latest_price = 0.0;
            bool found_price = false;
            
            // Unfortunately we can't scan without consuming entries, so we'll
            // save what we find along the way and then re-add them to a temporary vector
            std::vector<Exchange::MEMarketUpdate> saved_updates;
            
            // Look for the most recent price update for this ticker
            while (auto update = market_data_updates_->getNextToRead()) {
                // Save this update for later
                saved_updates.push_back(*update);
                
                // Check if this update contains price information for our ticker
                if (update->ticker_id_ == ticker_id && 
                    update->price_ != Common::Price_INVALID &&
                    (update->type_ == Exchange::MarketUpdateType::ADD || 
                     update->type_ == Exchange::MarketUpdateType::MODIFY)) {
                    
                    latest_price = Binance::internalPriceToBinance(update->price_); // Convert from internal format
                    found_price = true;
                }
                
                // Move to the next update
                market_data_updates_->updateReadIndex();
            }
            
            // If we found updates, log about it
            if (!saved_updates.empty()) {
                logger_.log("%:% %() % Scanned through % market data updates\n", 
                          __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                          saved_updates.size());
            }
            
            if (found_price) {
                logger_.log("%:% %() % Found latest price for % (ticker %) in market data queue: %\n", 
                          __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                          symbol, ticker_id, latest_price);
                return latest_price;
            }
            
            logger_.log("%:% %() % No price found for % (ticker %) in market data queue, will fetch from API\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      symbol, ticker_id);
        }
    }
    
    // If we couldn't find the price in the market data queue, fall back to API call
    try {
        // Fetch the current price from Binance API
        std::string response = http_client_.get(
            authenticator_.getRestBaseUrl(),
            "/api/v3/ticker/price?symbol=" + symbol,
            {},  // No query parameters - they're in the URL
            {}   // No headers needed for this endpoint
        );
        
        // Parse the JSON response
        auto json_response = nlohmann::json::parse(response);
        
        if (json_response.contains("price")) {
            double price = std::stod(json_response["price"].get<std::string>());
            logger_.log("%:% %() % Latest price for % from API: %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      symbol, price);
            return price;
        } else {
            logger_.log("%:% %() % Failed to get latest price for %: missing price field\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      symbol);
            return 0.0;
        }
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Failed to get latest price for %: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  symbol, e.what());
        return 0.0;
    }
}

bool BinanceOrderGateway::validateOrderPrice(const std::string& symbol, Price price, Side side) {
    // Convert internal price format to actual price
    double order_price = Binance::internalPriceToBinance(price);
    
    // Get the latest market price
    double market_price = getLatestMarketPrice(symbol);
    
    if (market_price <= 0.0) {
        logger_.log("%:% %() % Cannot validate price for %: no market price available\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  symbol);
        return false;
    }
    
    // Get symbol information from Binance
    auto symbol_info = getSymbolInfo(symbol);
    if (symbol_info.empty()) {
        logger_.log("%:% %() % Cannot validate price for %: symbol info not available\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  symbol);
        return false;
    }
    
    // Variables to store filter values
    double percent_price_filter_multiplier_up = 5.0;   // Default 5% up
    double percent_price_filter_multiplier_down = 5.0; // Default 5% down
    
    // Find the PERCENT_PRICE or PERCENT_PRICE_BY_SIDE filter
    bool found_percent_price_filter = false;
    
    if (symbol_info.contains("filters") && symbol_info["filters"].is_array()) {
        for (const auto& filter : symbol_info["filters"]) {
            if (filter.contains("filterType")) {
                std::string filter_type = filter["filterType"];
                
                // Check for PERCENT_PRICE filter
                if (filter_type == "PERCENT_PRICE") {
                    found_percent_price_filter = true;
                    
                    if (filter.contains("multiplierUp")) {
                        // Handle number type (double) or string type conversion
                        double multiplier_up = 0.0;
                        if (filter["multiplierUp"].is_number()) {
                            multiplier_up = filter["multiplierUp"].get<double>();
                        } else if (filter["multiplierUp"].is_string()) {
                            multiplier_up = std::stod(filter["multiplierUp"].get<std::string>());
                        }
                        percent_price_filter_multiplier_up = multiplier_up - 1.0;
                        logger_.log("%:% %() % Parsed multiplierUp=%\n", 
                                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                                  multiplier_up);
                    }
                    
                    if (filter.contains("multiplierDown")) {
                        // Handle number type (double) or string type conversion
                        double multiplier_down = 0.0;
                        if (filter["multiplierDown"].is_number()) {
                            multiplier_down = filter["multiplierDown"].get<double>();
                        } else if (filter["multiplierDown"].is_string()) {
                            multiplier_down = std::stod(filter["multiplierDown"].get<std::string>());
                        }
                        percent_price_filter_multiplier_down = 1.0 - multiplier_down;
                        logger_.log("%:% %() % Parsed multiplierDown=%\n", 
                                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                                  multiplier_down);
                    }
                }
                // Check for PERCENT_PRICE_BY_SIDE filter (newer version of Binance API)
                else if (filter_type == "PERCENT_PRICE_BY_SIDE") {
                    found_percent_price_filter = true;
                    
                    if (side == Side::BUY) {
                        if (filter.contains("bidMultiplierUp")) {
                            // Handle number type (double) or string type conversion
                            double multiplier_up = 0.0;
                            if (filter["bidMultiplierUp"].is_number()) {
                                multiplier_up = filter["bidMultiplierUp"].get<double>();
                            } else if (filter["bidMultiplierUp"].is_string()) {
                                multiplier_up = std::stod(filter["bidMultiplierUp"].get<std::string>());
                            }
                            percent_price_filter_multiplier_up = multiplier_up - 1.0;
                            logger_.log("%:% %() % Parsed bidMultiplierUp=%\n", 
                                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                                      multiplier_up);
                        }
                        if (filter.contains("bidMultiplierDown")) {
                            // Handle number type (double) or string type conversion
                            double multiplier_down = 0.0;
                            if (filter["bidMultiplierDown"].is_number()) {
                                multiplier_down = filter["bidMultiplierDown"].get<double>();
                            } else if (filter["bidMultiplierDown"].is_string()) {
                                multiplier_down = std::stod(filter["bidMultiplierDown"].get<std::string>());
                            }
                            percent_price_filter_multiplier_down = 1.0 - multiplier_down;
                            logger_.log("%:% %() % Parsed bidMultiplierDown=%\n", 
                                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                                      multiplier_down);
                        }
                    } else { // SELL
                        if (filter.contains("askMultiplierUp")) {
                            // Handle number type (double) or string type conversion
                            double multiplier_up = 0.0;
                            if (filter["askMultiplierUp"].is_number()) {
                                multiplier_up = filter["askMultiplierUp"].get<double>();
                            } else if (filter["askMultiplierUp"].is_string()) {
                                multiplier_up = std::stod(filter["askMultiplierUp"].get<std::string>());
                            }
                            percent_price_filter_multiplier_up = multiplier_up - 1.0;
                            logger_.log("%:% %() % Parsed askMultiplierUp=%\n", 
                                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                                      multiplier_up);
                        }
                        if (filter.contains("askMultiplierDown")) {
                            // Handle number type (double) or string type conversion
                            double multiplier_down = 0.0;
                            if (filter["askMultiplierDown"].is_number()) {
                                multiplier_down = filter["askMultiplierDown"].get<double>();
                            } else if (filter["askMultiplierDown"].is_string()) {
                                multiplier_down = std::stod(filter["askMultiplierDown"].get<std::string>());
                            }
                            percent_price_filter_multiplier_down = 1.0 - multiplier_down;
                            logger_.log("%:% %() % Parsed askMultiplierDown=%\n", 
                                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                                      multiplier_down);
                        }
                    }
                }
            }
        }
    }
    
    if (!found_percent_price_filter) {
        logger_.log("%:% %() % No PERCENT_PRICE filter found for %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  symbol);
        // Continue with default values
    }
    
    // Calculate the price difference as a percentage
    double price_diff_pct = (order_price - market_price) / market_price;
    
    // Determine if the price is valid based on side
    bool is_valid;
    
    if (side == Side::BUY) {
        // For buys, price should not be too high
        is_valid = (price_diff_pct <= percent_price_filter_multiplier_up) && 
                   (price_diff_pct >= -percent_price_filter_multiplier_down);
    } else { // SELL
        // For sells, price should not be too low
        is_valid = (price_diff_pct <= percent_price_filter_multiplier_up) && 
                   (price_diff_pct >= -percent_price_filter_multiplier_down);
    }
    
    // Calculate the percentage for logging
    double price_diff_pct_display = price_diff_pct * 100.0;
    
    logger_.log("%:% %() % Price validation for %: order_price=%, market_price=%, diff_pct=%.2f%%, "
              "filter_up=%.2f%%, filter_down=%.2f%%, is_valid=%\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
              symbol, order_price, market_price, price_diff_pct_display, 
              percent_price_filter_multiplier_up * 100.0, percent_price_filter_multiplier_down * 100.0, 
              is_valid);
    
    return is_valid;
}

double BinanceOrderGateway::getAccountBalance(const std::string& asset) {
    try {
        // Create parameters for the API call
        std::map<std::string, std::string> params = {
            {"timestamp", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count())}
        };
        
        // Sign the request
        std::string signed_query = authenticator_.signRequest(params);
        
        // Prepare headers with authentication
        std::map<std::string, std::string> headers;
        authenticator_.addAuthHeaders(headers);
        
        // Make the API call to get account information
        std::string response = http_client_.get(
            authenticator_.getRestBaseUrl(),
            "/api/v3/account?" + signed_query,
            {},  // No additional parameters
            headers
        );
        
        // Parse the JSON response
        auto json_response = nlohmann::json::parse(response);
        
        // Log the response for debugging
        logger_.log("%:% %() % Account info response received: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  response.substr(0, 200) + (response.length() > 200 ? "..." : ""));
        
        // Find the balance for the requested asset
        if (json_response.contains("balances") && json_response["balances"].is_array()) {
            for (const auto& balance : json_response["balances"]) {
                if (balance.contains("asset") && balance["asset"].get<std::string>() == asset) {
                    // Get the free balance (available for trading)
                    double free_balance = 0.0;
                    if (balance.contains("free")) {
                        if (balance["free"].is_string()) {
                            free_balance = std::stod(balance["free"].get<std::string>());
                        } else if (balance["free"].is_number()) {
                            free_balance = balance["free"].get<double>();
                        }
                    }
                    
                    logger_.log("%:% %() % Balance for %: %\n", 
                              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                              asset, free_balance);
                    return free_balance;
                }
            }
            
            // If we get here, the asset was not found
            logger_.log("%:% %() % Asset % not found in account balances\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      asset);
        } else {
            logger_.log("%:% %() % No balances found in account info\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        }
        
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Failed to get account balance: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  e.what());
    }
    
    // Return 0 if we couldn't get the balance
    return 0.0;
}

double BinanceOrderGateway::calculateOrderQuantity(const std::string& symbol, double price, Side side) {
    try {
        // Determine which asset we need to check balance for
        std::string base_asset = "";
        std::string quote_asset = "";
        
        // Get symbol info to determine base and quote assets
        auto symbol_info = getSymbolInfo(symbol);
        if (!symbol_info.empty()) {
            if (symbol_info.contains("baseAsset")) {
                base_asset = symbol_info["baseAsset"].get<std::string>();
            }
            
            if (symbol_info.contains("quoteAsset")) {
                quote_asset = symbol_info["quoteAsset"].get<std::string>();
            }
        }
        
        if (base_asset.empty() || quote_asset.empty()) {
            logger_.log("%:% %() % Could not determine base or quote asset for symbol %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      symbol);
            return 0.0;
        }
        
        logger_.log("%:% %() % Symbol %: base asset = %, quote asset = %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  symbol, base_asset, quote_asset);
        
        // For BUY orders, we need to check quote asset balance
        // For SELL orders, we need to check base asset balance
        double balance = 0.0;
        double quantity = 0.0;
        
        if (side == Side::BUY) {
            // For BUY orders, check quote asset balance (e.g., USDT)
            balance = getAccountBalance(quote_asset);
            
            // Calculate how much we can buy with our balance
            // Use 95% of balance to account for fees
            double usable_balance = balance * 0.95;
            
            if (price > 0) {
                quantity = usable_balance / price;
            }
            
            logger_.log("%:% %() % BUY order: % balance = %, usable = %, price = %, quantity = %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      quote_asset, balance, usable_balance, price, quantity);
        } else { // SELL
            // For SELL orders, check base asset balance (e.g., BTC)
            balance = getAccountBalance(base_asset);
            
            // Use 95% of balance to account for fees
            quantity = balance * 0.95;
            
            logger_.log("%:% %() % SELL order: % balance = %, quantity = %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      base_asset, balance, quantity);
        }
        
        // Apply LOT_SIZE filters to ensure the quantity is valid
        if (!symbol_info.empty() && symbol_info.contains("filters") && symbol_info["filters"].is_array()) {
            for (const auto& filter : symbol_info["filters"]) {
                if (filter.contains("filterType") && filter["filterType"] == "LOT_SIZE") {
                    double min_qty = 0.0;
                    double max_qty = 0.0;
                    double step_size = 0.0;
                    
                    // Parse min quantity
                    if (filter.contains("minQty")) {
                        if (filter["minQty"].is_string()) {
                            min_qty = std::stod(filter["minQty"].get<std::string>());
                        } else if (filter["minQty"].is_number()) {
                            min_qty = filter["minQty"].get<double>();
                        }
                    }
                    
                    // Parse max quantity
                    if (filter.contains("maxQty")) {
                        if (filter["maxQty"].is_string()) {
                            max_qty = std::stod(filter["maxQty"].get<std::string>());
                        } else if (filter["maxQty"].is_number()) {
                            max_qty = filter["maxQty"].get<double>();
                        }
                    }
                    
                    // Parse step size
                    if (filter.contains("stepSize")) {
                        if (filter["stepSize"].is_string()) {
                            step_size = std::stod(filter["stepSize"].get<std::string>());
                        } else if (filter["stepSize"].is_number()) {
                            step_size = filter["stepSize"].get<double>();
                        }
                    }
                    
                    logger_.log("%:% %() % LOT_SIZE filter: min_qty = %, max_qty = %, step_size = %\n", 
                              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                              min_qty, max_qty, step_size);
                    
                    // Apply the filters
                    if (quantity < min_qty) {
                        quantity = min_qty;
                    }
                    
                    if (max_qty > 0 && quantity > max_qty) {
                        quantity = max_qty;
                    }
                    
                    if (step_size > 0) {
                        // Round down to the nearest step size
                        quantity = std::floor(quantity / step_size) * step_size;
                    }
                    
                    logger_.log("%:% %() % After applying LOT_SIZE filter: quantity = %\n", 
                              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                              quantity);
                    
                    break; // We found the LOT_SIZE filter, no need to continue
                }
            }
        }
        
        // Apply NOTIONAL filter to ensure the order value meets minimum requirements
        if (!symbol_info.empty() && symbol_info.contains("filters") && symbol_info["filters"].is_array()) {
            for (const auto& filter : symbol_info["filters"]) {
                if (filter.contains("filterType") && filter["filterType"] == "NOTIONAL") {
                    double min_notional = 0.0;
                    
                    // Parse min notional
                    if (filter.contains("minNotional")) {
                        if (filter["minNotional"].is_string()) {
                            min_notional = std::stod(filter["minNotional"].get<std::string>());
                        } else if (filter["minNotional"].is_number()) {
                            min_notional = filter["minNotional"].get<double>();
                        }
                    }
                    
                    logger_.log("%:% %() % NOTIONAL filter: min_notional = %\n", 
                              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                              min_notional);
                    
                    // Check if the order meets the minimum notional
                    double notional = quantity * price;
                    if (notional < min_notional && min_notional > 0) {
                        // Adjust quantity to meet minimum notional
                        quantity = std::ceil(min_notional / price * 100) / 100.0; // Round up with 2 decimal places
                        
                        logger_.log("%:% %() % Adjusting quantity to meet minimum notional: quantity = %\n", 
                                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                                  quantity);
                    }
                    
                    break; // We found the NOTIONAL filter, no need to continue
                }
            }
        }
        
        // For Binance test environment, use a small but valid quantity to ensure acceptance
        if (authenticator_.isUsingTestnet()) {
            if (symbol == "BTCUSDT") {
                if (quantity > 0.001) {
                    quantity = 0.001; // Use a small quantity for BTC test orders
                } 
                
                // Make sure quantity is at least the minimum
                if (quantity < 0.00001) {
                    quantity = 0.00001; // Binance BTC minimum
                }
            } else if (symbol == "ETHUSDT") {
                if (quantity > 0.01) {
                    quantity = 0.01; // Use a small quantity for ETH test orders
                }
                
                // Make sure quantity is at least the minimum
                if (quantity < 0.0001) {
                    quantity = 0.0001; // Binance ETH minimum
                }
            }
            
            logger_.log("%:% %() % Using testnet, adjusting to small quantity: %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      quantity);
        }
        
        return quantity;
        
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Failed to calculate order quantity: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  e.what());
        return 0.0;
    }
}

void BinanceOrderGateway::startUserDataStream() {
    try {
        // Create a shared_ptr to the authenticator for use with the user data stream
        // We need to use a raw pointer reference because BinanceAuthenticator is not copyable
        auto authenticator_ptr = std::shared_ptr<BinanceAuthenticator>(&authenticator_, [](BinanceAuthenticator*){
            // No-op deleter - we don't own the authenticator
        });
        
        // Create the user data stream
        user_data_stream_ = std::make_unique<BinanceUserDataStream>(
            logger_,
            authenticator_ptr,
            config_,
            std::bind(&BinanceOrderGateway::handleUserDataMessage, this, std::placeholders::_1)
        );
        
        // Start the stream
        if (!user_data_stream_->start()) {
            logger_.log("%:% %() % Failed to start user data stream\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
            return;
        }
        
        logger_.log("%:% %() % User data stream started\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Exception while starting user data stream: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), e.what());
    }
}

void BinanceOrderGateway::stopUserDataStream() {
    try {
        if (user_data_stream_) {
            // Stop the stream
            user_data_stream_->stop();
            user_data_stream_.reset();
            
            logger_.log("%:% %() % User data stream stopped\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        }
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Exception while stopping user data stream: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), e.what());
    }
}

void BinanceOrderGateway::handleUserDataMessage(const std::string& message) {
    try {
        // Parse the message
        nlohmann::json json_message = nlohmann::json::parse(message);
        
        // Check if it's an order update or account update
        if (json_message.contains("e")) {
            std::string event_type = json_message["e"].get<std::string>();
            
            if (event_type == "executionReport") {
                // Order update
                processOrderUpdate(json_message);
            } else if (event_type == "outboundAccountPosition") {
                // Account update
                processAccountUpdate(json_message);
            } else {
                logger_.log("%:% %() % Received unknown event type: %\n", 
                          __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                          event_type);
            }
        } else {
            logger_.log("%:% %() % Received message with unknown format: %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      message);
        }
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Exception while handling user data message: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), e.what());
    }
}

void BinanceOrderGateway::processOrderUpdate(const nlohmann::json& order_update) {
    try {
        // Extract relevant fields from the order update
        std::string client_order_id = order_update["c"].get<std::string>();
        std::string binance_order_id = order_update["i"].get<std::string>();
        std::string symbol = order_update["s"].get<std::string>();
        std::string side_str = order_update["S"].get<std::string>();
        std::string order_status = order_update["X"].get<std::string>();
        double price = std::stod(order_update["p"].get<std::string>());
        double orig_qty = std::stod(order_update["q"].get<std::string>());
        double executed_qty = std::stod(order_update["z"].get<std::string>());
        double leaves_qty = orig_qty - executed_qty;
        
        logger_.log("%:% %() % Received order update: status=%, symbol=%, side=%, price=%, exec_qty=%, leaves_qty=%\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  order_status, symbol, side_str, price, executed_qty, leaves_qty);
        
        // Determine the ticker ID from the symbol
        TickerId ticker_id = config_.getTickerIdForSymbol(symbol);
        
        // Determine the side
        Side side = Side::INVALID;
        if (side_str == "BUY") {
            side = Side::BUY;
        } else if (side_str == "SELL") {
            side = Side::SELL;
        }
        
        // Extract the original order ID from the client order ID
        // Client order ID format is usually "x-<internal_order_id>"
        OrderId order_id = 0;
        if (client_order_id.find("x-") == 0) {
            try {
                order_id = std::stoull(client_order_id.substr(2));
            } catch (...) {
                logger_.log("%:% %() % Could not parse original order ID from client order ID: %\n", 
                          __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                          client_order_id);
            }
        }
        
        // Store the order ID mapping
        if (order_id > 0) {
            std::lock_guard<std::mutex> lock(order_map_mutex_);
            order_id_to_binance_id_[order_id] = binance_order_id;
        }
        
        // Convert price from double to internal representation
        Price internal_price = static_cast<Price>(price * Common::PriceMultiplier);
        
        // Convert quantities from double to internal representation
        Qty internal_exec_qty = static_cast<Qty>(executed_qty * Common::QtyMultiplier);
        Qty internal_leaves_qty = static_cast<Qty>(leaves_qty * Common::QtyMultiplier);
        
        // Determine the client response type based on the order status
        Exchange::ClientResponseType response_type = Exchange::ClientResponseType::ACCEPTED;
        
        if (order_status == "NEW" || order_status == "PARTIALLY_FILLED") {
            response_type = Exchange::ClientResponseType::ACCEPTED;
        } else if (order_status == "FILLED") {
            response_type = Exchange::ClientResponseType::FILLED;
        } else if (order_status == "CANCELED" || order_status == "EXPIRED" || order_status == "REJECTED") {
            response_type = Exchange::ClientResponseType::CANCELED;
        }
        
        // Generate and enqueue a response
        if (order_id > 0) {
            generateAndEnqueueResponse(order_id, response_type, ticker_id, side, 
                                      internal_price, internal_exec_qty, internal_leaves_qty);
        }
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Exception while processing order update: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), e.what());
    }
}

void BinanceOrderGateway::processAccountUpdate(const nlohmann::json& account_update) {
    try {
        // Log the account update
        logger_.log("%:% %() % Received account update: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  account_update.dump());
        
        // Process balance information if needed
        if (account_update.contains("B") && account_update["B"].is_array()) {
            for (const auto& balance : account_update["B"]) {
                std::string asset = balance["a"].get<std::string>();
                double free_amount = std::stod(balance["f"].get<std::string>());
                double locked_amount = std::stod(balance["l"].get<std::string>());
                
                logger_.log("%:% %() % Updated balance for %: free=%, locked=%\n", 
                          __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                          asset, free_amount, locked_amount);
            }
        }
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Exception while processing account update: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), e.what());
    }
}

} // namespace Trading