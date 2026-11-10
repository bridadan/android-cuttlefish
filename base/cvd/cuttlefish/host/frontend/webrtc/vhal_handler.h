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
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "json/value.h"

namespace cuttlefish {

// Executes host-side vhal_proxy_server_cmd commands for WebRTC control panel
// requests and broadcasts the resulting VHAL property state to subscribers.
class VhalHandler {
 public:
  struct CommandOutput {
    int exit_code = -1;
    std::string stdout_str;
    std::string stderr_str;
  };

  using CommandRunner =
      std::function<CommandOutput(const std::vector<std::string>& args)>;

  explicit VhalHandler(int vhal_proxy_server_port);
  VhalHandler(int vhal_proxy_server_port, CommandRunner command_runner);
  ~VhalHandler();

  VhalHandler(const VhalHandler&) = delete;
  VhalHandler& operator=(const VhalHandler&) = delete;

  void HandleMessage(const Json::Value& msg);

  int AddSubscriber(std::function<void(const Json::Value&)> subscriber);
  void Unsubscribe(int subscriber_id);

 private:
  void WorkerLoop();
  void DeliverEvent(const Json::Value& event);

  const int port_;
  const CommandRunner command_runner_;

  std::mutex queue_mtx_;
  std::condition_variable queue_cv_;
  std::deque<bool> pending_hvac_requests_;
  bool stopping_ = false;

  std::mutex subscribers_mtx_;
  std::map<int, std::function<void(const Json::Value&)>> subscribers_;
  int last_subscriber_id_ = 0;
  std::optional<bool> hvac_power_on_;

  std::thread worker_thread_;
};

}  // namespace cuttlefish
