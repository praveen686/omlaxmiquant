#pragma once

#include <map>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

#include "common/types.h"
#include "common/macros.h"
#include "common/logging.h"
#include "exchange/market_data/market_update.h"

namespace Trading {

/**
 * @brief Structure to represent a price level in the order book
 */
struct PriceLevel {
    Common::Price price;
    Common::Qty quantity;
};

/**
 * @brief Implementation of local order book for Binance
 *
 * This class maintains a local order book state by processing WebSocket updates 
 * and REST API snapshots from Binance. It handles:
 * - Initial order book snapshots
 * - Incremental depth updates
 * - Update sequence validation
 * - Periodic book health checks
 */
class BinanceOrderBook {
public:
    /**
     * @brief Constructor
     * @param symbol The trading symbol (e.g., "BTCUSDT")
     * @param ticker_id Internal ticker ID that maps to this symbol
     * @param logger Logger instance for recording activities
     */
    BinanceOrderBook(const std::string& symbol, Common::TickerId ticker_id, Logger& logger);

    /**
     * @brief Destructor
     */
    ~BinanceOrderBook() = default;

    /**
     * @brief Apply snapshot from REST API to initialize the order book
     * @param last_update_id The last update ID from the snapshot
     * @param bids Vector of bid price levels
     * @param asks Vector of ask price levels
     * @return true if snapshot was applied successfully
     */
    bool applySnapshot(uint64_t last_update_id, 
                      const std::vector<PriceLevel>& bids,
                      const std::vector<PriceLevel>& asks);

    /**
     * @brief Process an incremental depth update from WebSocket
     * @param first_update_id First update ID in this event
     * @param final_update_id Final update ID in this event
     * @param bids Vector of bid price levels to update
     * @param asks Vector of ask price levels to update
     * @return true if update was applied successfully
     */
    bool applyDepthUpdate(uint64_t first_update_id, 
                         uint64_t final_update_id,
                         const std::vector<PriceLevel>& bids,
                         const std::vector<PriceLevel>& asks);

    /**
     * @brief Check if the book needs to be refreshed from a snapshot
     * @return true if a refresh is needed
     */
    bool needsRefresh() const;

    /**
     * @brief Generate market updates from the current state of the book
     * @param updates Vector to populate with market updates
     */
    void generateMarketUpdates(std::vector<Exchange::MEMarketUpdate>& updates);

    /**
     * @brief Get the best bid price
     * @return The best bid price or Price_INVALID if no bids
     */
    Common::Price getBestBidPrice() const;

    /**
     * @brief Get the best ask price
     * @return The best ask price or Price_INVALID if no asks
     */
    Common::Price getBestAskPrice() const;

    /**
     * @brief Get the quantity available at a specific price level
     * @param price The price level to check
     * @param side The side (BID or ASK)
     * @return The quantity available or 0 if none
     */
    Common::Qty getQuantityAtPrice(Common::Price price, Common::Side side) const;

    /**
     * @brief Get the status of the order book
     * @return true if the order book is synchronized and valid
     */
    bool isValid() const { return is_valid_; }

private:
    // Symbol and ticker information
    std::string symbol_;
    Common::TickerId ticker_id_;
    
    // Maps to store bids and asks (price → quantity)
    std::map<Common::Price, Common::Qty, std::greater<Common::Price>> bids_; // Descending order
    std::map<Common::Price, Common::Qty> asks_; // Ascending order
    
    // Sequence tracking
    uint64_t last_update_id_ = 0;
    
    // State flags
    std::atomic<bool> is_valid_{false};
    std::atomic<bool> needs_refresh_{true};
    
    // Synchronization
    mutable std::mutex book_mutex_;
    
    // Logging
    Logger& logger_;
    std::string time_str_;

    // Utility functions
    void processPriceLevelUpdates(std::map<Common::Price, Common::Qty, std::greater<Common::Price>>& book_side, 
                                 const std::vector<PriceLevel>& updates);
    void processPriceLevelUpdates(std::map<Common::Price, Common::Qty>& book_side, 
                                 const std::vector<PriceLevel>& updates);
};

} // namespace Trading