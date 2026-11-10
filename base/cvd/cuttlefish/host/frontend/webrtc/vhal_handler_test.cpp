/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cuttlefish/host/frontend/webrtc/vhal_handler.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#include "json/value.h"

namespace cuttlefish {
namespace {

class EventCollector {
 public:
  void Push(const Json::Value& event) {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      events_.push_back(event);
    }
    cv_.notify_all();
  }

  bool WaitForCount(size_t count) {
    std::unique_lock<std::mutex> lock(mtx_);
    return cv_.wait_for(lock, std::chrono::seconds(5),
                        [this, count]() { return events_.size() >= count; });
  }

  std::vector<Json::Value> Events() {
    std::lock_guard<std::mutex> lock(mtx_);
    return events_;
  }

 private:
  std::mutex mtx_;
  std::condition_variable cv_;
  std::vector<Json::Value> events_;
};

TEST(VhalHandlerTest, ToggleHvacPowerSuccess) {
  std::vector<std::vector<std::string>> recorded_args;
  VhalHandler handler(
      9301,
      [&](const std::vector<std::string>& args) -> VhalHandler::CommandOutput {
        recorded_args.push_back(args);
        return {.exit_code = 0, .stdout_str = "ok\n", .stderr_str = ""};
      });

  EventCollector collector;
  const int sub_id = handler.AddSubscriber(
      [&](const Json::Value& evt) { collector.Push(evt); });

  Json::Value turn_on;
  turn_on["command"] = "vhal_hvac_power";
  turn_on["enabled"] = true;
  handler.HandleMessage(turn_on);

  Json::Value turn_off;
  turn_off["command"] = "vhal_hvac_power";
  turn_off["enabled"] = false;
  handler.HandleMessage(turn_off);

  ASSERT_TRUE(collector.WaitForCount(2));
  const std::vector<Json::Value> events = collector.Events();
  ASSERT_EQ(events.size(), 2);
  EXPECT_EQ(events[0]["event"].asString(), "VHAL_HVAC_POWER_STATE");
  EXPECT_TRUE(events[0]["success"].asBool());
  EXPECT_TRUE(events[0]["enabled"].asBool());

  EXPECT_EQ(events[1]["event"].asString(), "VHAL_HVAC_POWER_STATE");
  EXPECT_TRUE(events[1]["success"].asBool());
  EXPECT_FALSE(events[1]["enabled"].asBool());

  ASSERT_EQ(recorded_args.size(), 2);
  const std::vector<std::string> expected_on = {
      "--port=9301", "--set", "HVAC_POWER_ON", "-a", "1", "-i", "1"};
  const std::vector<std::string> expected_off = {
      "--port=9301", "--set", "HVAC_POWER_ON", "-a", "1", "-i", "0"};
  EXPECT_EQ(recorded_args[0], expected_on);
  EXPECT_EQ(recorded_args[1], expected_off);

  handler.Unsubscribe(sub_id);
}

TEST(VhalHandlerTest, ToggleFailureRevertsToKnownStateAndReportsError) {
  int call_count = 0;
  VhalHandler handler(
      9300, [&](const std::vector<std::string>&) -> VhalHandler::CommandOutput {
        ++call_count;
        if (call_count == 1) {
          return {.exit_code = 0, .stdout_str = "ok", .stderr_str = ""};
        }
        return {.exit_code = 1,
                .stdout_str = "",
                .stderr_str = "Failed to call Dump on VHAL proxy server\n"};
      });

  EventCollector collector;
  handler.AddSubscriber([&](const Json::Value& evt) { collector.Push(evt); });

  Json::Value turn_on;
  turn_on["enabled"] = true;
  handler.HandleMessage(turn_on);

  Json::Value turn_off;
  turn_off["enabled"] = false;
  handler.HandleMessage(turn_off);

  ASSERT_TRUE(collector.WaitForCount(2));
  const std::vector<Json::Value> events = collector.Events();
  ASSERT_EQ(events.size(), 2);
  EXPECT_TRUE(events[0]["success"].asBool());
  EXPECT_TRUE(events[0]["enabled"].asBool());

  EXPECT_FALSE(events[1]["success"].asBool());
  EXPECT_TRUE(events[1]["enabled"].asBool());
  EXPECT_NE(events[1]["error"].asString().find(
                "Failed to call Dump on VHAL proxy server"),
            std::string::npos);

  // A new subscriber receives the last known good state.
  EventCollector late_collector;
  handler.AddSubscriber(
      [&](const Json::Value& evt) { late_collector.Push(evt); });
  ASSERT_TRUE(late_collector.WaitForCount(1));
  const std::vector<Json::Value> late_events = late_collector.Events();
  EXPECT_TRUE(late_events[0]["success"].asBool());
  EXPECT_TRUE(late_events[0]["enabled"].asBool());
}

TEST(VhalHandlerTest, InvalidPayloadReportsErrorWithoutRunningCommand) {
  bool called = false;
  VhalHandler handler(
      9300, [&](const std::vector<std::string>&) -> VhalHandler::CommandOutput {
        called = true;
        return {.exit_code = 0};
      });

  EventCollector collector;
  handler.AddSubscriber([&](const Json::Value& evt) { collector.Push(evt); });

  Json::Value bad_msg;
  bad_msg["command"] = "vhal_hvac_power";
  handler.HandleMessage(bad_msg);

  ASSERT_TRUE(collector.WaitForCount(1));
  const std::vector<Json::Value> events = collector.Events();
  EXPECT_FALSE(events[0]["success"].asBool());
  EXPECT_FALSE(events[0]["enabled"].asBool());
  EXPECT_FALSE(events[0]["error"].asString().empty());
  EXPECT_FALSE(called);
}

}  // namespace
}  // namespace cuttlefish
