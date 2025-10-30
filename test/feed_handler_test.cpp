#include "../include/feed_handler.hpp"
#include <gmock/gmock-actions.h>
#include <gmock/gmock-spec-builders.h>
#include <gtest/gtest.h>
#include <gmock/gmock.h>      // GoogleMock: EXPECT_CALL, matchers, actions

#include "mocks/mock_feed_handler.h"

using ::testing::_;           // The wildcard matcher: “accept any argument(s)”
using ::testing::Return;      // make a mocked method return a value

// LETS CREATE TEST WITH EXCHANGES ONCE THE WEBSOCKET WORKS !!!

// A test case named "FeedHandlerInterfaceTest" with a single test "InitAndStartLifecycle".
// TEST(...) is a GoogleTest macro that generates a function and registers it with the test runner.
TEST(FeedHandlerInterfaceTest, InitAndStartLifecycle) {
    // Create our mock object. Every MOCK_METHOD you declared is now callable & observable.
    md::MockFeedHandler fh;

    // Prepare a minimal config object that will be passed to fh.init(...).
    md::FeedHandlerConfig cfg;
    cfg.s.venue = 1;
    cfg.s.venue_name = "BINANCE";

    // ---- SETUP EXPECTATIONS (GoogleMock) -------------------------------------
    // EXPECT_CALL tells the mock: “I expect this method to be called (with these args),
    // and when it is, do X (here: Return(Status::OK)).”
    //
    // 1) We expect init(...) to be called once with ANY FeedHandlerConfig (the '_' matcher),
    //    and we tell the mock to return Status::OK for that call.
    EXPECT_CALL(fh, init(_))
        .WillOnce(Return(md::Status::OK));

    // 2) We expect start() to be called once and to return Status::OK.
    EXPECT_CALL(fh, start())
        .WillOnce(Return(md::Status::OK));

    // 3) We expect is_running() to be called once and to return true.
    EXPECT_CALL(fh, is_running())
        .WillOnce(Return(true));

    // ---- ACT (call the code under test) --------------------------------------
    // In a real system you’d call your “system under test” (SUT), which internally calls fh.*
    // For this introductory example we just call the mock directly to show the flow.

    // init(...) should hit expectation #1 and return OK.
    EXPECT_EQ(fh.init(cfg), md::Status::OK);

    // start() should hit expectation #2 and return OK.
    EXPECT_EQ(fh.start(), md::Status::OK);

    // is_running() should hit expectation #3 and return true.
    EXPECT_TRUE(fh.is_running());

    // ---- TEARDOWN (implicit) -------------------------------------------------
    // When the test scope ends, GoogleMock verifies all EXPECT_CALLs:
    // - if any expected call did NOT happen (or arg matchers failed), the test FAILS.
    // - if extra unexpected calls happened, you’ll get warnings (or failures with StrictMock).
}

TEST(FeedHandlerInterfaceTest, SymbolResolution) {
    md::MockFeedHandler fh;

    EXPECT_CALL(fh, resolve_symbol("BTCUSDT"))
        .WillOnce(Return(1001));
    EXPECT_CALL(fh, symbol_for(1001))
        .WillOnce(Return("BTCUSDT"));

    auto id = fh.resolve_symbol("BTCUSDT");
    EXPECT_EQ(id, 1001);
    EXPECT_EQ(fh.symbol_for(1001), "BTCUSDT");
}