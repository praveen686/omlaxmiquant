#include "binance_order_book.h"

namespace Trading {

BinanceOrderBook::BinanceOrderBook(const std::string& symbol, Common::TickerId ticker_id, Logger& logger)
    : symbol_(symbol), ticker_id_(ticker_id), logger_(logger) {
    logger_.log("%:% %() Creating order book for symbol: %\n", 
                __FILE__, __LINE__, __FUNCTION__, symbol_);
}

bool BinanceOrderBook::applySnapshot(uint64_t last_update_id, 
                                   const std::vector<PriceLevel>& bids,
                                   const std::vector<PriceLevel>& asks) {
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    logger_.log("%:% %() % Applying snapshot for % with last_update_id: %\n", 
                __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), symbol_, last_update_id);
    
    // Clear existing book
    bids_.clear();
    asks_.clear();
    
    // Apply bids
    for (const auto& level : bids) {
        if (level.quantity > 0) {
            bids_[level.price] = level.quantity;
        }
    }
    
    // Apply asks
    for (const auto& level : asks) {
        if (level.quantity > 0) {
            asks_[level.price] = level.quantity;
        }
    }
    
    last_update_id_ = last_update_id;
    is_valid_ = true;
    needs_refresh_ = false;
    
    logger_.log("%:% %() % Snapshot applied for %. Bids: %, Asks: %\n", 
                __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                symbol_, bids_.size(), asks_.size());
    
    return true;
}

bool BinanceOrderBook::applyDepthUpdate(uint64_t first_update_id, 
                                      uint64_t final_update_id,
                                      const std::vector<PriceLevel>& bids,
                                      const std::vector<PriceLevel>& asks) {
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    // Check sequence integrity
    if (!is_valid_) {
        logger_.log("%:% %() % Cannot apply update: order book not initialized with snapshot\n", 
                    __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        needs_refresh_ = true;
        return false;
    }
    
    // According to Binance documentation, process updates only if:
    // 1. firstUpdateId <= lastUpdateId+1 AND finalUpdateId >= lastUpdateId+1
    // Drop if u_id < lastUpdateId+1
    if (final_update_id < last_update_id_ + 1) {
        // This update is older than our current state, ignore it
        logger_.log("%:% %() % Ignoring outdated update: final_id: % <= our_last_id+1: %\n", 
                    __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                    final_update_id, last_update_id_ + 1);
        return false;
    }
    
    // If there's a gap, we need to refresh the book
    if (first_update_id > last_update_id_ + 1) {
        logger_.log("%:% %() % Sequence gap detected: first_id: % > our_last_id+1: %. Setting refresh flag.\n", 
                    __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                    first_update_id, last_update_id_ + 1);
        needs_refresh_ = true;
        return false;
    }
    
    // Apply the bid updates
    processPriceLevelUpdates(bids_, bids);
    
    // Apply the ask updates
    processPriceLevelUpdates(asks_, asks);
    
    // Update our last processed ID
    last_update_id_ = final_update_id;
    
    logger_.log("%:% %() % Update applied for %. New last_update_id: %. Bids: %, Asks: %\n", 
                __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                symbol_, last_update_id_, bids_.size(), asks_.size());
    
    return true;
}

bool BinanceOrderBook::needsRefresh() const {
    return needs_refresh_;
}

void BinanceOrderBook::generateMarketUpdates(std::vector<Exchange::MEMarketUpdate>& updates) {
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    if (!is_valid_) {
        logger_.log("%:% %() % Cannot generate updates: order book not valid\n", 
                    __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
        return;
    }
    
    // First, create a CLEAR update to reset the book
    Exchange::MEMarketUpdate clear_update;
    clear_update.type_ = Exchange::MarketUpdateType::CLEAR;
    clear_update.ticker_id_ = ticker_id_;
    updates.push_back(clear_update);
    
    // Add all bids
    uint32_t priority = 1;
    for (const auto& bid : bids_) {
        Exchange::MEMarketUpdate update;
        update.type_ = Exchange::MarketUpdateType::ADD;
        update.ticker_id_ = ticker_id_;
        update.side_ = Common::Side::BUY;
        update.price_ = bid.first;
        update.qty_ = bid.second;
        update.priority_ = priority++;
        update.order_id_ = static_cast<Common::OrderId>(bid.first); // Use price as a unique ID
        
        updates.push_back(update);
    }
    
    // Add all asks
    priority = 1;
    for (const auto& ask : asks_) {
        Exchange::MEMarketUpdate update;
        update.type_ = Exchange::MarketUpdateType::ADD;
        update.ticker_id_ = ticker_id_;
        update.side_ = Common::Side::SELL;
        update.price_ = ask.first;
        update.qty_ = ask.second;
        update.priority_ = priority++;
        update.order_id_ = static_cast<Common::OrderId>(ask.first); // Use price as a unique ID
        
        updates.push_back(update);
    }
    
    logger_.log("%:% %() % Generated % market updates for %\n", 
                __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), 
                updates.size(), symbol_);
}

Common::Price BinanceOrderBook::getBestBidPrice() const {
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    if (!is_valid_ || bids_.empty()) {
        return Common::Price_INVALID;
    }
    
    return bids_.begin()->first;
}

Common::Price BinanceOrderBook::getBestAskPrice() const {
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    if (!is_valid_ || asks_.empty()) {
        return Common::Price_INVALID;
    }
    
    return asks_.begin()->first;
}

Common::Qty BinanceOrderBook::getQuantityAtPrice(Common::Price price, Common::Side side) const {
    std::lock_guard<std::mutex> lock(book_mutex_);
    
    if (!is_valid_) {
        return 0;
    }
    
    if (side == Common::Side::BUY) {
        auto it = bids_.find(price);
        return (it != bids_.end()) ? it->second : 0;
    } else {
        auto it = asks_.find(price);
        return (it != asks_.end()) ? it->second : 0;
    }
}

void BinanceOrderBook::processPriceLevelUpdates(std::map<Common::Price, Common::Qty, std::greater<Common::Price>>& book_side, 
                                              const std::vector<PriceLevel>& updates) {
    for (const auto& level : updates) {
        if (level.quantity > 0) {
            // Add or update price level
            book_side[level.price] = level.quantity;
        } else {
            // Remove price level
            book_side.erase(level.price);
        }
    }
}

void BinanceOrderBook::processPriceLevelUpdates(std::map<Common::Price, Common::Qty>& book_side, 
                                              const std::vector<PriceLevel>& updates) {
    for (const auto& level : updates) {
        if (level.quantity > 0) {
            // Add or update price level
            book_side[level.price] = level.quantity;
        } else {
            // Remove price level
            book_side.erase(level.price);
        }
    }
}

} // namespace Trading