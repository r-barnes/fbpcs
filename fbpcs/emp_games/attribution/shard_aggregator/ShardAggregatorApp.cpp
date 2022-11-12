/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ShardAggregatorApp.h"

#include <memory>
#include <string>
#include <vector>

#include <emp-sh2pc/emp-sh2pc.h>
#include <folly/Format.h>
#include <folly/json.h>
#include <folly/logging/xlog.h>
#include "folly/Conv.h"

#include <fbpcf/common/FunctionalUtil.h>
#include <fbpcf/io/api/FileIOWrappers.h>
#include <fbpcf/mpc/EmpGame.h>
#include "AggMetrics.h"
#include "ShardAggregatorGame.h"
#include "ShardAggregatorValidation.h"
#include "fbpcs/emp_games/attribution/shard_aggregator/AggMetricsThresholdCheckers.h"

namespace measurement::private_attribution {
using AggMetrics = private_measurement::AggMetrics;
using AggMetricsTag = private_measurement::AggMetricsTag;
using CompressedAdIdToOriginalAdId =
    private_measurement::CompressedAdIdToOriginalAdId;

void ShardAggregatorApp::run() {
  auto inputData = getInputData();

  auto io = std::make_unique<emp::NetIO>(
      party_ == fbpcf::Party::Alice ? nullptr : serverIp_.c_str(),
      port_,
      true /* quiet mode */);

  XLOG(INFO) << "NetIO is connected.";

  std::function<void(std::shared_ptr<AggMetrics>)> thresholdChecker;
  if (metricsFormatType_ == "lift") {
    thresholdChecker = constructLiftThresholdChecker(threshold_);

  } else if (metricsFormatType_ == "ad_object") {
    thresholdChecker = constructAdObjectFormatThresholdChecker(threshold_);

  } else {
    XLOG(FATAL) << "invalid format type " << metricsFormatType_
                << " passed to aggregator";
  }

  if (!inputData.empty()) {
    ShardAggregatorGame game{
        std::move(io), party_, thresholdChecker, visibility_};
    auto encryptedResult = game.perfPlay(inputData);

    auto result = revealMetrics(encryptedResult);
    if (useNewOutputFormat_) {
      auto compressedAdIdMapping = getCompressedMapping();
      auto newResult =
          replaceCompressedAdIdWithAdId(compressedAdIdMapping, result);
      putOutputData(newResult);
    } else {
      putOutputData(result);
    }
  } else {
    XLOG(WARN) << "inputData is empty().";
    putOutputData(nullptr);
  }
};

std::vector<std::string> ShardAggregatorApp::getInputPaths(
    const std::string& inputPath,
    const int firstShardIndex,
    const int numShards) {
  std::vector<std::string> v;

  for (int i = firstShardIndex; i < firstShardIndex + numShards; i++) {
    v.push_back(folly::sformat("{}_{}", inputPath, i));
  }

  return v;
}

std::vector<std::shared_ptr<AggMetrics>> ShardAggregatorApp::getInputData() {
  XLOG(INFO) << "getting input data ...";
  auto inputPaths = ShardAggregatorApp::getInputPaths(
      inputPath_, firstShardIndex_, numShards_);

  auto inputData =
      fbpcf::functional::map<std::string, std::shared_ptr<AggMetrics>>(
          inputPaths, [](const auto& inputPath) -> std::shared_ptr<AggMetrics> {
            XLOG(INFO) << "Opening file at <" << inputPath << ">";
            auto contents = fbpcf::io::FileIOWrappers::readFile(inputPath);
            if (contents.empty()) {
              XLOG(WARN) << "Empty file: <" << inputPath << ">";
              return nullptr;
            }
            return std::make_shared<AggMetrics>(
                AggMetrics::fromDynamic(folly::parseJson(std::move(contents))));
          });
  std::remove_if(
      inputData.begin(), inputData.end(), [](auto& x) { return x == nullptr; });

  validateInputDataAggMetrics(inputData, metricsFormatType_);
  return inputData;
}

CompressedAdIdToOriginalAdId ShardAggregatorApp::getCompressedMapping() {
  XLOG(INFO) << "getting compressed Ad Id mapping ...";
  auto contents = fbpcf::io::FileIOWrappers::readFile(inputMappingPath_);
  CompressedAdIdToOriginalAdId inputMapping =
      CompressedAdIdToOriginalAdId::fromDynamic(folly::parseJson(contents));
  return inputMapping;
}
void ShardAggregatorApp::putOutputData(
    const std::shared_ptr<AggMetrics>& metrics) {
  XLOG(INFO) << "putting out data ...";

  auto json = (metrics != nullptr) ? folly::toJson(metrics->toDynamic()) : "";
  fbpcf::io::FileIOWrappers::writeFile(outputPath_, std::move(json));
}

std::shared_ptr<AggMetrics> ShardAggregatorApp::revealMetrics(
    const std::shared_ptr<AggMetrics>& metrics) {
  switch (metrics->getTag()) {
    case AggMetricsTag::Map: {
      auto revealedMetrics = std::make_shared<AggMetrics>(AggMetricsTag::Map);
      for (const auto& [key, value] : metrics->getAsMap()) {
        revealedMetrics->emplace(key, revealMetrics(value));
      }
      return revealedMetrics;
    }
    case AggMetricsTag::List: {
      auto revealedMetrics = std::make_shared<AggMetrics>(AggMetricsTag::List);
      for (const auto& m : metrics->getAsList()) {
        revealedMetrics->pushBack(revealMetrics(m));
      }
      return revealedMetrics;
    }
    case AggMetricsTag::EmpInteger: {
      return std::make_shared<AggMetrics>(
          AggMetrics{metrics->getEmpIntValue().reveal<int64_t>(
              static_cast<int32_t>(visibility_))});
    }
    default: {
      XLOG(FATAL)
          << "AggMetrics should only store a map, list, or emp::Integer at this point";
    }
  }
}
std::shared_ptr<AggMetrics> ShardAggregatorApp::replaceCompressedAdIdWithAdId(
    const CompressedAdIdToOriginalAdId& compressedAdIdMapping,
    std::shared_ptr<AggMetrics> result) {
  auto map = compressedAdIdMapping.compressedAdIdToAdIdMap;

  auto originalAdIdResult = std::make_shared<AggMetrics>(AggMetricsTag::Map);

  for (auto& [rule, resultMap] : result->getAsMap()) {
    auto compressedResultMap = std::make_shared<AggMetrics>(AggMetricsTag::Map);
    originalAdIdResult->emplace(rule, compressedResultMap);

    for (auto& [aggregationName, aggregationData] : resultMap->getAsMap()) {
      auto compressedAggregationData =
          std::make_shared<AggMetrics>(AggMetricsTag::Map);
      compressedResultMap->emplace(aggregationName, compressedAggregationData);
      for (auto [id, metrics] : aggregationData->getAsMap()) {
        if (map.find(id) != map.end()) {
          auto new_id = std::to_string(
              compressedAdIdMapping.compressedAdIdToAdIdMap.at(id));
          compressedAggregationData->emplace(new_id, metrics);
        }
      }
    }
  }
  return originalAdIdResult;
}
} // namespace measurement::private_attribution
