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

#include "default_off.hpp"
#include <cmath>
#include "../strategy.hpp"

#include "../../../../../main.hpp"

// ============================================================
// DefaultOffenseStrategy::RequestInput —— 前锋无球跑位策略
//
// 流程：
//   1. 混合"静态阵型位置"与"动态角色位置"作为基准
//      （靠近控球者时更倾向动态，可以换位）
//   2. 进攻组件：用力场法计算支援跑位，控球时进一步前插
//      （attackBias上限0.6，比中场/后卫更激进）
//   3. 防守组件：若己方失球，加入向本方半场回退的防守分量
//      （前锋的防守阈值最低：1.3 - mindset，几乎不回防）
//   4. 懒惰修正：根据距离和控球情况降低速度
// ============================================================
void DefaultOffenseStrategy::RequestInput(ElizaController *controller,
                                          const MentalImage *mentalImage,
                                          Vector3 &direction, float &velocity) {
  DO_VALIDATION;

  bool offensiveComponents = true;
  bool defensiveComponents = true;
  bool laziness = true;

  // 静态阵型位置：球员在阵型表中的固定位置（不随队友换位）
  Vector3 desiredPosition_static = controller->GetTeam()->GetController()->GetAdaptedFormationPosition(static_cast<Player*>(controller->GetPlayer()), false);
  // 动态阵型位置：通过匈牙利算法动态分配，允许球员换占对方位置
  Vector3 desiredPosition_dynamic = controller->GetTeam()->GetController()->GetAdaptedFormationPosition(static_cast<Player*>(controller->GetPlayer()), true);
  // 与控球者的归一化距离 [15m, 20m] → [0, 1]
  float actionDistance = NormalizedClamp(controller->GetPlayer()->GetPosition().GetDistance(controller->GetMatch()->GetDesignatedPossessionPlayer()->GetPosition()), 15.0f, 20.0f);
  // staticPositionBias越高 → 更倾向固定位置（远离控球者时）
  // 前锋系数0.8：比后卫(1.0)更愿意换位（攻击性更强）
  float staticPositionBias = curve(0.8f * actionDistance, 1.0f);
  Vector3 desiredPosition = desiredPosition_static * staticPositionBias + desiredPosition_dynamic * (1.0f - staticPositionBias);

  if (offensiveComponents) {
    DO_VALIDATION;
    // attackBias：己方控球程度映射到进攻偏置，前锋上限0.6（中等激进）
    float attackBias = NormalizedClamp((controller->GetFadingTeamPossessionAmount() - 0.5f) * 1.0f, 0.1f, 0.6f);
    bool makeRun = false;
    // 当攻势足够强（attackBias>0.7，此处前锋不触发，故前锋不会主动前插跑，
    // 由中场触发。前锋的激进跑位由力场法内部处理）
    if (attackBias > 0.7f) {
      DO_VALIDATION;
      if (controller->GetTeam()->GetController()->GetEndApplyAttackingRun_ms() >
              controller->GetMatch()->GetActualTime_ms() &&
          controller->GetTeam()->GetController()->GetAttackingRunPlayer() ==
              controller->GetPlayer()) {
        DO_VALIDATION;
        makeRun = true; // 被队伍AI指定为前插球员
      }
    }
    // 用力场法计算最优支援位置（综合考虑空间、对手位置等）
    Vector3 supportPosition = controller->GetSupportPosition_ForceField(mentalImage, desiredPosition, makeRun);
    // 混合：attackBias越高，越靠近力场支援位置（进攻性更强）
    desiredPosition = desiredPosition * (1.0f - attackBias) + supportPosition * attackBias;
  }

  if (defensiveComponents) {
    DO_VALIDATION;
    float mindset = AI_GetMindSet(static_cast<Player*>(controller->GetPlayer())->GetDynamicFormationEntry().role);
    // 防守分量强度 = pow(max(1.3 - mindset - 控球量, 0), 0.7)
    // 前锋mindset≈1：1.3-1=0.3，失球时才有少量防守分量
    // 控球量>1.3时防守分量为0（不需要回防）
    controller->AddDefensiveComponent(
        desiredPosition,
        std::pow(
            clamp(1.3f - mindset - controller->GetFadingTeamPossessionAmount(),
                  0.0f, 1.0f),
            0.7f));
  }

  // 方向 = 朝向目标位置
  direction = (desiredPosition - controller->GetPlayer()->GetPosition()).GetNormalized(controller->GetPlayer()->GetDirectionVec());
  // 速度 = 距离目标位置的距离 × 速度倍率
  float desiredVelocity = (desiredPosition - controller->GetPlayer()->GetPosition()).GetLength() * distanceToVelocityMultiplier;

  // 懒惰修正：远离球/控球充裕时节省体力
  if (laziness) desiredVelocity = controller->GetLazyVelocity(desiredVelocity);

  desiredVelocity = clamp(desiredVelocity, 0, sprintVelocity);

  velocity = desiredVelocity;
}
