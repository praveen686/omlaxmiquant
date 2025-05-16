#include <csignal>
#include <fstream>
#include <nlohmann/json.hpp>

#include "strategy/trade_engine.h"
#include "order_gw/order_gateway.h"
#include "market_data/market_data_consumer.h"

#include "common/logging.h"

using json = nlohmann::json;

/// Main components.
Common::Logger *logger = nullptr;
Trading::TradeEngine *trade_engine = nullptr;
Trading::MarketDataConsumer *market_data_consumer = nullptr;
Trading::OrderGateway *order_gateway = nullptr;

/// Loads configuration from JSON file
bool loadConfigFromJson(const std::string& algo_type_str, Common::TradeEngineCfgHashMap& ticker_cfg, 
                        std::string& order_gw_ip, std::string& order_gw_iface, int& order_gw_port,
                        std::string& mkt_data_iface, std::string& snapshot_ip, int& snapshot_port,
                        std::string& incremental_ip, int& incremental_port, std::string& time_str) {
  const std::string config_path = "/home/praveen/omlaxmiquant/ida/config/StrategyConfig.json";
  
  try {
    // Open config file
    std::ifstream config_file(config_path);
    if (!config_file.is_open()) {
      if (logger) logger->log("%:% %() % Failed to open config file: %\n", __FILE__, __LINE__, __FUNCTION__, 
                          Common::getCurrentTimeStr(&time_str), config_path.c_str());
      return false;
    }
    
    // Parse JSON
    json config;
    config_file >> config;
    
    // Check if strategy exists in config
    if (!config["strategies"].contains(algo_type_str)) {
      if (logger) logger->log("%:% %() % Strategy % not found in config\n", __FILE__, __LINE__, __FUNCTION__, 
                          Common::getCurrentTimeStr(&time_str), algo_type_str.c_str());
      return false;
    }
    
    // Load strategy configuration
    const auto& strategy_config = config["strategies"][algo_type_str];
    
    // Load ticker configurations
    for (const auto& ticker : strategy_config["tickers"]) {
      size_t ticker_id = ticker["ticker_id"];
      if (ticker_id < Common::ME_MAX_TICKERS) {
        ticker_cfg.at(ticker_id) = {
          static_cast<Common::Qty>(ticker["clip"]),
          ticker["threshold"],
          {
            static_cast<Common::Qty>(ticker["risk"]["max_order_size"]),
            static_cast<Common::Qty>(ticker["risk"]["max_position"]),
            ticker["risk"]["max_loss"]
          }
        };
      }
    }
    
    // Load global settings
    if (config.contains("global_settings")) {
      const auto& global = config["global_settings"];
      
      // Load market data settings
      if (global.contains("market_data")) {
        const auto& md = global["market_data"];
        if (md.contains("snapshot_ip")) snapshot_ip = md["snapshot_ip"];
        if (md.contains("snapshot_port")) snapshot_port = md["snapshot_port"];
        if (md.contains("incremental_ip")) incremental_ip = md["incremental_ip"];
        if (md.contains("incremental_port")) incremental_port = md["incremental_port"];
        if (md.contains("interface")) mkt_data_iface = md["interface"];
      }
      
      // Load order gateway settings
      if (global.contains("order_gateway")) {
        const auto& og = global["order_gateway"];
        if (og.contains("ip")) order_gw_ip = og["ip"];
        if (og.contains("port")) order_gw_port = og["port"];
        if (og.contains("interface")) order_gw_iface = og["interface"];
      }
    }
    
    if (logger) logger->log("%:% %() % Successfully loaded config for % strategy\n", __FILE__, __LINE__, __FUNCTION__, 
                        Common::getCurrentTimeStr(&time_str), algo_type_str.c_str());
    return true;
    
  } catch (const std::exception& e) {
    if (logger) logger->log("%:% %() % Error parsing config file: %\n", __FILE__, __LINE__, __FUNCTION__, 
                        Common::getCurrentTimeStr(&time_str), e.what());
    return false;
  }
}

/// ./trading_main CLIENT_ID ALGO_TYPE [CLIP_1 THRESH_1 MAX_ORDER_SIZE_1 MAX_POS_1 MAX_LOSS_1] [CLIP_2 THRESH_2 MAX_ORDER_SIZE_2 MAX_POS_2 MAX_LOSS_2] ...
int main(int argc, char **argv) {
  if(argc < 3) {
    FATAL("USAGE trading_main CLIENT_ID ALGO_TYPE [CLIP_1 THRESH_1 MAX_ORDER_SIZE_1 MAX_POS_1 MAX_LOSS_1] [CLIP_2 THRESH_2 MAX_ORDER_SIZE_2 MAX_POS_2 MAX_LOSS_2] ...");
  }

  const Common::ClientId client_id = atoi(argv[1]);
  srand(client_id);

  const auto algo_type = stringToAlgoType(argv[2]);
  const std::string algo_type_str = algoTypeToString(algo_type);

  logger = new Common::Logger("/home/praveen/omlaxmiquant/ida/logs/trading_main_" + std::to_string(client_id) + ".log");

  const int sleep_time = 20 * 1000;

  // The lock free queues to facilitate communication between order gateway <-> trade engine and market data consumer -> trade engine.
  Exchange::ClientRequestLFQueue client_requests(ME_MAX_CLIENT_UPDATES);
  Exchange::ClientResponseLFQueue client_responses(ME_MAX_CLIENT_UPDATES);
  Exchange::MEMarketUpdateLFQueue market_updates(ME_MAX_MARKET_UPDATES);

  std::string time_str;

  // Initialize default network settings that can be overridden by config
  std::string order_gw_ip = "127.0.0.1";
  std::string order_gw_iface = "lo";
  int order_gw_port = 12345;
  std::string mkt_data_iface = "lo";
  std::string snapshot_ip = "233.252.14.1";
  int snapshot_port = 20000;
  std::string incremental_ip = "233.252.14.3";
  int incremental_port = 20001;

  // Initialize TradeEngineCfgHashMap with empty config
  TradeEngineCfgHashMap ticker_cfg;

  // Try to load configuration from JSON file
  bool config_loaded = false;
  if (argc == 3) { // Only client_id and algo_type provided, attempt to use config file
    config_loaded = loadConfigFromJson(algo_type_str, ticker_cfg, 
                                      order_gw_ip, order_gw_iface, order_gw_port,
                                      mkt_data_iface, snapshot_ip, snapshot_port,
                                      incremental_ip, incremental_port, time_str);
    
    if (config_loaded) {
      logger->log("%:% %() % Successfully loaded configuration from JSON file\n", 
                 __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
    } else {
      logger->log("%:% %() % Failed to load configuration from JSON file. Using command line args.\n", 
                 __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
    }
  }

  // If config wasn't loaded from file and command line args were provided, use those
  if (!config_loaded && argc > 3) {
    logger->log("%:% %() % Using command line arguments for configuration\n", 
               __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
               
    // Parse and initialize the TradeEngineCfgHashMap from command line arguments
    // [CLIP_1 THRESH_1 MAX_ORDER_SIZE_1 MAX_POS_1 MAX_LOSS_1] [CLIP_2 THRESH_2 MAX_ORDER_SIZE_2 MAX_POS_2 MAX_LOSS_2] ...
    size_t next_ticker_id = 0;
    for (int i = 3; i < argc; i += 5, ++next_ticker_id) {
      ticker_cfg.at(next_ticker_id) = {static_cast<Qty>(std::atoi(argv[i])), std::atof(argv[i + 1]),
                                      {static_cast<Qty>(std::atoi(argv[i + 2])),
                                        static_cast<Qty>(std::atoi(argv[i + 3])),
                                        std::atof(argv[i + 4])}};
    }
  }

  logger->log("%:% %() % Starting Trade Engine...\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
  trade_engine = new Trading::TradeEngine(client_id, algo_type,
                                          ticker_cfg,
                                          &client_requests,
                                          &client_responses,
                                          &market_updates);
  trade_engine->start();

  logger->log("%:% %() % Starting Order Gateway...\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
  order_gateway = new Trading::OrderGateway(client_id, &client_requests, &client_responses, order_gw_ip, order_gw_iface, order_gw_port);
  order_gateway->start();

  logger->log("%:% %() % Starting Market Data Consumer...\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str));
  market_data_consumer = new Trading::MarketDataConsumer(client_id, &market_updates, mkt_data_iface, snapshot_ip, snapshot_port, incremental_ip, incremental_port);
  market_data_consumer->start();

  usleep(10 * 1000 * 1000);

  trade_engine->initLastEventTime();

  // For the random trading algorithm, we simply implement it here instead of creating a new trading algorithm which is another possibility.
  // Generate random orders with random attributes and randomly cancel some of them.
  if (algo_type == AlgoType::RANDOM) {
    Common::OrderId order_id = client_id * 1000;
    std::vector<Exchange::MEClientRequest> client_requests_vec;
    std::array<Price, ME_MAX_TICKERS> ticker_base_price;
    for (size_t i = 0; i < ME_MAX_TICKERS; ++i)
      ticker_base_price[i] = (rand() % 100) + 100;
    for (size_t i = 0; i < 10000; ++i) {
      const Common::TickerId ticker_id = rand() % Common::ME_MAX_TICKERS;
      const Price price = ticker_base_price[ticker_id] + (rand() % 10) + 1;
      const Qty qty = 1 + (rand() % 100) + 1;
      const Side side = (rand() % 2 ? Common::Side::BUY : Common::Side::SELL);

      Exchange::MEClientRequest new_request{Exchange::ClientRequestType::NEW, client_id, ticker_id, order_id++, side,
                                            price, qty};
      trade_engine->sendClientRequest(&new_request);
      usleep(sleep_time);

      client_requests_vec.push_back(new_request);
      const auto cxl_index = rand() % client_requests_vec.size();
      auto cxl_request = client_requests_vec[cxl_index];
      cxl_request.type_ = Exchange::ClientRequestType::CANCEL;
      trade_engine->sendClientRequest(&cxl_request);
      usleep(sleep_time);

      if (trade_engine->silentSeconds() >= 60) {
        logger->log("%:% %() % Stopping early because been silent for % seconds...\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str), trade_engine->silentSeconds());

        break;
      }
    }
  }

  while (trade_engine->silentSeconds() < 60) {
    logger->log("%:% %() % Waiting till no activity, been silent for % seconds...\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str), trade_engine->silentSeconds());

    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(30s);
  }

  trade_engine->stop();
  market_data_consumer->stop();
  order_gateway->stop();

  using namespace std::literals::chrono_literals;
  std::this_thread::sleep_for(10s);

  delete logger;
  logger = nullptr;
  delete trade_engine;
  trade_engine = nullptr;
  delete market_data_consumer;
  market_data_consumer = nullptr;
  delete order_gateway;
  order_gateway = nullptr;

  std::this_thread::sleep_for(10s);

  exit(EXIT_SUCCESS);
}
