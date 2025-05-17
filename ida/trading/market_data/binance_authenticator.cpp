#include "binance_authenticator.h"
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <openssl/hmac.h>

namespace Trading {

BinanceAuthenticator::BinanceAuthenticator(Common::Logger& logger, const std::string& vault_path)
    : logger_(logger), vault_path_(vault_path) {
    loadCredentials();
}

bool BinanceAuthenticator::loadCredentials() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        logger_.log("%:% %() % Loading credentials from vault: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  vault_path_);
        
        // Open the vault file
        std::ifstream vault_file(vault_path_);
        if (!vault_file.is_open()) {
            logger_.log("%:% %() % Failed to open vault file: %\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                      vault_path_);
            return false;
        }
        
        // Parse the JSON
        nlohmann::json vault_json;
        vault_file >> vault_json;
        
        // Check if the Binance testnet credentials are present
        if (!vault_json.contains("binance_testnet")) {
            logger_.log("%:% %() % Vault file does not contain binance_testnet credentials\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
            return false;
        }
        
        // Extract the credentials
        const auto& binance_testnet = vault_json["binance_testnet"];
        
        if (!binance_testnet.contains("api_key") || !binance_testnet.contains("secret_key")) {
            logger_.log("%:% %() % Binance testnet credentials are incomplete\n", 
                      __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
            return false;
        }
        
        api_key_ = binance_testnet["api_key"].get<std::string>();
        secret_key_ = binance_testnet["secret_key"].get<std::string>();
        
        // Check if use_testnet is specified
        if (binance_testnet.contains("use_testnet")) {
            use_testnet_ = binance_testnet["use_testnet"].get<bool>();
        }
        
        credentials_loaded_ = true;
        logger_.log("%:% %() % Successfully loaded Binance credentials. Use testnet: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  use_testnet_);
                  
        return true;
    } catch (const std::exception& e) {
        logger_.log("%:% %() % Exception while loading credentials: %\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                  e.what());
        credentials_loaded_ = false;
        return false;
    }
}

bool BinanceAuthenticator::hasValidCredentials() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return credentials_loaded_ && !api_key_.empty() && !secret_key_.empty();
}

std::string BinanceAuthenticator::signRequest(const std::map<std::string, std::string>& parameters, bool with_timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!credentials_loaded_) {
        logger_.log("%:% %() % Cannot sign request: credentials not loaded\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        return "";
    }
    
    // Build query string from parameters
    std::stringstream query_string;
    for (const auto& param : parameters) {
        if (!query_string.str().empty()) {
            query_string << "&";
        }
        query_string << param.first << "=" << param.second;
    }
    
    // Add timestamp if requested
    if (with_timestamp) {
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        
        if (!query_string.str().empty()) {
            query_string << "&";
        }
        query_string << "timestamp=" << ms;
    }
    
    // Calculate HMAC-SHA256 signature
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_length = 0;
    
    HMAC(EVP_sha256(), secret_key_.c_str(), static_cast<int>(secret_key_.length()),
         reinterpret_cast<const unsigned char*>(query_string.str().c_str()),
         query_string.str().length(), hash, &hash_length);
    
    // Convert to hex string
    std::stringstream signature;
    signature << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < hash_length; i++) {
        signature << std::setw(2) << static_cast<unsigned int>(hash[i]);
    }
    
    // Add signature to query string
    query_string << "&signature=" << signature.str();
    
    return query_string.str();
}

void BinanceAuthenticator::addAuthHeaders(std::map<std::string, std::string>& headers) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (credentials_loaded_) {
        headers["X-MBX-APIKEY"] = api_key_;
    } else {
        logger_.log("%:% %() % Cannot add auth headers: credentials not loaded\n", 
                  __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    }
}

std::string BinanceAuthenticator::getApiKey() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return api_key_;
}

bool BinanceAuthenticator::isUsingTestnet() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return use_testnet_;
}

std::string BinanceAuthenticator::getRestBaseUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return use_testnet_ ? "testnet.binance.vision" : "api.binance.com";
}

std::string BinanceAuthenticator::getWsBaseUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return use_testnet_ ? "stream.testnet.binance.vision" : "stream.binance.com";
}

} // namespace Trading