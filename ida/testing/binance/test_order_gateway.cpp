#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <signal.h>
#include <unordered_map>

#include "common/logging.h"
#include "common/macros.h"
#include "common/lf_queue.h"
#include "common/types.h"
#include "market_data/binance_types.h"

#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "exchange/market_data/market_update.h"

#include "market_data/binance_authenticator.h"
#include "market_data/binance_http_client.h"
#include "market_data/binance_config.h"
#include "order_gw/binance_order_gateway.h"

std::atomic<bool> running{true};

void signal_handler(int signal) {
    std::cout << "Caught signal " << signal << ", shutting down..." << std::endl;
    running = false;
}

// Function to print client response details
void print_client_response(const Exchange::MEClientResponse& response) {
    std::cout << "Response: " 
              << Exchange::clientResponseTypeToString(response.type_)
              << " | ClientID: " << Common::clientIdToString(response.client_id_)
              << " | OrderID: " << Common::orderIdToString(response.client_order_id_)
              << " | TickerID: " << Common::tickerIdToString(response.ticker_id_)
              << " | Side: " << Common::sideToString(response.side_);
              
    if (response.price_ != Common::Price_INVALID) {
        std::cout << " | Price: " << Common::priceToString(response.price_);
    }
    
    if (response.exec_qty_ != Common::Qty_INVALID) {
        std::cout << " | Exec Qty: " << Common::qtyToString(response.exec_qty_);
    }
    
    if (response.leaves_qty_ != Common::Qty_INVALID) {
        std::cout << " | Leaves Qty: " << Common::qtyToString(response.leaves_qty_);
    }
    
    std::cout << std::endl;
}

// Function to test order submission
bool test_order_submission(Trading::BinanceOrderGateway& /*order_gateway*/, 
                          Exchange::ClientRequestLFQueue& client_requests,
                          Exchange::ClientResponseLFQueue& client_responses,
                          Common::Logger& logger,
                          const Exchange::MEClientRequest& new_order) {
    std::string time_str;
    std::cout << "Testing order submission..." << std::endl;
    
    logger.log("%:% %() % Submitting test order: %\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str), 
              new_order.toString());
    
    // Add order to requests queue
    auto next_write = client_requests.getNextToWriteTo();
    *next_write = new_order;
    client_requests.updateWriteIndex();
    
    // Wait for response
    bool got_response = false;
    std::cout << "Waiting for order response..." << std::endl;
    
    // Try for up to 10 seconds
    for (int i = 0; i < 100 && running; ++i) {
        for (auto client_response = client_responses.getNextToRead(); 
             client_response; 
             client_response = client_responses.getNextToRead()) {
            
            logger.log("%:% %() % Received response: %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str), 
                      client_response->toString());
            
            print_client_response(*client_response);
            
            // Check if this is a response for our order
            if (client_response->client_order_id_ == new_order.order_id_) {
                got_response = true;
                
                // Log the result
                if (client_response->type_ == Exchange::ClientResponseType::ACCEPTED) {
                    std::cout << "Order accepted successfully!" << std::endl;
                } else {
                    std::cout << "Order was not accepted: " 
                              << Exchange::clientResponseTypeToString(client_response->type_) 
                              << std::endl;
                }
            }
            
            client_responses.updateReadIndex();
        }
        
        if (got_response) {
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    if (!got_response) {
        std::cout << "Timed out waiting for order response" << std::endl;
        return false;
    }
    
    return true;
}

// Function to test order cancellation
bool test_order_cancellation(Trading::BinanceOrderGateway& /*order_gateway*/, 
                            Exchange::ClientRequestLFQueue& client_requests,
                            Exchange::ClientResponseLFQueue& client_responses,
                            Common::OrderId order_id_to_cancel,
                            Common::Logger& logger,
                            Common::TickerId ticker_id) {
    std::string time_str;
    std::cout << "Testing order cancellation..." << std::endl;
    
    // Create a cancel order request
    Exchange::MEClientRequest cancel_order;
    cancel_order.type_ = Exchange::ClientRequestType::CANCEL;
    cancel_order.client_id_ = 1;  // Client ID 1
    cancel_order.ticker_id_ = ticker_id;  // Use the provided ticker ID
    cancel_order.order_id_ = order_id_to_cancel;  // Order ID to cancel
    
    logger.log("%:% %() % Submitting cancel request: %\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str), 
              cancel_order.toString());
    
    // Add cancel request to requests queue
    auto next_write = client_requests.getNextToWriteTo();
    *next_write = cancel_order;
    client_requests.updateWriteIndex();
    
    // Wait for response
    bool got_response = false;
    std::cout << "Waiting for cancel response..." << std::endl;
    
    // Try for up to 10 seconds
    for (int i = 0; i < 100 && running; ++i) {
        for (auto client_response = client_responses.getNextToRead(); 
             client_response; 
             client_response = client_responses.getNextToRead()) {
            
            logger.log("%:% %() % Received response: %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str), 
                      client_response->toString());
            
            print_client_response(*client_response);
            
            // Check if this is a response for our cancel request
            if (client_response->client_order_id_ == order_id_to_cancel) {
                got_response = true;
                
                // Log the result
                if (client_response->type_ == Exchange::ClientResponseType::CANCELED) {
                    std::cout << "Order canceled successfully!" << std::endl;
                } else {
                    std::cout << "Order was not canceled: " 
                              << Exchange::clientResponseTypeToString(client_response->type_) 
                              << std::endl;
                }
            }
            
            client_responses.updateReadIndex();
        }
        
        if (got_response) {
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    if (!got_response) {
        std::cout << "Timed out waiting for cancel response" << std::endl;
        return false;
    }
    
    return true;
}

int main(int argc, char* argv[]) {
    // Set up signal handler
    signal(SIGINT, signal_handler);
    
    // Create logger and time string for logging
    std::string time_str;
    Common::Logger logger("/home/praveen/omlaxmiquant/ida/logs/binance_order_gateway_test.log");
    logger.log("Starting Binance order gateway test\n");
    
    // Parse command-line arguments
    bool test_submission = false;
    bool test_cancellation = false;
    bool use_testnet = true;  // Default to testnet for safety
    
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--test-submission") {
                test_submission = true;
            } else if (arg == "--test-cancellation") {
                test_cancellation = true;
            } else if (arg == "--testnet=false") {
                use_testnet = false;
            } else if (arg == "--testnet=true") {
                use_testnet = true;
            }
        }
    }
    
    // If no specific tests are requested, run all tests
    if (!test_submission && !test_cancellation) {
        test_submission = true;
        test_cancellation = true;
    }
    
    std::cout << "Binance Order Gateway Test" << std::endl;
    std::cout << "Using " << (use_testnet ? "testnet" : "mainnet") << std::endl;
    std::cout << "Tests enabled: " 
              << (test_submission ? "Order Submission " : "")
              << (test_cancellation ? "Order Cancellation" : "")
              << std::endl;
    
    // Create the Binance authenticator
    Trading::BinanceAuthenticator authenticator(logger);
    
    if (!authenticator.hasValidCredentials()) {
        std::cerr << "ERROR: No valid Binance API credentials found. Please check your vault.json file." << std::endl;
        return 1;
    }
    
    // Create Binance configuration
    Trading::BinanceConfig config(logger);
    
    if (!config.loadConfig()) {
        std::cerr << "ERROR: Failed to load Binance configuration. Please check your BinanceConfig.json file." << std::endl;
        return 1;
    }
    
    // Override use_testnet if specified through command line
    if (!use_testnet) {
        config.use_testnet = false;
    }
    
    // Create request and response queues (with space for 128 messages each)
    Exchange::ClientRequestLFQueue client_requests(128);
    Exchange::ClientResponseLFQueue client_responses(128);
    Exchange::MEMarketUpdateLFQueue market_data_updates(128);
    
    // Get the first ticker from the config
    auto ticker_ids = config.getAllTickerIds();
    Common::TickerId test_ticker_id = ticker_ids.empty() ? 1 : ticker_ids[0]; // Default to 1 if no tickers
    
    // Create a new order request that we'll use for testing
    Exchange::MEClientRequest new_order;
    new_order.type_ = Exchange::ClientRequestType::NEW;
    new_order.client_id_ = config.getClientId();
    new_order.ticker_id_ = test_ticker_id;
    new_order.order_id_ = config.getDefaultTestOrderId();
    new_order.side_ = config.getDefaultTestSide();
    
    // Get ticker info to set appropriate price and quantity
    auto ticker_info = config.getTickerInfo(test_ticker_id);
    double test_price = ticker_info.test_price;
    double test_qty = ticker_info.test_qty;
    
    // Set default values if ticker info is invalid
    if (test_price <= 0) test_price = 30000.0;
    if (test_qty <= 0) test_qty = 0.001;
    
    // Set price and quantity in internal format using Binance conversion helpers
    new_order.price_ = Trading::Binance::binancePriceToInternal(test_price);
    new_order.qty_ = Trading::Binance::binanceQtyToInternal(test_qty);
    
    // Populate the market data queue with current market prices
    // This will help the price validation in the order gateway
    logger.log("%:% %() % Fetching current market prices for test symbols\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
    
    // Get the symbol for the test ticker
    std::string test_symbol = config.getSymbolForTickerId(test_ticker_id);
    if (test_symbol.empty()) {
        test_symbol = "BTCUSDT"; // Default symbol if not found in config
    }
    
    logger.log("%:% %() % Test symbol: % (ticker ID %)\n", 
              __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str),
              test_symbol, test_ticker_id);
    
    // Connect to Binance API to get current prices
    try {
        // Create a temporary HTTP client
        Trading::BinanceHttpClient http_client(logger);
        
        // Get current price for the test symbol
        std::string response = http_client.get(
            config.getRestBaseUrl(),
            "/api/v3/ticker/price?symbol=" + test_symbol,
            {},  // No query params
            {}   // No headers
        );
        
        // Parse response
        auto json = nlohmann::json::parse(response);
        
        if (json.contains("price")) {
            double current_price = std::stod(json["price"].get<std::string>());
            
            // Create a market update with the current price and add it to the queue
            Exchange::MEMarketUpdate update;
            update.type_ = Exchange::MarketUpdateType::ADD;
            update.ticker_id_ = test_ticker_id;
            update.price_ = Trading::Binance::binancePriceToInternal(current_price);  // Convert to internal format
            update.side_ = Common::Side::BUY;
            
            auto next_write = market_data_updates.getNextToWriteTo();
            *next_write = update;
            market_data_updates.updateWriteIndex();
            
            logger.log("%:% %() % Added market data for %: price=%\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str),
                      test_symbol, current_price);
            
            // Update the test order's price to be close to market price
            // Use a price that's slightly below market price to ensure it passes validation
            double test_price = current_price * config.getTestPriceMultiplier();
            new_order.price_ = Trading::Binance::binancePriceToInternal(test_price);
            
            logger.log("%:% %() % Using test price for order: % (% * multiplier %)\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str),
                      test_price, current_price, config.getTestPriceMultiplier());
        }
    } catch (const std::exception& e) {
        logger.log("%:% %() % Error fetching prices: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str),
                  e.what());
        // Continue with the default price
    }
    
    // Create the Binance order gateway
    Trading::BinanceOrderGateway order_gateway(
        config.getClientId(),
        &client_requests,
        &client_responses,
        &market_data_updates,
        authenticator,
        config
    );
    
    // Start the order gateway
    order_gateway.start();
    std::cout << "Order gateway started." << std::endl;
    
    // Wait a moment for the gateway to initialize
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    bool overall_success = true;
    Common::OrderId last_order_id = 0;
    
    // Test order submission if enabled
    if (test_submission) {
        bool submission_success = test_order_submission(order_gateway, client_requests, client_responses, logger, new_order);
        std::cout << "Order submission test " << (submission_success ? "PASSED" : "FAILED") << std::endl;
        overall_success &= submission_success;
        
        if (submission_success) {
            last_order_id = 1001;  // The order ID we used
        }
    }
    
    // Test order cancellation if enabled and we have an order to cancel
    if (test_cancellation && last_order_id != 0) {
        bool cancellation_success = test_order_cancellation(
            order_gateway, client_requests, client_responses, last_order_id, logger, new_order.ticker_id_);
        std::cout << "Order cancellation test " << (cancellation_success ? "PASSED" : "FAILED") << std::endl;
        overall_success &= cancellation_success;
    } else if (test_cancellation) {
        std::cout << "Skipping cancellation test as no order was successfully submitted" << std::endl;
    }
    
    // Stop the order gateway
    order_gateway.stop();
    std::cout << "Order gateway stopped." << std::endl;
    
    // Final status
    std::cout << "Test completed. Overall status: " << (overall_success ? "PASSED" : "FAILED") << std::endl;
    
    logger.log("Binance order gateway test finished\n");
    return overall_success ? 0 : 1;
}