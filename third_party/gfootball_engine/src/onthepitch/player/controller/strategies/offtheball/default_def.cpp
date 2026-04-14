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

#include "../strategy.hpp"
#include "default_def.hpp"
#include <cmath>

// ============================================================
// DefaultDefenseStrategy::RequestInput —— 后卫无球跑位策略
//
// 与前锋/中场的核心区别：
//   1. staticPositionBias系数1.0（最高）：后卫最不愿意换位，
//      严格保持阵型位置，防止被对手拉出空档
//   2. attackBias范围[0.2, 0.9]：失球时还是有一定力场计算（0.2）
//      确保不完全忽略支援跑位
//   3. 防守阈值1.9（最高）：失球时后卫有强烈的回防分量
//   4. ApplyOffsideTrap：后卫是越位陷阱的执行者
// ============================================================
void DefaultDefenseStrategy::RequestInput(ElizaController *controller,
                                          const MentalImage *mentalImage,
                                          Vector3 &direction, float &velocity) {
  DO_VALIDATION;

  bool offensiveComponents = true;
  bool defensiveComponents = true;
  bool laziness = true;

  // 静态阵型位置（后卫强调保持固定位置）
  Vector3 desiredPosition_static = controller->GetTeam()->GetController()->GetAdaptedFormationPosition(static_cast<Player*>(controller->GetPlayer()), false);
  // 动态角色位置（匈牙利算法分配）
  Vector3 desiredPosition_dynamic = controller->GetTeam()->GetController()->GetAdaptedFormationPosition(static_cast<Player*>(controller->GetPlayer()), true);
  float actionDistance = NormalizedClamp(controller->GetPlayer()->GetPosition().GetDistance(controller->GetMatch()->GetDesignatedPossessionPlayer()->GetPosition()), 15.0f, 20.0f);
  // 系数1.0：后卫最保守，远离控球者时几乎完全使用静态位置
  float staticPositionBias = curve(1.0f * actionDistance, 1.0f);
  Vector3 desiredPosition = desiredPosition_static * staticPositionBias + desiredPosition_dynamic * (1.0f - staticPositionBias);

  if (offensiveComponents) {
    DO_VALIDATION;
    // attackBias范围[0.2, 0.9]：下限0.2保证即使失球也有部分力场支援计算
    float attackBias = NormalizedClamp((controller->GetFadingTeamPossessionAmount() - 0.5f) * 1.0f, 0.2f, 0.9f);
    // 后卫不会触发makeRun（没有传makeRun参数），力场计算较保守
    Vector3 supportPosition = controller->GetSupportPosition_ForceField(mentalImage, desiredPosition);
    desiredPosition = desiredPosition * (1.0f - attackBias) + supportPosition * attackBias;
  }

  if (defensiveComponents) {
    DO_VALIDATION;

    float mindset = AI_GetMindSet(static_cast<Player*>(controller->GetPlayer())->GetDynamicFormationEntry().role);
    // 防守分量强度 = pow(max(1.9 - mindset - 控球量, 0), 0.7)
    // 后卫mindset≈0：1.9-0=1.9，失球时有强烈的回防冲动
    // 控球量>1.9时防守分量才为0
    controller->AddDefensiveComponent(
        desiredPosition,
        std::pow(
            clamp(1.9f - mindset - controller->GetFadingTeamPossessionAmount(),
                  0.0f, 1.0f),
            0.7f));

    // 越位陷阱：将后卫的目标位置压缩到越位线附近，
    // 形成整体防线（避免某后卫单独站太深而撑开防线）
    controller->GetTeam()->GetController()->ApplyOffsideTrap(desiredPosition);
  }

  direction = (desiredPosition - controller->GetPlayer()->GetPosition()).GetNormalized(controller->GetPlayer()->GetDirectionVec());
  float desiredVelocity = (desiredPosition - controller->GetPlayer()->GetPosition()).GetLength() * distanceToVelocityMultiplier;

  // 懒惰修正
  if (laziness) desiredVelocity = controller->GetLazyVelocity(desiredVelocity);

  desiredVelocity = clamp(desiredVelocity, 0, sprintVelocity);

  velocity = desiredVelocity;
}
