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

#include "goalie_default.hpp"

#include "../../../../../base/geometry/triangle.hpp"

#include "../../../../../main.hpp"
#include "../strategy.hpp"

// ============================================================
// GoalieDefaultStrategy::RequestInput —— 门将跑位策略
//
// 两种主要状态：
//   A. 球没有直奔球门（正常战术站位）：
//      - 找从球到球门两柱之间角平分线
//      - 在该线上找最优站位（减小射门角度）
//      - 失球且队友来不及拦截时：主动出击（来球减小空间）
//      - 出击程度还受第二威胁对手影响（避免传给无人盯防的队友）
//   B. 球飞向球门（CalculateIfBallIsBoundForGoal返回true）：
//      - 预判球过门线的Y坐标
//      - 跑到球的飞行轨迹上拦截
//      - 若球到达时高于2.5米，改为回撤到门线处扑救
// ============================================================
void GoalieDefaultStrategy::RequestInput(ElizaController *controller,
                                         const MentalImage *mentalImage,
                                         Vector3 &direction, float &velocity) {
  DO_VALIDATION;

  // 门将默认站在门线前方10米处
  float lineDistance = 10.0f;
  // 球的预测位置（考虑到球员到球需要的时间）
  Vector3 ballPos = mentalImage->GetBallPrediction(600 + static_cast<Player*>(controller->GetPlayer())->GetTimeNeededToGetToBall_ms() * 0.2f).Get2D();
  Vector3 targetPos = Vector3(
      (pitchHalfW - lineDistance) * controller->GetTeam()->GetDynamicSide(), 0,
      0);
  Vector3 goalPos =
      Vector3(pitchHalfW * controller->GetTeam()->GetDynamicSide(), 0, 0);

  float maxVelocity = sprintVelocity;

  // 仅当球在本方半场时才做战术计算（优化性能）
  if (ballPos.coords[0] * controller->GetTeam()->GetDynamicSide() > 0) {
    DO_VALIDATION;

    CalculateIfBallIsBoundForGoal(controller, mentalImage);

    if (!IsBallBoundForGoal()) {
      // ---- 状态A：战术站位（减小射门角度）----

      maxVelocity = sprintVelocity;

      // 从球到左右门柱各画一条线，取角平分线方向
      // 沿此方向站位 = 门将平分射门角度（视觉上目标最小）
      Vector3 toPost1 =
          Vector3(pitchHalfW * controller->GetTeam()->GetDynamicSide(), 3.7f,
                  0) -
          ballPos;
      Vector3 toPost2 =
          Vector3(pitchHalfW * controller->GetTeam()->GetDynamicSide(), -3.7f,
                  0) -
          ballPos;
      radian angle = toPost2.GetAngle2D(toPost1);
      Vector3 middle = toPost1.GetRotated2D(angle * 0.5f)
                           .GetNormalized(Vector3(
                               controller->GetTeam()->GetDynamicSide(), 0, 0));
      Line ballToGoal;
      ballToGoal.SetVertex(0, ballPos);
      ballToGoal.SetVertex(1, ballPos + middle);

      // 将射门角平分线延伸到底线（留0.7m安全距离）
      Line backLine;
      backLine.SetVertex(0, Vector3((pitchHalfW - 0.7f) *
                                        controller->GetTeam()->GetDynamicSide(),
                                    -pitchHalfH, 0));
      backLine.SetVertex(1, Vector3((pitchHalfW - 0.7f) *
                                        controller->GetTeam()->GetDynamicSide(),
                                    pitchHalfH, 0));
      Vector3 intersect = ballToGoal.GetIntersectionPoint(backLine).Get2D();
      intersect.coords[1] = clamp(intersect.coords[1], -3.7f, 3.7f); // 不超出球门宽度
      ballToGoal.SetVertex(1, intersect);

      float awayFromGoalOffset_m = 0.7f; // 距底线的最小距离（沿角平分线方向）
      float awayFromGoalBias = 0.3f;     // 出击偏置：0=贴门线，1=出击到球
      // 己方控球时适当离开门线（控球时可以大胆一点）
      awayFromGoalBias *= NormalizedClamp(controller->GetFadingTeamPossessionAmount(), 1.0f, 1.5f);

      // ---- 出击逻辑：对方有人突入，且己方无人封堵 ----
      if (controller->GetFadingTeamPossessionAmount() < 1.0f) {
        DO_VALIDATION;

        Player *opp = controller->GetOppTeam()->GetDesignatedTeamPossessionPlayer();
        Vector3 oppPos = opp->GetPosition() + opp->GetMovement() * 0.32f;

        // 若对手还没控球，参考对手位置+球位置（混合）
        if (opp->HasPossession() == false) {
          DO_VALIDATION;
          ballToGoal.SetVertex(0, oppPos * 0.6f + ballPos * 0.4f);
        } else {
          ballToGoal.SetVertex(0, oppPos * 0.4f + ballPos * 0.6f);
        }

        // 计算对手到射门阈值点的距离（动态射门阈值随距离变化）
        float shootThreshold = 20.0f;
        float oppToGoalDistance = (goalPos - oppPos).GetLength();
        float oppToThresholdDistance = clamp(oppToGoalDistance - shootThreshold * NormalizedClamp(oppToGoalDistance, 0.0f, shootThreshold * 2.0f), 0.0f, pitchHalfW);
        Vector3 shootingPoint = oppPos + (goalPos - oppPos).GetNormalized(0) * oppToThresholdDistance;

        // 计算己方最近队友到射门点的距离
        Player *mate = AI_GetClosestPlayer(controller->GetTeam(), shootingPoint, false, static_cast<Player*>(controller->GetPlayer()));
        float mateToThresholdDistance = 99999;
        if (mate) {
          DO_VALIDATION;
          Vector3 matePos = mate->GetPosition() + mate->GetMovement() * 0.24f;
          mateToThresholdDistance = (shootingPoint - matePos).GetLength();
        }

        // 队友比对手离射门点远1米以上 → 门将需要出击
        if (mateToThresholdDistance > oppToThresholdDistance + 1.0f) {
          DO_VALIDATION;

          awayFromGoalBias = 1.0f; // 初始设为全出击

          // 但要考虑第二威胁：若对手有接应队友也在危险位置，不能太冒进
          Player *oppHelper = AI_GetClosestPlayer(controller->GetOppTeam(), goalPos, false, opp);
          if (oppHelper) {
            DO_VALIDATION;

            Vector3 oppHelperPosition = oppHelper->GetPosition() + oppHelper->GetMovement() * 0.32f;

            float helperShootThreshold = 24.0f;
            float oppHelperToGoalDistance = (goalPos - oppHelperPosition).GetLength();
            float oppHelperToThresholdDistance = clamp(oppHelperToGoalDistance - helperShootThreshold * NormalizedClamp(oppHelperToGoalDistance, 0.0f, helperShootThreshold * 2.0f), 0.0f, pitchHalfW);
            Vector3 helperShootingPoint = oppHelperPosition + (goalPos - oppHelperPosition).GetNormalized(0) * oppHelperToThresholdDistance;

            Player *mateHelper = AI_GetClosestPlayer(controller->GetTeam(), helperShootingPoint, false, static_cast<Player*>(controller->GetPlayer()));
            float mateHelperToThresholdDistance = 99999;
            if (mateHelper) mateHelperToThresholdDistance = (helperShootingPoint - (mateHelper->GetPosition() + mateHelper->GetMovement() * 0.24f)).GetLength();

            // secondaryDistanceDiff：越大说明第二威胁对手越危险 → 减少出击
            float secondaryDistanceDiff = 0.0f;
            if (mateHelperToThresholdDistance > oppHelperToThresholdDistance) secondaryDistanceDiff = NormalizedClamp(mateHelperToThresholdDistance - oppHelperToThresholdDistance, 0.0f, 2.0f);

            // helperVSPrimaryDistanceRatio：接应对手越接近主威胁距离，越危险
            float helperVSPrimaryDistanceRatio = 1.0f - NormalizedClamp(oppHelperToThresholdDistance / (oppToThresholdDistance + 0.0001f), 1.0f, 1.5f);
            helperVSPrimaryDistanceRatio *= 0.7f; // 允许一定程度出击，不完全退缩

            // 综合出击偏置
            awayFromGoalBias = clamp(1.0f - (secondaryDistanceDiff * helperVSPrimaryDistanceRatio), 0.0f, 1.0f);
          }
        }

      }  // end keeper come out code

      // 队伍AI强制冲出（ApplyKeeperRush触发）
      bool applyRushOut = controller->GetTeam()->GetController()->GetEndApplyKeeperRush_ms() > controller->GetMatch()->GetActualTime_ms();
      if (applyRushOut) awayFromGoalBias = 1.0f;

      // 实际离门线距离 = ballToGoal线长 × 出击偏置（但不低于最小距离）
      float distance = std::max(ballToGoal.GetLength() - 0.5f, 0.0f);
      awayFromGoalOffset_m = clamp(distance * awayFromGoalBias, awayFromGoalOffset_m, pitchHalfW);

      // 目标位置 = 底线交点沿角平分线向球方向偏移awayFromGoalOffset_m
      targetPos = ballToGoal.GetVertex(1) + (ballToGoal.GetVertex(0) - ballToGoal.GetVertex(1)).GetNormalized(0) * awayFromGoalOffset_m;

      // 回撤时（目标比当前更靠近门线）减速，给动画足够时间转身
      float u = 0.0f;
      float distanceToBallToGoalLine = ballToGoal.GetDistanceToPoint(controller->GetPlayer()->GetPosition(), u);
      if ((targetPos - goalPos).GetLength() < (controller->GetPlayer()->GetPosition() - goalPos).GetLength() &&
          distanceToBallToGoalLine < 1.0f && u > 0.0f) maxVelocity = walkVelocity;

      targetPos.coords[0] = clamp(targetPos.coords[0], -pitchHalfW + 0.2f, pitchHalfW - 0.2f);

    } else {
      // ---- 状态B：球飞向球门，跑到轨迹上拦截 ----
      maxVelocity = sprintVelocity;

      Line ballToGoal;
      ballToGoal.SetVertex(0, mentalImage->GetBallPrediction(10).Get2D());
      float minGoalLineDist = 0.4f;
      // 球预计落点（在门线上）
      Vector3 ballOverGoalLinePos =
          Vector3(pitchHalfW * controller->GetTeam()->GetDynamicSide(),
                  ballBoundForGoal_ycoord, 0);
      ballOverGoalLinePos += (ballToGoal.GetVertex(0) - ballOverGoalLinePos).GetNormalized(0) * minGoalLineDist;
      ballToGoal.SetVertex(1, ballOverGoalLinePos);
      float u = 0.0f;
      // 门将当前位置（0.05秒后）到球轨迹线的最近点参数u
      float distance = ballToGoal.GetDistanceToPoint(controller->GetPlayer()->GetPosition() + controller->GetPlayer()->GetMovement() * 0.05f, u);

      // 预测1秒后球的位置，判断门将能否在球落地前到达拦截点
      float u_at_1sec = 0.0f;
      float distance_at_1sec = ballToGoal.GetDistanceToPoint(mentalImage->GetBallPrediction(1010).Get2D(), u_at_1sec);

      bool should_gk_run_towards_the_goal = false;
      if (u_at_1sec > 1e-4) {
        DO_VALIDATION;
        float time_to_reach_gk = u / u_at_1sec;
        Vector3 ball_position_at_gk = mentalImage->GetBallPrediction(10 + 1000 * time_to_reach_gk);
        // 若到达门将时球高于2.5米，改回撤（高球扑救方式不同）
        if (ball_position_at_gk.coords[2] > 2.5) {
          DO_VALIDATION;
          should_gk_run_towards_the_goal = true;
        }
      }

      u = clamp(u, 0.0f, 1.0f);

      if (should_gk_run_towards_the_goal) {
        DO_VALIDATION;
        targetPos = ballOverGoalLinePos; // 回撤到门线落点
      } else {
        // 跑到球轨迹的最近拦截点
        targetPos = ballToGoal.GetVertex(0) + (ballToGoal.GetVertex(1) - ballToGoal.GetVertex(0)) * u;
        targetPos.coords[2] = 0.0;
        targetPos.coords[0] = clamp(targetPos.coords[0], -pitchHalfW + 0.2f, pitchHalfW - 0.2f);
      }
    }
  }

  direction = targetPos - controller->GetPlayer()->GetPosition();
  velocity = clamp(direction.GetLength() * distanceToVelocityMultiplier, idleVelocity, maxVelocity);
  direction.Normalize(controller->GetPlayer()->GetDirectionVec());
}

void GoalieDefaultStrategy::CalculateIfBallIsBoundForGoal(
    ElizaController *controller, const MentalImage *mentalImage) {
  DO_VALIDATION;

  ballBoundForGoal = false;
  bool intersect = false;
  ballBoundForGoal_ycoord = 0;

  int side = controller->GetTeam()->GetDynamicSide();

  float panic = 1.02f + (1.0f - (controller->GetPlayer()->GetStat(mental_defensivepositioning) * 0.6f + controller->GetPlayer()->GetStat(mental_vision) * 0.4f)) * 0.5f;
  if (mentalImage->GetBallPrediction(4000).coords[0] * side > pitchHalfW &&
      (controller->GetPlayer()->GetPosition() -
       mentalImage->GetBallPrediction(250))
              .GetLength() < 32.0f) {
    DO_VALIDATION;  // only if ball is close enough (cpu optimization)

    /* 3d version
        Line line;
        line.SetVertex(0, mentalImage->GetBallPrediction(40));
        line.SetVertex(1, mentalImage->GetBallPrediction(4000));
        //line.SetVertex(1, mentalImage->GetBallPrediction(0) +
       match->GetBall()->GetMovement() * 800); Triangle goal1;
        goal1.SetVertex(0, Vector3((pitchHalfW - 0.0) * side,  3.7 * panic, 0));
        goal1.SetVertex(1, Vector3((pitchHalfW - 0.0) * side, -3.7 * panic, 0));
        goal1.SetVertex(2, Vector3((pitchHalfW - 0.0) * side,  3.7 * panic, 2.5
       * panic)); goal1.SetNormals(Vector3(-side, 0, 0)); Triangle goal2;
        goal2.SetVertex(0, Vector3((pitchHalfW - 0.0) * side, -3.7 * panic, 0));
        goal2.SetVertex(1, Vector3((pitchHalfW - 0.0) * side, -3.7 * panic, 2.5
       * panic)); goal2.SetVertex(2, Vector3((pitchHalfW - 0.0) * side,  3.7 *
       panic, 2.5 * panic)); goal2.SetNormals(Vector3(-side, 0, 0));

        //match->SetDebugPilon(Vector3(55 * side, 3.66, 2.44));
        //match->SetDebugPilon2(line.GetVertex(1));

        Vector3 intersectVec;
        intersect = goal1.IntersectsLine(line, intersectVec);
        if (!intersect) { DO_VALIDATION;
          intersect = goal2.IntersectsLine(line, intersectVec);
        }
    */

    // 2d version

    Line ballToGoal;
    ballToGoal.SetVertex(0, mentalImage->GetBallPrediction(0).Get2D());
    ballToGoal.SetVertex(1, mentalImage->GetBallPrediction(800).Get2D());
    Line goalLine;
    goalLine.SetVertex(0, Vector3(pitchHalfW * side, -pitchHalfH, 0));
    goalLine.SetVertex(1, Vector3(pitchHalfW * side,  pitchHalfH, 0));

    Vector3 intersectPoint = ballToGoal.GetIntersectionPoint(goalLine).Get2D();
    if (fabs(intersectPoint.coords[1]) > 3.7 * panic) intersect = false; else intersect = true;

    if (intersect) {
      DO_VALIDATION;
      //SetGreenDebugPilon(intersectPoint);
      ballBoundForGoal_ycoord = intersectPoint.coords[1];
      ballBoundForGoal = true;
    } else {
      //SetGreenDebugPilon(Vector3(0, 0, -10));
    }
  }
}

void GoalieDefaultStrategy::ProcessState(EnvState *state) {
  DO_VALIDATION;
  state->process(ballBoundForGoal);
  state->process(ballBoundForGoal_ycoord);
}
