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

#include "default_mid.hpp"
#include <cmath>
#include "../strategy.hpp"

#include "../../../../../main.hpp"

// ============================================================
// DefaultMidfieldStrategy::RequestInput —— 中场无球跑位策略
//
// 与后卫/前锋的对比：
//   1. staticPositionBias系数0.9：介于后卫(1.0)和前锋(0.8)之间，
//      换位意愿适中
//   2. attackBias范围[0.1, 0.7]：比前锋更保守但比后卫更激进
//   3. makeRun触发阈值0.9（比前锋的0.7更高）：中场前插更谨慎，
//      需要控球非常优势时才跑
//   4. 防守阈值1.5：介于后卫和前锋之间，失球时适量回防
//   5. 同样执行越位陷阱
// ============================================================
void DefaultMidfieldStrategy::RequestInput(ElizaController *controller,
                                           const MentalImage *mentalImage,
                                           Vector3 &direction,
                                           float &velocity) {
  DO_VALIDATION;

  bool offensiveComponents = true;
  bool defensiveComponents = true;
  bool laziness = true;

  Vector3 desiredPosition_static = controller->GetTeam()->GetController()->GetAdaptedFormationPosition(static_cast<Player*>(controller->GetPlayer()), false);
  Vector3 desiredPosition_dynamic = controller->GetTeam()->GetController()->GetAdaptedFormationPosition(static_cast<Player*>(controller->GetPlayer()), true);
  float actionDistance = NormalizedClamp(controller->GetPlayer()->GetPosition().GetDistance(controller->GetMatch()->GetDesignatedPossessionPlayer()->GetPosition()), 15.0f, 20.0f);
  // 系数0.9：中场在靠近控球者时会适度换位，保持一定灵活性
  float staticPositionBias = curve(0.9f * actionDistance, 1.0f);
  Vector3 desiredPosition = desiredPosition_static * staticPositionBias + desiredPosition_dynamic * (1.0f - staticPositionBias);

  if (offensiveComponents) {
    DO_VALIDATION;
    // 进攻偏置上限0.7（前锋0.6，后卫0.9），中场最全面
    float attackBias = NormalizedClamp((controller->GetFadingTeamPossessionAmount() - 0.5f) * 1.0f, 0.1f, 0.7f);
    bool makeRun = false;
    // 中场前插跑阈值更高(0.9)：需要绝对控球优势才会前插
    if (attackBias > 0.9f) {
      DO_VALIDATION;
      if (controller->GetTeam()->GetController()->GetEndApplyAttackingRun_ms() >
              controller->GetMatch()->GetActualTime_ms() &&
          controller->GetTeam()->GetController()->GetAttackingRunPlayer() ==
              controller->GetPlayer()) {
        DO_VALIDATION;
        makeRun = true;
      }
    }
    Vector3 supportPosition = controller->GetSupportPosition_ForceField(mentalImage, desiredPosition, makeRun);
    desiredPosition = desiredPosition * (1.0f - attackBias) + supportPosition * attackBias;
  }

  if (defensiveComponents) {
    DO_VALIDATION;

    float mindset = AI_GetMindSet(static_cast<Player*>(controller->GetPlayer())->GetDynamicFormationEntry().role);
    // 防守分量：1.5 - mindset - 控球量，中场mindset≈0.5
    // 失球时有中等程度的回防
    controller->AddDefensiveComponent(
        desiredPosition,
        std::pow(
            clamp(1.5f - mindset - controller->GetFadingTeamPossessionAmount(),
                  0.0f, 1.0f),
            0.7f));

    // 中场也执行越位陷阱（配合后卫整体压线）
    controller->GetTeam()->GetController()->ApplyOffsideTrap(desiredPosition);
  }

  direction = (desiredPosition - controller->GetPlayer()->GetPosition()).GetNormalized(controller->GetPlayer()->GetDirectionVec());
  float desiredVelocity = (desiredPosition - controller->GetPlayer()->GetPosition()).GetLength() * distanceToVelocityMultiplier;

  // 懒惰修正
  if (laziness) desiredVelocity = controller->GetLazyVelocity(desiredVelocity);

  desiredVelocity = clamp(desiredVelocity, 0, sprintVelocity);

  velocity = desiredVelocity;
}
