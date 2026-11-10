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

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/log.h"
#include "absl/strings/ascii.h"
#include "absl/strings/string_view.h"
#include "fmt/format.h"
#include "json/value.h"

#include "cuttlefish/common/libs/utils/json.h"
#include "cuttlefish/host/libs/config/known_paths.h"
#include "cuttlefish/process/command.h"
#include "cuttlefish/process/managed_stdio.h"
#include "cuttlefish/result/result.h"

namespace cuttlefish {
namespace {

constexpr absl::string_view kHvacPowerEvent = "VHAL_HVAC_POWER_STATE";

VhalHandler::CommandOutput RunVhalProxyServerCmd(
    const std::vector<std::string>& args) {
  Command cmd(VhalProxyServerCmdBinary());
  for (const std::string& arg : args) {
    cmd.AddParameter(arg);
  }
  std::string stdout_str;
  std::string stderr_str;
  const int exit_code =
      RunWithManagedStdio(std::move(cmd), nullptr, &stdout_str, &stderr_str);
  return VhalHandler::CommandOutput{
      .exit_code = exit_code,
      .stdout_str = std::move(stdout_str),
      .stderr_str = std::move(stderr_str),
  };
}

Json::Value MakeHvacStateEvent(
    bool enabled, bool success,
    std::optional<std::string> error = std::nullopt) {
  Json::Value event;
  event["event"] = std::string(kHvacPowerEvent);
  event["enabled"] = enabled;
  event["success"] = success;
  if (error.has_value()) {
    event["error"] = *error;
  }
  return event;
}

std::string FormatCommandError(const VhalHandler::CommandOutput& output) {
  const absl::string_view trimmed_err =
      absl::StripAsciiWhitespace(output.stderr_str);
  const absl::string_view trimmed_out =
      absl::StripAsciiWhitespace(output.stdout_str);
  const absl::string_view details =
      !trimmed_err.empty() ? trimmed_err : trimmed_out;
  if (details.empty()) {
    return fmt::format("vhal_proxy_server_cmd failed with exit code {}",
                       output.exit_code);
  }
  return fmt::format("vhal_proxy_server_cmd failed (exit code {}): {}",
                     output.exit_code, details);
}

}  // namespace

VhalHandler::VhalHandler(int vhal_proxy_server_port)
    : VhalHandler(vhal_proxy_server_port, RunVhalProxyServerCmd) {}

VhalHandler::VhalHandler(int vhal_proxy_server_port,
                         CommandRunner command_runner)
    : port_(vhal_proxy_server_port),
      command_runner_(std::move(command_runner)),
      worker_thread_([this]() { WorkerLoop(); }) {}

VhalHandler::~VhalHandler() {
  {
    std::lock_guard<std::mutex> lock(queue_mtx_);
    stopping_ = true;
  }
  queue_cv_.notify_all();
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
}

void VhalHandler::HandleMessage(const Json::Value& msg) {
  const Result<bool> enabled_res = GetValue<bool>(msg, {"enabled"});
  if (!enabled_res.has_value()) {
    LOG(ERROR) << "Invalid VHAL HVAC command: " << enabled_res.error();
    bool current_state = false;
    {
      std::lock_guard<std::mutex> lock(subscribers_mtx_);
      current_state = hvac_power_on_.value_or(false);
    }
    DeliverEvent(
        MakeHvacStateEvent(current_state, /*success=*/false,
                           fmt::format("Invalid VHAL HVAC command: {}",
                                       enabled_res.error().Message())));
    return;
  }

  {
    std::lock_guard<std::mutex> lock(queue_mtx_);
    pending_hvac_requests_.push_back(*enabled_res);
  }
  queue_cv_.notify_one();
}

int VhalHandler::AddSubscriber(
    std::function<void(const Json::Value&)> subscriber) {
  std::lock_guard<std::mutex> lock(subscribers_mtx_);
  if (hvac_power_on_.has_value()) {
    subscriber(MakeHvacStateEvent(*hvac_power_on_, /*success=*/true));
  }
  subscribers_[++last_subscriber_id_] = std::move(subscriber);
  return last_subscriber_id_;
}

void VhalHandler::Unsubscribe(int subscriber_id) {
  std::lock_guard<std::mutex> lock(subscribers_mtx_);
  subscribers_.erase(subscriber_id);
}

void VhalHandler::WorkerLoop() {
  while (true) {
    bool target_enabled = false;
    {
      std::unique_lock<std::mutex> lock(queue_mtx_);
      queue_cv_.wait(lock, [this]() {
        return stopping_ || !pending_hvac_requests_.empty();
      });
      if (stopping_ && pending_hvac_requests_.empty()) {
        break;
      }
      target_enabled = pending_hvac_requests_.front();
      pending_hvac_requests_.pop_front();
    }

    const std::vector<std::string> args = {
        fmt::format("--port={}", port_),
        "--set",
        "HVAC_POWER_ON",
        "-a",
        "1",
        "-i",
        target_enabled ? "1" : "0",
    };

    const CommandOutput output = command_runner_(args);
    if (output.exit_code == 0) {
      VLOG(1) << "vhal_proxy_server_cmd succeeded: " << output.stdout_str;
      {
        std::lock_guard<std::mutex> lock(subscribers_mtx_);
        hvac_power_on_ = target_enabled;
      }
      DeliverEvent(MakeHvacStateEvent(target_enabled, /*success=*/true));
    } else {
      const std::string error = FormatCommandError(output);
      LOG(ERROR) << error;
      bool current_state = false;
      {
        std::lock_guard<std::mutex> lock(subscribers_mtx_);
        current_state = hvac_power_on_.value_or(false);
      }
      DeliverEvent(MakeHvacStateEvent(current_state, /*success=*/false, error));
    }
  }
}

void VhalHandler::DeliverEvent(const Json::Value& event) {
  std::lock_guard<std::mutex> lock(subscribers_mtx_);
  for (const auto& entry : subscribers_) {
    entry.second(event);
  }
}

}  // namespace cuttlefish
