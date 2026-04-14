// Copyright 2019 Google LLC & Bastiaan Konings
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// written by bastiaan konings schuiling 2008 - 2015
// this work is public domain. the code is undocumented, scruffy, untested, and should generally not be used for anything important.
// i do not offer support, so don't ask. to be used for inspiration :)

#include "mentalimage.hpp"

#include "../../main.hpp"
#include "../match.hpp"

// ============================================================
// MentalImage —— AI球员的延迟感知快照
//
// 核心设计：AI球员不能即时感知世界，而是基于一个"心理快照"
// 做决策，模拟人类的视觉感知延迟（约100ms）。
// 快照包含：
//   - 所有球员在快照时刻的位置/速度/朝向
//   - 球的运动轨迹预测数组
//
// GetPlayerImage会在读取时对位置做外推（基于时间差），
// 同时限制外推量（maxDistanceDeviation），防止感知偏差过大。
// ============================================================

// 构造函数：快照当前帧所有球员状态和球的预测轨迹
MentalImage::MentalImage(Match* match)
    : timeStamp_ms(match->GetActualTime_ms()), match(match) {
  DO_VALIDATION;
  timeStamp_ms = match->GetActualTime_ms();
  std::vector<Player*> allPlayers;
  match->GetTeam(match->FirstTeam())->GetActivePlayers(allPlayers);
  match->GetTeam(match->SecondTeam())->GetActivePlayers(allPlayers);
  players.resize(allPlayers.size());

  for (int playerCounter = 0; playerCounter < (signed int)allPlayers.size();
       playerCounter++) {
    DO_VALIDATION;

    Player *player = allPlayers.at(playerCounter);

    // 记录每个球员的快照状态（位置、方向、速度、角色）
    PlayerImage& playerImage = players[playerCounter];
    playerImage.player = player;
    playerImage.position = player->GetPosition();
    playerImage.directionVec = player->GetDirectionVec();
    playerImage.velocity = player->GetEnumVelocity();
    playerImage.movement = player->GetMovement();
    playerImage.role = player->GetDynamicFormationEntry().role;
  }

  // 记录球的预测轨迹（物理引擎预测的未来球位置数组）
  UpdateBallPredictions();
}

void MentalImage::Mirror(bool team_0, bool team_1, bool ball) {
  for (auto& i : players) {
    if (i.player->GetTeamID() == 0 ? team_0 : team_1) {
      i.Mirror();
    }
  }
  if (ball) {
    ballPredictions_mirrored = !ballPredictions_mirrored;
    for (auto& i : ballPredictions) {
      i.Mirror();
    }
  }
}

int MentalImage::GetTimeStampNeg_ms() const { return match->GetActualTime_ms() - timeStamp_ms; }

void MentalImage::ProcessState(EnvState* state, Match* match) {
  this->match = match;
  state->process(timeStamp_ms);
  state->process(maxDistanceDeviation);
  state->process(maxMovementDeviation);
  int size = players.size();
  state->process(size);
  players.resize(size);
  for (auto& p : players) {
    p.ProcessState(state);
  }
  size = ballPredictions.size();
  state->process(size);
  ballPredictions.resize(size);
  for (auto& b : ballPredictions) {
    if (state->getConfig()->reverse_team_processing &&
        !ballPredictions_mirrored) {
      b.Mirror();
    }
    state->process(b);
    if (state->getConfig()->reverse_team_processing &&
        !ballPredictions_mirrored) {
      b.Mirror();
    }
  }
}

// 获取某球员的心理快照图像（带外推和偏差限制）
PlayerImage MentalImage::GetPlayerImage(PlayerBase* p) const {
  for (auto& player : players) {
    DO_VALIDATION;
    if (player.player == p) {
      DO_VALIDATION;
      PlayerImage newImage = player;
      // 外推：基于快照时的速度，推算到当前时刻的位置
      // GetTimeStampNeg_ms() = 当前时间 - 快照时间 = 已经过的时间
      Vector3 extrapolation = player.movement * GetTimeStampNeg_ms() * 0.001f;
      newImage.position = player.position + extrapolation;
      // 限制外推偏差（防止感知位置与实际位置差距过大）
      newImage.position = newImage.position.EnforceMaximumDeviation(newImage.player->GetPosition(), maxDistanceDeviation);
      newImage.movement = newImage.movement.EnforceMaximumDeviation(newImage.player->GetMovement(), maxMovementDeviation);
      return newImage;
    }
  }

  // 找不到时返回第一个（安全回退）
  return players[0];
}

// 获取某队所有球员的心理快照（返回位置+速度+角色的轻量结构体）
std::vector<PlayerImagePosition> MentalImage::GetTeamPlayerImages(int teamID) const {
  std::vector<PlayerImagePosition> result;
  result.reserve(11);
  for (auto& player : players) {
    DO_VALIDATION;
    if (player.player->IsActive() && player.player->GetTeamID() == teamID) {
      DO_VALIDATION;
      // 同样做外推和偏差限制
      Vector3 extrapolation = player.movement * GetTimeStampNeg_ms() * 0.001f;
      Vector3 position = player.position + extrapolation;
      position = position.EnforceMaximumDeviation(player.player->GetPosition(), maxDistanceDeviation);
      Vector3 movement = player.movement.EnforceMaximumDeviation(player.player->GetMovement(), maxMovementDeviation);
      result.emplace_back(position, movement, player.role);
    }
  }
  return result;
}

// 从物理引擎更新球的预测轨迹数组
void MentalImage::UpdateBallPredictions() {
  DO_VALIDATION;
  match->GetBall()->GetPredictionArray(ballPredictions);
}

// 获取快照中 time_ms 时刻的球位置
// 关键设计：返回"心理预测"和"真实预测"的折中值
// 原因：心理图像对于突发球权变化会有滞后（如被截后球的方向变了），
//       但某些计算（如timeneededtogettoball）已经用了非延迟的实时量，
//       若完全使用心理预测会造成内部不一致。
//       折中方案：心理预测不能与实时预测偏差超过maxDistanceDeviation。
Vector3 MentalImage::GetBallPrediction(int time_ms) const {

  // 计算在心理快照内对应的时间索引（每10ms一个样本）
  int index = time_ms + match->GetActualTime_ms() - timeStamp_ms;
  if (index >= ballPredictionSize_ms) index = ballPredictionSize_ms - 10;
  index = index / 10;
  if (index < 0) index = 0;

  Vector3 mentalResult = ballPredictions[index];   // 快照时记录的轨迹
  Vector3 realResult = match->GetBall()->Predict(time_ms); // 实时物理预测

  // 心理结果与实时结果的折中（限制最大偏差）
  Vector3 result = mentalResult.EnforceMaximumDeviation(realResult, maxDistanceDeviation);

  return result;
}
