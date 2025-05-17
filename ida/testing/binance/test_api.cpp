#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <signal.h>
#include <map>

#include "common/logging.h"
#include "market_data/binance_authenticator.h"
#include "market_data/binance_http_client.h"
#include "market_data/binance_market_data_consumer.h"

std::atomic<bool> running{true};

void signal_handler(int signal) {
    std::cout << "Caught signal " << signal << ", shutting down..." << std::endl;
    running = false;
}

// Test account information API
bool test_account_info(Common::Logger& logger, Trading::BinanceAuthenticator& auth) {
    logger.log("Testing Binance account information API\n");
    
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
    
    // Make account info request
    try {
        std::string response = http_client.get(
            auth.getRestBaseUrl(),
            "/api/v3/account?" + signed_query,
            {},  // No additional query params needed
            headers
        );
        
        logger.log("Account info API test successful. Response: %\n", response.substr(0, 200) + "...");
        std::cout << "Account Information API test PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        logger.log("Account info API test failed: %\n", e.what());
        std::cout << "Account Information API test FAILED: " << e.what() << std::endl;
        return false;
    }
}

// Test exchange information API
bool test_exchange_info(Common::Logger& logger, Trading::BinanceAuthenticator& auth) {
    logger.log("Testing Binance exchange information API\n");
    
    // Create the HTTP client
    Trading::BinanceHttpClient http_client(logger);
    
    // Exchange info doesn't require authentication
    try {
        std::string response = http_client.get(
            auth.getRestBaseUrl(),
            "/api/v3/exchangeInfo",
            { {"symbol", "BTCUSDT"} }  // Get info for BTCUSDT
        );
        
        logger.log("Exchange info API test successful. Response: %\n", response.substr(0, 200) + "...");
        std::cout << "Exchange Information API test PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        logger.log("Exchange info API test failed: %\n", e.what());
        std::cout << "Exchange Information API test FAILED: " << e.what() << std::endl;
        return false;
    }
}

// Test order book API
bool test_order_book(Common::Logger& logger, Trading::BinanceAuthenticator& auth) {
    logger.log("Testing Binance order book API\n");
    
    // Create the HTTP client
    Trading::BinanceHttpClient http_client(logger);
    
    // Order book doesn't require authentication
    try {
        std::string response = http_client.get(
            auth.getRestBaseUrl(),
            "/api/v3/depth",
            { {"symbol", "BTCUSDT"}, {"limit", "10"} }
        );
        
        logger.log("Order book API test successful. Response: %\n", response.substr(0, 200) + "...");
        std::cout << "Order Book API test PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        logger.log("Order book API test failed: %\n", e.what());
        std::cout << "Order Book API test FAILED: " << e.what() << std::endl;
        return false;
    }
}

// Test recent trades API
bool test_recent_trades(Common::Logger& logger, Trading::BinanceAuthenticator& auth) {
    logger.log("Testing Binance recent trades API\n");
    
    // Create the HTTP client
    Trading::BinanceHttpClient http_client(logger);
    
    // Recent trades doesn't require authentication
    try {
        std::string response = http_client.get(
            auth.getRestBaseUrl(),
            "/api/v3/trades",
            { {"symbol", "BTCUSDT"}, {"limit", "10"} }
        );
        
        logger.log("Recent trades API test successful. Response: %\n", response.substr(0, 200) + "...");
        std::cout << "Recent Trades API test PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        logger.log("Recent trades API test failed: %\n", e.what());
        std::cout << "Recent Trades API test FAILED: " << e.what() << std::endl;
        return false;
    }
}

// Test ping API
bool test_ping(Common::Logger& logger, Trading::BinanceAuthenticator& auth) {
    logger.log("Testing Binance ping API\n");
    
    // Create the HTTP client
    Trading::BinanceHttpClient http_client(logger);
    
    // Ping doesn't require authentication
    try {
        std::string response = http_client.get(
            auth.getRestBaseUrl(),
            "/api/v3/ping"
        );
        
        logger.log("Ping API test successful. Response: %\n", response);
        std::cout << "Ping API test PASSED" << std::endl;
        return true;
    } catch (const std::exception& e) {
        logger.log("Ping API test failed: %\n", e.what());
        std::cout << "Ping API test FAILED: " << e.what() << std::endl;
        return false;
    }
}

int main(int argc, char* argv[]) {
    // Set up signal handler
    signal(SIGINT, signal_handler);
    
    // Create logger
    Common::Logger logger("/home/praveen/omlaxmiquant/ida/logs/binance_api_test.log");
    logger.log("Starting Binance API tests\n");
    
    // Parse command-line arguments
    bool test_auth = false;
    bool test_market = false;
    bool use_testnet = true;  // Default to testnet for safety
    
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--test-auth") {
                test_auth = true;
            } else if (arg == "--test-market") {
                test_market = true;
            } else if (arg == "--testnet=false") {
                use_testnet = false;
            } else if (arg == "--testnet=true") {
                use_testnet = true;
            }
        }
    }
    
    // If no specific tests are requested, run all tests
    if (!test_auth && !test_market) {
        test_auth = true;
        test_market = true;
    }
    
    std::cout << "Binance API Test" << std::endl;
    std::cout << "Using " << (use_testnet ? "testnet" : "mainnet") << std::endl;
    std::cout << "Tests enabled: " 
              << (test_auth ? "Authentication " : "")
              << (test_market ? "Market Data" : "")
              << std::endl;
    
    // Create the Binance authenticator
    Trading::BinanceAuthenticator authenticator(logger);
    
    bool overall_success = true;
    
    // Test ping API (basic connectivity)
    overall_success &= test_ping(logger, authenticator);
    
    // Test market data API endpoints if enabled
    if (test_market) {
        overall_success &= test_exchange_info(logger, authenticator);
        overall_success &= test_order_book(logger, authenticator);
        overall_success &= test_recent_trades(logger, authenticator);
    }
    
    // Test authentication-based API endpoints if enabled
    if (test_auth) {
        overall_success &= test_account_info(logger, authenticator);
    }
    
    // Final status
    std::cout << std::endl;
    std::cout << "Tests completed. Overall status: " << (overall_success ? "PASSED" : "FAILED") << std::endl;
    
    logger.log("Binance API tests finished\n");
    return overall_success ? 0 : 1;
}