# Design Decisions for Binance Integration

This document outlines key design decisions made during the Binance integration implementation.

## BinanceOrderGateway Implementation Approach

### Implementation Strategy: Independent Class vs. Inheritance

We considered two approaches for implementing the BinanceOrderGateway:

1. **Inheritance from OrderGateway**:
   - Extend the OrderGateway class
   - Override the `run()` method to use HTTP instead of sockets
   - Reuse existing queue handling logic
   - Would require modifying core component accessibility
   
2. **Independent Class Implementation (chosen approach)**:
   - Create a completely separate class
   - Implement similar interface but using REST API
   - Use same queue interface for communication
   - No modifications to core components

We chose the independent class approach because:
- It avoids modifying core components
- It allows for a clean implementation focused on REST API communication
- It provides flexibility to design Binance-specific features
- It eliminates the need to implement unused socket-based methods

### Benefits of Independent Class Approach

1. **Zero Impact on Core Components**:
   - No changes required to OrderGateway class
   - Existing code continues to work as expected
   - Easier to maintain backward compatibility

2. **Separation of Concerns**:
   - Socket-based logic remains in OrderGateway
   - HTTP/REST-based logic is isolated in BinanceOrderGateway
   - Cleaner mental model of the communication methods

## BinanceOrderGateway Implementation Details

### Key Components

1. **Queue Interface Compatibility**:
   - Uses the same lock-free queue types as OrderGateway
   - Maintains compatibility with existing trade engine
   - Processes client requests from outgoing_requests_ queue
   - Publishes responses to incoming_responses_ queue

2. **Request Processing**:
   - Converts internal order requests to Binance API format
   - Translates Exchange::ClientRequestType to Binance order types
   - Maps internal order IDs to Binance order IDs
   - Handles order lifecycle through REST API calls

3. **Response Handling**:
   - Translates Binance API responses to internal format
   - Generates Exchange::MEClientResponse objects
   - Places responses in queue for consumption by trade engine

## Extensibility for Future Exchanges

This approach establishes a pattern for adding support for additional exchanges:
1. Create an exchange-specific authenticator if needed
2. Create an exchange-specific HTTP or WebSocket client if needed
3. Implement an exchange-specific OrderGateway with queue compatibility
4. Implement exchange-specific request/response handling

This approach allows each exchange integration to use appropriate communication methods while maintaining a consistent interface with the trade engine.

## Testing Strategy

To ensure proper integration:
1. Test queue interaction with mock requests and responses
2. Manual testing of order flow with Binance testnet
3. Dedicated testing of the Binance-specific components
4. End-to-end testing of the full system with market data and order execution