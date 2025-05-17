#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <signal.h>
#include <map>

#include "common/logging.h"
#include "market_data/binance_market_data_consumer.h"
#include "market_data/binance_authenticator.h"
#include "market_data/binance_http_client.h"
#include "order_gw/binance_order_gateway.h"
#include "exchange/market_data/market_update.h"
#include "common/lf_queue.h"

std::atomic<bool> running{true};

void signal_handler(int signal) {
    std::cout << "Caught signal " << signal << ", shutting down..." << std::endl;
    running = false;
}

bool testAuthentication(Common::Logger& logger, Trading::BinanceAuthenticator& auth) {
    logger.log("Testing Binance authentication...\n");
    
    if (!auth.hasValidCredentials()) {
        logger.log("Authentication test failed: No valid credentials found\n");
        return false;
    }
    
    logger.log("Authentication credentials loaded successfully\n");
    
    // Create the HTTP client
    Trading::BinanceHttpClient http_client(logger);
    
    // Add authentication headers
    std::map<std::string, std::string> headers;
    auth.addAuthHeaders(headers);
    
    // Sign a request for the account endpoint
    std::map<std::string, std::string> emptyParams;
    std::string signed_query = auth.signRequest(emptyParams);
    logger.log("Generated signed query: %\n", signed_query);
    
    // Make a simple account test request (GET /api/v3/account)
    try {
        std::string response = http_client.get(
            auth.getRestBaseUrl(),
            "/api/v3/account?" + signed_query,
            {},  // No additional query params needed
            headers
        );
        
        logger.log("Authentication test successful. Response: %\n", response.substr(0, 200) + "...");
        return true;
    } catch (const std::exception& e) {
        logger.log("Authentication test failed: %\n", e.what());
        return false;
    }
}

int main(int argc, char* argv[]) {
    // Set up signal handler
    signal(SIGINT, signal_handler);
    
    // Create logger
    Common::Logger logger("/home/praveen/omlaxmiquant/ida/logs/binance_test.log");
    logger.log("Starting Binance test application\n");
    
    // Parse command-line arguments
    std::vector<std::string> symbols;
    bool use_testnet = true;  // Default to testnet for safety
    bool test_auth = false;
    
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--testnet=false") {
                use_testnet = false;
            } else if (arg == "--testnet=true") {
                use_testnet = true;
            } else if (arg == "--test-auth") {
                test_auth = true;
            } else if (arg.find("--") != 0) {
                symbols.push_back(arg);
            }
        }
    }
    
    if (symbols.empty()) {
        // Default symbol
        symbols.push_back("BTCUSDT");
    }
    
    std::cout << "Testing Binance integration with " << (use_testnet ? "testnet" : "mainnet") << std::endl;
    std::cout << "Symbols: ";
    for (const auto& symbol : symbols) {
        std::cout << symbol << " ";
    }
    std::cout << std::endl;
    
    // Create market updates queue (with space for 1024 updates)
    Exchange::MEMarketUpdateLFQueue market_updates(1024);
    
    // Create the Binance authenticator
    Trading::BinanceAuthenticator authenticator(logger);
    
    // Test authentication if requested
    if (test_auth) {
        bool auth_success = testAuthentication(logger, authenticator);
        std::cout << "Authentication test " << (auth_success ? "successful" : "failed") << std::endl;
        
        if (!auth_success) {
            // If authentication fails, we might want to continue with market data anyway
            std::cout << "Continuing with market data retrieval..." << std::endl;
        }
    }
    
    // Create the Binance market data consumer
    Trading::BinanceMarketDataConsumer consumer(1, &market_updates, symbols, use_testnet);
    
    // Start the consumer
    consumer.start();
    
    std::cout << "Market data consumer started. Press Ctrl+C to stop." << std::endl;
    
    // Process market updates
    std::string time_str;
    while (running) {
        for (auto market_update = market_updates.getNextToRead(); 
             market_update; 
             market_update = market_updates.getNextToRead()) {
             
            // Process market update
            std::cout << Common::getCurrentTimeStr(&time_str) 
                      << " Received " << Exchange::marketUpdateTypeToString(market_update->type_) 
                      << " for ticker " << Common::tickerIdToString(market_update->ticker_id_);
            
            if (market_update->type_ == Exchange::MarketUpdateType::TRADE) {
                std::cout << " - TRADE: " << Common::sideToString(market_update->side_) 
                         << " " << Common::qtyToString(market_update->qty_) 
                         << " @ " << Common::priceToString(market_update->price_);
            } else if (market_update->type_ == Exchange::MarketUpdateType::ADD ||
                      market_update->type_ == Exchange::MarketUpdateType::MODIFY) {
                std::cout << " - " << Common::sideToString(market_update->side_) 
                         << " " << Common::qtyToString(market_update->qty_) 
                         << " @ " << Common::priceToString(market_update->price_);
            }
            
            std::cout << std::endl;
            
            market_updates.updateReadIndex();
        }
        
        // Check book status every second
        for (const auto& symbol : symbols) {
            if (consumer.isOrderBookValid(symbol)) {
                Common::Price best_bid = consumer.getBestBidPrice(symbol);
                Common::Price best_ask = consumer.getBestAskPrice(symbol);
                
                if (best_bid != Common::Price_INVALID && best_ask != Common::Price_INVALID) {
                    std::cout << Common::getCurrentTimeStr(&time_str) 
                              << " " << symbol << " best bid: " << Common::priceToString(best_bid) 
                              << " best ask: " << Common::priceToString(best_ask)
                              << " spread: " << Common::priceToString(best_ask - best_bid)
                              << std::endl;
                }
            } else {
                std::cout << Common::getCurrentTimeStr(&time_str) 
                          << " " << symbol << " order book not yet valid" << std::endl;
            }
        }
        
        // Sleep a bit to prevent high CPU usage
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    // Stop the consumer
    consumer.stop();
    
    std::cout << "Test complete." << std::endl;
    logger.log("Binance test application finished\n");
    
    return 0;
}