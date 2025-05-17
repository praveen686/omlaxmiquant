#pragma once

#include "common/types.h"

namespace Trading {
namespace Binance {

// Multipliers for Binance-specific price and quantity conversions
// Binance often uses more decimal places than our internal representation

// For prices (7 decimal places for most cryptocurrencies on Binance)
constexpr int64_t PriceMultiplier = 10000000;  

// For quantities (8 decimal places for most cryptocurrencies on Binance)
constexpr uint32_t QtyMultiplier = 100000000;

/**
 * @brief Convert a Binance decimal price to internal price representation
 * @param decimal_price The price in decimal format (e.g., 45123.45)
 * @return The price in internal format
 */
inline Common::Price binancePriceToInternal(double decimal_price) {
    return static_cast<Common::Price>(decimal_price * 10000.0);  // Use existing system multiplier for compatibility
}

/**
 * @brief Convert an internal price to Binance decimal format
 * @param internal_price The price in internal format
 * @return The price in Binance decimal format
 */
inline double internalPriceToBinance(Common::Price internal_price) {
    return static_cast<double>(internal_price) / 10000.0;  // Use existing system multiplier for compatibility
}

/**
 * @brief Convert a Binance decimal quantity to internal quantity representation
 * @param decimal_qty The quantity in decimal format (e.g., 0.01)
 * @return The quantity in internal format
 */
inline Common::Qty binanceQtyToInternal(double decimal_qty) {
    return static_cast<Common::Qty>(decimal_qty * 10000.0);  // Use existing system multiplier for compatibility
}

/**
 * @brief Convert an internal quantity to Binance decimal format
 * @param internal_qty The quantity in internal format
 * @return The quantity in Binance decimal format
 */
inline double internalQtyToBinance(Common::Qty internal_qty) {
    return static_cast<double>(internal_qty) / 10000.0;  // Use existing system multiplier for compatibility
}

/**
 * @brief Convert a price string from Binance to internal format
 * @param price_str The price as a string (e.g., "45123.45")
 * @return The price in internal format
 */
inline Common::Price binancePriceStringToInternal(const std::string& price_str) {
    try {
        double price_dbl = std::stod(price_str);
        return binancePriceToInternal(price_dbl);
    } catch (const std::exception&) {
        return Common::Price_INVALID;
    }
}

/**
 * @brief Convert a quantity string from Binance to internal format
 * @param qty_str The quantity as a string (e.g., "0.01")
 * @return The quantity in internal format
 */
inline Common::Qty binanceQtyStringToInternal(const std::string& qty_str) {
    try {
        double qty_dbl = std::stod(qty_str);
        return binanceQtyToInternal(qty_dbl);
    } catch (const std::exception&) {
        return Common::Qty_INVALID;
    }
}

} // namespace Binance
} // namespace Trading