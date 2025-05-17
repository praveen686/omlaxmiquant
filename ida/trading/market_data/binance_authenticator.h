#pragma once

#include <string>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include "common/logging.h"

namespace Trading {

/**
 * @brief Authenticator for Binance API
 *
 * Handles loading of API credentials, generating signatures for API requests,
 * and adding authentication headers to requests.
 */
class BinanceAuthenticator {
public:
    /**
     * @brief Constructor
     * @param logger Reference to the logger instance
     * @param vault_path Path to the vault file containing API credentials
     */
    BinanceAuthenticator(Common::Logger& logger, 
                         const std::string& vault_path = "/home/praveen/omlaxmiquant/ida/vault/vault.json");
    
    /**
     * @brief Loads API credentials from the vault file
     * @return true if credentials were successfully loaded, false otherwise
     */
    bool loadCredentials();
    
    /**
     * @brief Checks if credentials are loaded and valid
     * @return true if credentials are loaded and valid, false otherwise
     */
    bool hasValidCredentials() const;
    
    /**
     * @brief Generate a signature for a Binance API request
     * @param parameters Query parameters to include in the signature
     * @param with_timestamp Whether to include a timestamp in the signature
     * @return The signed query string
     */
    std::string signRequest(const std::map<std::string, std::string>& parameters, bool with_timestamp = true);
    
    /**
     * @brief Add authentication headers to a map of headers
     * @param headers Map of headers to add authentication to
     */
    void addAuthHeaders(std::map<std::string, std::string>& headers);
    
    /**
     * @brief Get the API key
     * @return The API key
     */
    std::string getApiKey() const;
    
    /**
     * @brief Check if using the testnet
     * @return true if using testnet, false if using mainnet
     */
    bool isUsingTestnet() const;
    
    /**
     * @brief Get the base URL for the Binance REST API
     * @return The base URL for the Binance REST API
     */
    std::string getRestBaseUrl() const;
    
    /**
     * @brief Get the base URL for the Binance WebSocket API
     * @return The base URL for the Binance WebSocket API
     */
    std::string getWsBaseUrl() const;
    
private:
    // Logging
    Common::Logger& logger_;
    std::string time_str_;
    
    // API credentials
    std::string api_key_;
    std::string secret_key_;
    bool use_testnet_ = true;
    
    // Path to the vault file
    std::string vault_path_;
    
    // Mutex for thread safety
    mutable std::mutex mutex_;
    
    // Load status
    bool credentials_loaded_ = false;
};

} // namespace Trading