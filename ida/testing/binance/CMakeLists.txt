# Binance Integration Tests

# Market Data Test
add_executable(test_market_data
  test_market_data.cpp
)

target_link_libraries(test_market_data
  libtrading
  libexchange
  libcommon
  ${Boost_LIBRARIES}
  ${OPENSSL_LIBRARIES}
  nlohmann_json::nlohmann_json
  pthread
)

# API Test
add_executable(test_binance_api
  test_api.cpp
)

target_link_libraries(test_binance_api
  libtrading
  libexchange
  libcommon
  ${Boost_LIBRARIES}
  ${OPENSSL_LIBRARIES}
  nlohmann_json::nlohmann_json
  pthread
)

# Order Gateway Test
add_executable(test_order_gateway
  test_order_gateway.cpp
)

target_link_libraries(test_order_gateway
  libtrading
  libexchange
  libcommon
  ${Boost_LIBRARIES}
  ${OPENSSL_LIBRARIES}
  nlohmann_json::nlohmann_json
  pthread
)