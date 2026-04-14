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

#include "../../../main.hpp"

#include "elizacontroller.hpp"

#include <cmath>

#include "../../AIsupport/mentalimage.hpp"
#include "../../AIsupport/AIfunctions.hpp"

#include "../humanoid/humanoid_utils.hpp"

#include "strategies/strategy.hpp"
#include "../playerofficial.hpp"

ElizaController::ElizaController(Match *match, bool lazyPlayer)
    : PlayerController(match), lazyPlayer(lazyPlayer) {
  DO_VALIDATION;
}

ElizaController::~ElizaController() { DO_VALIDATION; }

// ============================================================
// RequestCommand —— 内置AI每帧决策入口
// 负责判断当前情境并向 commandQueue 推送本帧的行动指令。
// 指令优先级（从高到低）：
//   1. 进球庆祝
//   2. 停球时朝向裁判
//   3. 比赛暂停 / 懒人球员 → 站立不动
//   4. 定位球执行球员 / 持球球员
//   5. 无球球员跑位策略（后卫/中场/前锋）
//   6. 指定控球球员 → 传/射/带球
//   7. 门将专属逻辑
//   8. 全员：球控/停球/干扰/滑铲
//   9. 移动指令（包含逼抢、盯人）
// ============================================================
void ElizaController::RequestCommand(PlayerCommandQueue &commandQueue) {
  DO_VALIDATION;
  // 获取当前帧的"心理快照"（AI看到的延迟世界状态）
  auto _mentalImage = match->GetMentalImage(_mentalImageTime);
  lastSwitchTimeDuration_ms = 0;
  lastSwitchTime_ms = 0;

  CastPlayer()->SetDesiredTimeToBall_ms(0);

  // 计算当前情境（控球、体力等）
  _CalculateSituation();

  // 预处理：计算若干中间变量
  _Preprocess();


  FormationEntry entry = CastPlayer()->GetDynamicFormationEntry();
  // mindSet: 0=纯防守型 ~ 1=纯进攻型，决定跑位激进程度
  float mindSet = AI_GetMindSet(entry.role);


  // --- 原始输入方向/速度（由策略层写入，供移动指令使用）---
  Vector3 rawInputDirection = player->GetDirectionVec();
  float rawInputVelocityFloat = idleVelocity;
  Vector3 manualMovementDirection = player->GetDirectionVec();
  float manualMovementVelocityFloat = idleVelocity;

  bool manualMovement = false; // true时跳过自动移动逻辑（门将拦截）
  bool extraHaste = false;     // true时移动更紧迫（抢球争夺中）


  // -------------------------------------------------------
  // 1. 进球庆祝
  // -------------------------------------------------------
  if (!match->IsInPlay() && match->IsGoalScored()) {
    DO_VALIDATION;
    _AddCelebration(commandQueue);
    return;
  }

  // -------------------------------------------------------
  // 2. 停球期间朝向裁判（犯规类型2/3，比赛暂停后1秒内）
  // -------------------------------------------------------
  else if (!match->IsInPlay() &&
           match->GetReferee()->GetBuffer().active == true &&
           (match->GetReferee()->GetCurrentFoulType() == 2 ||
            match->GetReferee()->GetCurrentFoulType() == 3) &&
           match->GetReferee()->GetBuffer().stopTime <
               match->GetActualTime_ms() - 1000 &&
           match->GetReferee()->GetBuffer().prepareTime >
               match->GetActualTime_ms()) {
    DO_VALIDATION;

    // 站立，面朝裁判位置
    PlayerCommand command;
    command.desiredFunctionType = e_FunctionType_Movement;
    command.useDesiredMovement = true;
    command.useDesiredLookAt = true;
    command.desiredDirection = CastPlayer()->GetDirectionVec();
    assert(command.desiredDirection.coords[2] == 0.0f);
    command.desiredVelocityFloat = idleVelocity;
    command.desiredLookAt = match->GetOfficials()->GetReferee()->GetPosition();
    commandQueue.push_back(command);

    return;
  }

  // -------------------------------------------------------
  // 3. 比赛暂停 或 懒人球员：减速至停止，视线对准球
  // -------------------------------------------------------
  else if (!match->IsInPlay() || lazyPlayer) {
    DO_VALIDATION;  // this whole if/then/else structure is ugly and unclear

    PlayerCommand command;
    command.desiredFunctionType = e_FunctionType_Movement;
    command.useDesiredMovement = true;
    if (match->GetBallRetainer() == player) {
      // 持球时朝向场地中心，准备发球
      DO_VALIDATION;
      command.desiredDirection = (Vector3(0) - player->GetPosition()).GetNormalized(player->GetDirectionVec());
    } else {
      command.desiredDirection = player->GetDirectionVec();
    }
    command.desiredVelocityFloat = idleVelocity;
    // 方向混合：60%保持当前方向 + 40%朝向中心 → 自然减速
    command.desiredDirection =
        (player->GetDirectionVec() * 0.6f +
         (Vector3(0) - player->GetPosition())
                 .GetNormalized(player->GetDirectionVec()) *
             0.4f)
            .GetNormalized(player->GetDirectionVec());
    // 速度逐帧衰减（×0.95），加随机抖动，模拟不同球员减速节奏
    command.desiredVelocityFloat = ClampVelocity(
        player->GetFloatVelocity() * 0.95f - boostrandom(0.0f, 3.2f));
    command.useDesiredLookAt = true;
    // 视线朝向球的位置（让球员面球）
    command.desiredLookAt =
        player->GetPosition() +
        (match->GetBall()->Predict(0).Get2D() - player->GetPosition())
                .GetNormalized(command.desiredDirection) *
            10.0f;
    commandQueue.push_back(command);

    return;
  }

  // -------------------------------------------------------
  // 4. 定位球执行球员 / 持球球员（门将发球等）
  // -------------------------------------------------------
  else if ((match->IsInSetPiece() &&
            team->GetController()->GetPieceTaker() == player) ||
           match->GetBallRetainer() == player) {
    DO_VALIDATION;

    PlayerCommand actionCommand;

    // --- 4a. 点球：直接射门 ---
    if (team->GetController()->GetSetPieceType() == e_GameMode_Penalty) {
      DO_VALIDATION;

      actionCommand.desiredFunctionType = e_FunctionType_Shot;
      actionCommand.useDesiredMovement = false;
      actionCommand.useDesiredLookAt = false;
      // 射门方向：对方球门中心偏随机[-5,5]米（y轴），模拟点球落点
      actionCommand.touchInfo.desiredDirection =
          (Vector3(-team->GetDynamicSide() * pitchHalfW, boostrandom(-5, 5),
                   0) -
           CastPlayer()->GetPosition())
              .GetNormalized(Vector3(-team->GetDynamicSide(), 0, 0));
      actionCommand.touchInfo.desiredPower = boostrandom(0.4f, 1.0f);
      commandQueue.push_back(actionCommand);

    } else {
      // --- 4b. 其他定位球：传球 ---
      actionCommand.desiredFunctionType = e_FunctionType_ShortPass;
      actionCommand.useDesiredMovement = false;
      actionCommand.useDesiredLookAt = false;
      actionCommand.touchInfo.inputDirection = player->GetDirectionVec();
      actionCommand.touchInfo.inputPower = 0.5f;
      actionCommand.touchInfo.autoDirectionBias = 1.0f; // 完全依赖AI自动选方向
      actionCommand.touchInfo.autoPowerBias = 1.0f;     // 完全依赖AI自动选力度

      actionCommand.touchInfo.forcedTargetPlayer = 0;

      Vector3 desiredTargetPosition;
      bool doCommand = true;

      // 球门球：60%概率高球长传，40%概率短传
      if (team->GetController()->GetSetPieceType() == e_GameMode_GoalKick) {
        DO_VALIDATION;
        if (boostrandom(0.0f, 1.0f) > 0.4f && team->GetHumanGamerCount() == 0) {
          DO_VALIDATION;
          actionCommand.desiredFunctionType = e_FunctionType_HighPass;
          desiredTargetPosition =
              Vector3((pitchHalfW * -team->GetDynamicSide()) * 0.2f,
                      boostrandom(-pitchHalfH, pitchHalfH), 0.0f);
        } else {
          actionCommand.desiredFunctionType = e_FunctionType_ShortPass;
          desiredTargetPosition =
              Vector3(player->GetPosition().coords[0] * 0.9f,
                      boostrandom(-pitchHalfH, pitchHalfH), 0.0f);
        }

      // 开球：直接短传给身前队友
      } else if (team->GetController()->GetSetPieceType() ==
                 e_GameMode_KickOff) {
        DO_VALIDATION;
        actionCommand.desiredFunctionType = e_FunctionType_ShortPass;
        desiredTargetPosition = player->GetPosition() + player->GetDirectionVec() * 1.0f;

      // 任意球：50%高球传远，50%短传到斜前方
      } else if (team->GetController()->GetSetPieceType() ==
                 e_GameMode_FreeKick) {
        DO_VALIDATION;
        if (boostrandom(0.0f, 1.0f) > 0.5f) {
          DO_VALIDATION;
          actionCommand.desiredFunctionType = e_FunctionType_HighPass;
          desiredTargetPosition = Vector3(pitchHalfW * -team->GetDynamicSide(),
                                          boostrandom(-10.0f, 10.0f), 0.0f);
        } else {
          actionCommand.desiredFunctionType = e_FunctionType_ShortPass;
          desiredTargetPosition =
              player->GetPosition() + Vector3(-team->GetDynamicSide() * 10.0f,
                                              boostrandom(-10.0f, 10.0f), 0.0f);
        }

      // 角球：70%传禁区，30%短角球
      } else if (team->GetController()->GetSetPieceType() ==
                 e_GameMode_Corner) {
        DO_VALIDATION;
        if (boostrandom(0.0f, 1.0f) > 0.3f) {
          DO_VALIDATION;
          actionCommand.desiredFunctionType = e_FunctionType_HighPass;
          desiredTargetPosition =
              Vector3((pitchHalfW * -team->GetDynamicSide()) *
                          (0.99f - boostrandom(0.0f, 0.12f)),
                      boostrandom(-10.0f, 10.0f), 0.0f);
        } else {
          actionCommand.desiredFunctionType = e_FunctionType_ShortPass;
          desiredTargetPosition =
              Vector3((pitchHalfW * -team->GetDynamicSide()) * 0.8f,
                      player->GetPosition().coords[1] * 0.8f, 0.0f);
        }

      // 界外球：短传给最近队友
      } else if (team->GetController()->GetSetPieceType() ==
                 e_GameMode_ThrowIn) {
        DO_VALIDATION;
        actionCommand.desiredFunctionType = e_FunctionType_ShortPass;
        desiredTargetPosition = player->GetPosition(); // closest to player

      // 门将持球（如接球后）：高球传到前场，但先检查目标队友是否被对手紧逼
      } else if (match->GetBallRetainer() == player) {
        DO_VALIDATION;  // keeper fetched ball, probably
        actionCommand.desiredFunctionType = e_FunctionType_HighPass;
        desiredTargetPosition =
            Vector3(pitchHalfW * team->GetDynamicSide(),
                    boostrandom(-pitchHalfH, pitchHalfH), 0.0f);
        Player *targetPlayer = AI_GetClosestPlayer(team, desiredTargetPosition, false, CastPlayer());

        if (targetPlayer) {
          DO_VALIDATION;
          // 检查目标队友与最近对手的距离；距离>10m 或已持球超过4秒才传
          Player *closestOpp = AI_GetClosestPlayer(match->GetTeam(abs(team->GetID() - 1)), targetPlayer->GetPosition(), false, 0);
          if (closestOpp) {
            DO_VALIDATION;
            if (((closestOpp->GetPosition() +
                  closestOpp->GetMovement() * 0.1f) -
                 targetPlayer->GetPosition())
                        .GetLength() > 10.0f ||
                CastPlayer()->GetPossessionDuration_ms() > 4000) {
              DO_VALIDATION;  // last touch bias is maximum so he won't hold
                              // forever
              actionCommand.touchInfo.forcedTargetPlayer = targetPlayer;
            } else {
              doCommand = false; // 目标不安全，暂不传球
            }
          }
        }
      }

      if (doCommand) {
        DO_VALIDATION;
        // 若未指定强制目标，寻找离目标位最近的队友
        if (actionCommand.touchInfo.forcedTargetPlayer == 0) actionCommand.touchInfo.forcedTargetPlayer = AI_GetClosestPlayer(team, desiredTargetPosition, false, CastPlayer());
        // 调用AI传球计算：确定传球方向、力度、目标球员
        AI_GetPass(CastPlayer(), actionCommand.desiredFunctionType, actionCommand.touchInfo.inputDirection, actionCommand.touchInfo.inputPower, actionCommand.touchInfo.autoDirectionBias, actionCommand.touchInfo.autoPowerBias, actionCommand.touchInfo.desiredDirection, actionCommand.touchInfo.desiredPower, actionCommand.touchInfo.targetPlayer, actionCommand.touchInfo.forcedTargetPlayer);
        commandQueue.push_back(actionCommand);
      }
    }

    // 仍在定位球/持球 if 块内
    if (match->GetBallRetainer() != player) {
      // 定位球执行球员：附加控球移动指令（保持在球旁边）
      DO_VALIDATION;
      PlayerCommand command;
      command.desiredFunctionType = e_FunctionType_Movement;
      command.useDesiredMovement = true;
      command.useDesiredLookAt = true;

      AI_GetBallControlMovement(
          _mentalImage, CastPlayer(), player->GetDirectionVec(),
          walkVelocity, command.desiredDirection, command.desiredVelocityFloat,
          command.desiredLookAt);
      assert(command.desiredDirection.coords[2] == 0.0f);

      commandQueue.push_back(command);
      return;
    } else {  // 门将持球：站立，视线朝向传球方向
      PlayerCommand command;
      command.desiredFunctionType = e_FunctionType_Movement;
      command.useDesiredMovement = true;
      command.desiredDirection = (actionCommand.touchInfo.desiredDirection.GetLength() > 0.0f) ? actionCommand.touchInfo.desiredDirection.Get2D().GetNormalized(player->GetDirectionVec()) : (Vector3(0) - player->GetPosition()).GetNormalized(player->GetDirectionVec());
      command.desiredVelocityFloat = idleVelocity;
      command.useDesiredLookAt = true;
      command.desiredLookAt = player->GetPosition() + command.desiredDirection * 10.0f;
      commandQueue.push_back(command);
      if (team->GetController()->GetSetPieceType() == e_GameMode_ThrowIn) {
        DO_VALIDATION;
        //printf("elizacontroller throw in doCommand (green pilon == target)\n");
        //SetGreenDebugPilon(command.desiredLookAt);
      }
      return;
    }

  }

  // -------------------------------------------------------
  // 5. 无球跑位策略（后卫/中场/前锋，不含门将）
  //    根据球员角色调用不同的 Strategy::RequestInput，
  //    写入 rawInputDirection / rawInputVelocityFloat
  // -------------------------------------------------------
  else if (match->IsInPlay() && !match->IsInSetPiece() &&
           match->GetBallRetainer() != player &&
           match->GetDesignatedPossessionPlayer() != player &&
           CastPlayer()->GetFormationEntry().role != e_PlayerRole_GK) {
    DO_VALIDATION;
    if (CastPlayer()->GetFormationEntry().role == e_PlayerRole_LB ||
        CastPlayer()->GetFormationEntry().role == e_PlayerRole_CB ||
        CastPlayer()->GetFormationEntry().role == e_PlayerRole_RB) {
      // 后卫跑位：保守防线，兼顾逼抢
      DO_VALIDATION;
      defenseStrategy.RequestInput(this, _mentalImage, rawInputDirection,
                                   rawInputVelocityFloat);
    } else if (CastPlayer()->GetFormationEntry().role == e_PlayerRole_DM ||
               CastPlayer()->GetFormationEntry().role == e_PlayerRole_LM ||
               CastPlayer()->GetFormationEntry().role == e_PlayerRole_CM ||
               CastPlayer()->GetFormationEntry().role == e_PlayerRole_RM ||
               CastPlayer()->GetFormationEntry().role == e_PlayerRole_AM) {
      // 中场跑位：攻守均衡，随控球方向移动
      DO_VALIDATION;
      midfieldStrategy.RequestInput(this, _mentalImage, rawInputDirection,
                                    rawInputVelocityFloat);
    } else if (CastPlayer()->GetFormationEntry().role == e_PlayerRole_CF) {
      // 前锋跑位：积极前插，寻找接球空间
      DO_VALIDATION;
      offenseStrategy.RequestInput(this, _mentalImage, rawInputDirection,
                                   rawInputVelocityFloat);
    }

  }

  // -------------------------------------------------------
  // 6. 控球球员（场上指定持球者）：传球/射门/带球决策
  // -------------------------------------------------------
  else if (match->IsInPlay() && !match->IsInSetPiece() &&
           match->GetDesignatedPossessionPlayer() == player &&
           (team->GetHumanGamerCount() == 0 ||
           (!CastPlayer()->ExternalControllerActive() && CastPlayer()->ExternalController())) &&
           CastPlayer()->GetFormationEntry().role != e_PlayerRole_GK) {
    DO_VALIDATION;
    // 只有在能够1秒内到球时才触发在球行动
    if (CastPlayer()->GetTimeNeededToGetToBall_ms() < 1000) {
      DO_VALIDATION;
      GetOnTheBallCommands(commandQueue, rawInputDirection, rawInputVelocityFloat);
    }
    extraHaste = false;
  }

  // -------------------------------------------------------
  // 7. 门将专属逻辑
  // -------------------------------------------------------
  else if (match->IsInPlay() && !match->IsInSetPiece() &&
           CastPlayer()->GetFormationEntry().role == e_PlayerRole_GK) {
    DO_VALIDATION;
    // 门将的心理快照更新几乎是即时的（近似预知球的轨迹）
    goalieStrategy.CalculateIfBallIsBoundForGoal(this, _mentalImage);
    bool boundForGoal = goalieStrategy.IsBallBoundForGoal();
    if (boundForGoal) manualMovement = true; // 球飞向球门→手动模式拦截
    if (CastPlayer() != match->GetDesignatedPossessionPlayer()) manualMovement = true;
    goalieStrategy.RequestInput(this, _mentalImage,
                                manualMovementDirection,
                                manualMovementVelocityFloat);

    // 没有绝对控球时，尝试扑球/接球动画
    if (!hasUniquePossession || possessionAmount < 3.4f) {
      DO_VALIDATION;
      bool onlyPickupAnims = false;
      if (!boundForGoal && possessionAmount > 1.3f) onlyPickupAnims = true;
      _KeeperDeflectCommand(commandQueue, onlyPickupAnims);
    }

    // 门将接近球且被指定为控球者时，也执行在球决策（传球/带球）
    if (CastPlayer()->GetTimeNeededToGetToBall_ms() < 1000 &&
        match->GetDesignatedPossessionPlayer() == player) {
      DO_VALIDATION;
      GetOnTheBallCommands(commandQueue, rawInputDirection, rawInputVelocityFloat);
    }
  }

  // -------------------------------------------------------
  // 8. 全员通用：球控/停球/干扰/滑铲
  // -------------------------------------------------------
  bool forceMagnet = false;

  // 队伍整体施压：若本球员被指定为施压球员，强制朝对方控球者方向移动
  float ballDistance = _mentalImage->GetBallPrediction(0).Get2D().GetDistance(player->GetPosition());
  if (match->IsInPlay() && !match->IsInSetPiece() &&
      ((team->GetController()->GetEndApplyTeamPressure_ms() > match->GetActualTime_ms() && team->GetController()
                                                                                                   ->GetTeamPressurePlayer() ==
                                                                                               player) /*|| (player == team->GetDesignatedTeamPossessionPlayer() && ballDistance < 8.0f && CastPlayer()->GetManMarkingID() == -1) */) &&
      !teamHasBestPossession &&
      match->GetDesignatedPossessionPlayer() != player &&
      CastPlayer()->GetFormationEntry().role != e_PlayerRole_GK) {
    DO_VALIDATION;
    forceMagnet = true; // 磁力模式：无视跑位策略，直接向目标点冲
  }

  _SetInput(rawInputDirection, rawInputVelocityFloat);
  if (rawInputDirection.GetLength() != 0) lastDesiredDirection = rawInputDirection; else
                                          lastDesiredDirection = player->GetDirectionVec();
  lastDesiredVelocity = rawInputVelocityFloat;

  if (match->IsInPlay() && !match->IsInSetPiece()) {
    DO_VALIDATION;

    // 球控指令：尝试接管球（碰球、控球）
    _BallControlCommand(commandQueue, false, false, false); // last param true == enable sticky run direction.

    // 停球指令：尝试停住来球
    _TrapCommand(commandQueue);

    // 干扰指令：尝试铲断或拦截对方
    bool byAnyMeans = false;
    _InterfereCommand(commandQueue, byAnyMeans);

    // 滑铲指令：激进抢球
    _SlidingCommand(commandQueue);
  }

  // -------------------------------------------------------
  // 9. 移动指令（非门将拦截状态）
  // -------------------------------------------------------
  if (!manualMovement && match->IsInPlay() && !match->IsInSetPiece()) {
    DO_VALIDATION;

    // 对方持球球员
    Player *opp = match->GetTeam(abs(team->GetID() - 1))->GetDesignatedTeamPossessionPlayer();

    if (CastPlayer() != match->GetDesignatedPossessionPlayer()) {
      DO_VALIDATION;

      float mindSet = AI_GetMindSet(CastPlayer()->GetDynamicFormationEntry().role);
      // 追球距离阈值：防守型球员追球半径更大（守得更远）
      // 受体力、平均速度、AI难度影响
      float huntDistanceThreshold = 10.0f + (1.0f - mindSet) * 10.0f; // 10 + .. * 10
      huntDistanceThreshold *= 0.5f * CastPlayer()->GetFatigueFactorInv() +
                               0.5f * (1.0f - NormalizedClamp(CastPlayer()->GetAverageVelocity(10), idleVelocity, sprintVelocity));
      huntDistanceThreshold *=
          0.3f + CastPlayer()->GetTeam()->GetAiDifficulty() * 0.7f;

      if (forceMagnet) {
        // 施压模式：朝向己方底线方向移动（把对方推离禁区）
        DO_VALIDATION;

        inputDirection = Vector3(team->GetDynamicSide() * pitchHalfW, 0, 0) -
                         CastPlayer()->GetPosition();
        inputVelocityFloat = idleVelocity;
        inputDirection.Normalize(CastPlayer()->GetDirectionVec());

      } else if (!teamHasBestPossession && !CastPlayer()->GetManMarking() &&
                 ((opp->GetPosition() + opp->GetMovement() * 0.12f) -
                  (CastPlayer()->GetPosition() +
                   CastPlayer()->GetMovement() * 0.04f))
                         .GetLength() < huntDistanceThreshold) {
        // 对方持球者在追球阈值内 → 主动防守/逼抢
        DO_VALIDATION;

        if (player == team->GetDesignatedTeamPossessionPlayer() &&
            possessionAmount > 0.8f) {
          DO_VALIDATION;
          forceMagnet = true;  // 已在争抢中，不轻易放弃
          extraHaste = true;
        }

        Player *opp = match->GetTeam(abs(team->GetID() - 1))
                          ->GetDesignatedTeamPossessionPlayer();

        // 最多2名球员同时追球（包括盯人球员）
        int huntingPlayersNum = 2;
        std::vector<Player *> closestPlayers;
        AI_GetClosestPlayers(team,
                             opp->GetPosition() + opp->GetMovement() * 0.1f,
                             false, closestPlayers, huntingPlayersNum);
        bool close = false;
        for (unsigned int i = 0; i < closestPlayers.size(); i++) {
          DO_VALIDATION;
          if (closestPlayers[i] == player) {
            DO_VALIDATION;
            close = true;
            break;
          }
        }

        if (close) {
          DO_VALIDATION;

          /*
          // 'easy off' method
          Vector3 defendPosition_easy = CastPlayer()->GetPosition();
          AddDefensiveComponent(defendPosition_easy, 1.0f, opp->GetID());
          inputDirection = (defendPosition -
          CastPlayer()->GetPosition()).GetNormalized(inputDirection);
          inputVelocityFloat = clamp((defendPosition -
          CastPlayer()->GetPosition()).GetLength() *
          distanceToVelocityMultiplier, 0.0f, sprintVelocity);
          //forceMagnet = true; // more aggressive!
          */

          // "追猎"方法：计算防守站位，向其移动
          Vector3 defendPosition = GetDefendPosition(opp);
          if (NeedDefendingMovement(team->GetDynamicSide(),
                                    player->GetPosition(), defendPosition)) {
            DO_VALIDATION;
            inputDirection = (defendPosition - CastPlayer()->GetPosition())
                                 .GetNormalized(inputDirection);
            inputVelocityFloat = clamp(
                (defendPosition - CastPlayer()->GetPosition()).GetLength() *
                    distanceToVelocityMultiplier,
                0.0f, sprintVelocity);
            forceMagnet = true;  // 激进逼抢
          }

        }  // close

      }  // else (!forceMagnet)

    }  // !designated

    // 将速度量化（模拟手柄档位分级）
    inputVelocityFloat = RangeVelocity(inputVelocityFloat);

    // 生成最终移动指令（结合跑位策略输出 + 磁力/紧迫标志）
    _MovementCommand(commandQueue, forceMagnet, extraHaste);

  } else {
    // 门将拦截状态：直接使用手动方向/速度，视线锁定球
    PlayerCommand command;
    command.desiredFunctionType = e_FunctionType_Movement;
    command.useDesiredMovement = true;
    command.desiredDirection = manualMovementDirection;
    assert(command.desiredDirection.coords[2] == 0.0f);
    command.desiredVelocityFloat = manualMovementVelocityFloat;
    command.useDesiredLookAt = true;
    command.desiredLookAt = player->GetPosition() + (_mentalImage->GetBallPrediction(40).Get2D() - player->GetPosition()).GetNormalized(0) * 10.0f;
    commandQueue.push_back(command);
  }
}

void ElizaController::Process() {
  DO_VALIDATION;
  PlayerController::Process();
}

Vector3 ElizaController::GetDirection() {
  DO_VALIDATION;
  return lastDesiredDirection;
}

float ElizaController::GetFloatVelocity() {
  DO_VALIDATION;
  return lastDesiredVelocity;
}

// ============================================================
// GetLazyVelocity —— 根据情境给期望速度打"懒惰折扣"
// 模拟球员在不需要全力的情况下节省体力的行为。
// 三个维度决定懒惰程度：
//   1. lazinessByPosition：离球越远越懒（距离阈值20~65米）
//   2. lazinessByRole：进攻型球员在己方控球时偷懒；后卫反之
//   3. breathLeftFactor：短期冲刺后喘气减速（工作率属性调节）
// ============================================================
float ElizaController::GetLazyVelocity(float desiredVelocityFloat) {
  DO_VALIDATION;

  // 输入速度未做限制；超过冲刺速度的部分只保留10%，避免溢出导致极端值
  float adaptedDesiredVelocityFloat = desiredVelocityFloat;
  if (adaptedDesiredVelocityFloat > sprintVelocity) adaptedDesiredVelocityFloat = sprintVelocity + (adaptedDesiredVelocityFloat - sprintVelocity) * 0.1f;

  // 懒惰生效的距离区间（受体力影响：体力越差，懒惰越早生效）
  float startLazinessDistance = 20.0f * (CastPlayer()->GetFatigueFactorInv() * 0.8f + 0.2f);
  float endLazinessDistance = 65.0f * (CastPlayer()->GetFatigueFactorInv() * 0.5f + 0.5f);

  Vector3 oppPos = match->GetTeam(abs(team->GetID() - 1))->GetDesignatedTeamPossessionPlayer()->GetPosition();
  float actionDistance = (player->GetPosition() - oppPos).GetLength(); // 与对方控球者的距离
  float teamPossession = clamp(GetFadingTeamPossessionAmount() - 0.5f, 0.0f, 1.0f); // 己方控球程度 [0,1]
  float mindSet = AI_GetMindSet(CastPlayer()->GetDynamicFormationEntry().role);

  // 角色懒惰因子：
  //   进攻型(mindSet=1)：己方控球时节省体力（等球来），失球时拼命跑
  //   防守型(mindSet=0)：己方控球时积极跑位，失球时守住阵型
  float lazinessByRole = mindSet + teamPossession * (1.0f - mindSet * 2.0f);
  // 位置懒惰因子：离动作中心越远，越放松
  float lazinessByPosition = NormalizedClamp(actionDistance, startLazinessDistance, endLazinessDistance);

  float lazyFactor = lazinessByPosition * (0.5f + lazinessByRole * 0.5f);
  float resultingVelocityFloat = adaptedDesiredVelocityFloat * (1.0f - lazyFactor);

  // 若期望速度≥带球速度，确保结果也不低于带球速度（不能原地停下）
  bool clampToDribble = false;
  if (desiredVelocityFloat >= dribbleVelocity) clampToDribble = true;
  if (clampToDribble && resultingVelocityFloat < dribbleVelocity) resultingVelocityFloat = dribbleVelocity;

  // 短期疲劳（喘气）：近10帧平均速度越高，剩余"气"越少
  // workRate属性越高的球员，喘气曲线越平缓（更耐跑）
  float breathLeftFactor = 1.0f - NormalizedClamp(CastPlayer()->GetAverageVelocity(10), idleVelocity, sprintVelocity);
  float workRate = CastPlayer()->GetStat(mental_workrate);
  breathLeftFactor = std::pow(breathLeftFactor, 0.8f - workRate * 0.2f);
  breathLeftFactor = clamp(breathLeftFactor * 1.2f, 0.0f, 1.0f); // 确保冲刺初始阶段是全速
  // 在需要全力（lazyFactor低）的情况下不受喘气限制
  breathLeftFactor = breathLeftFactor * lazyFactor + 1.0f * (1.0f - lazyFactor);
  resultingVelocityFloat = std::min(resultingVelocityFloat, sprintVelocity * breathLeftFactor);

  return resultingVelocityFloat;
}

// ============================================================
// GetSupportPosition_ForceField —— 力场法计算无球支援跑位
//
// 核心思路：在球场上叠加多个"力场源"（吸引/排斥），
// 通过合力求出当前球员的最优支援位置。
// 各力场贡献：
//   + 吸引：阵型基础位置（最重要）
//   + 吸引：冲刺跑位目标（指定前插球员）
//   + 吸引：控球队友附近（保持传球距离）
//   - 排斥：对方球员（拉开空间）
//   - 排斥：己方队友（避免堆在一起）
//   - 排斥：球的预测路径（不挡传球线路）
//   - 排斥：控球队友（太近会妨碍）
// 最终位置额外受越位线约束。
// ============================================================
Vector3 ElizaController::GetSupportPosition_ForceField(
    const MentalImage *mentalImage, const Vector3 &basePosition, bool makeRun) {
  DO_VALIDATION;
  auto _mentalImage = match->GetMentalImage(_mentalImageTime);
  Player *designatedPlayer = team->GetDesignatedTeamPossessionPlayer();

  // 当前球员预测位置（考虑0.1秒后的移动）
  Vector3 currentPos = player->GetPosition() + CastPlayer()->GetMovement() * 0.1f;
  // 控球队友预测位置
  Vector3 mainManPos = designatedPlayer->GetPosition() + designatedPlayer->GetMovement() * 0.1f;

  std::vector<ForceSpot> forceField;

  float dynamicMindSet = AI_GetMindSet(CastPlayer()->GetDynamicFormationEntry().role);
  float ballDistance = (match->GetBall()->Predict(250).Get2D() - player->GetPosition()).GetLength();
  float ballDistanceX = fabs(match->GetBall()->Predict(250).coords[0] - player->GetPosition().coords[0]);

  bool forceNoOffside = true; // 禁止越位

  // 各力场权重（可按需调节平衡进攻/防守性）
  float basePositionWeight = 0.7f;
  float overallWeight = 1.0f;
  float opponentRepelWeight = 0.3f * overallWeight; // 对方排斥权重（按角色放大）
  float teammateRepelWeight = 0.4f * overallWeight; // 队友排斥权重
  float ballRepelWeight = 1.0f * overallWeight;     // 球路排斥权重
  float runWeight = 1.0f * overallWeight;            // 前插跑权重
  float flockToPossessionPlayerWeight = 0.45f * overallWeight; // 靠近控球者权重

  float webScale = 0.75f; // 整体力场范围缩放

  // 后卫/中场对对方的排斥更强（防守意识更强，不让对方靠近）
  switch (CastPlayer()->GetDynamicFormationEntry().role) {
    DO_VALIDATION;
    case e_PlayerRole_CB:
    case e_PlayerRole_LB:
    case e_PlayerRole_RB:
      opponentRepelWeight *= 2.2f;
      break;
    case e_PlayerRole_DM:
      opponentRepelWeight *= 2.0f;
      break;
    case e_PlayerRole_CM:
    case e_PlayerRole_LM:
    case e_PlayerRole_RM:
      opponentRepelWeight *= 1.6f;
      break;
    case e_PlayerRole_AM:
      opponentRepelWeight *= 1.2f;
      break;
    case e_PlayerRole_CF:
      opponentRepelWeight *= 1.0f;
      break;
    default:
      break;
  }

  // 越位线（对方防线位置），用于最终约束
  float offsideX =
      AI_GetOffsideLine(match, _mentalImage, abs(team->GetID() - 1), 240);
  float adaptedMakeRun = makeRun;

  // --- 力场1：阵型基础位置（吸引） ---
  {
    ForceSpot spot;
    spot.origin = basePosition;

    Player *forwardSupportPlayer = team->GetController()->GetForwardSupportPlayer();
    if (player == forwardSupportPlayer) {
      // 指定前插球员：在基础位置上额外向前推（进攻性越强推得越远）
      DO_VALIDATION;
      spot.origin.coords[0] +=
          -team->GetDynamicSide() * (0.3f + 0.7f * dynamicMindSet) * 12.0f;
    } else {
      /* sine version（正弦波版本，已弃用）
      ...
      */
      // 通道版本：根据当前球员与控球者的y轴位置关系，向前推进
      float amount = 22.0f;
      float laneY = -signSide(mainManPos.coords[1]) * 8.0f;
      // 越靠近laneY的球员，前插幅度越大
      amount *= curve(1.0f - NormalizedClamp(fabs(laneY - currentPos.coords[1]), 0.0f, 30.0f), 1.0f);
      float delta =
          -team->GetDynamicSide() * std::pow(dynamicMindSet, 1.5f) * amount;
      spot.origin.coords[0] += delta;
    }

    spot.magnetType = e_MagnetType_Attract;
    spot.decayType = e_DecayType_Constant;
    spot.power = 1.0f * basePositionWeight;
    // 越远离目标位，引力越强（让球员坚持跑到位）
    spot.power *= 0.3f + 0.7f * NormalizedClamp((spot.origin - currentPos).GetLength(), 0.0f, 20.0f);
    forceField.push_back(spot);
  }

  // --- 力场2：前插跑目标（吸引，仅限makeRun=true球员） ---
  if (adaptedMakeRun) {
    DO_VALIDATION;
    ForceSpot spot;
    // 目标：对方底线附近（纵向位置跟随自身，横向全力前插）
    spot.origin = Vector3(-team->GetDynamicSide() * pitchHalfW,
                          currentPos.coords[1] * 0.5f,
                          0.0f);
    spot.magnetType = e_MagnetType_Attract;
    spot.decayType = e_DecayType_Constant;
    spot.power = 2.0f * runWeight;
    forceField.push_back(spot);
  }

  // --- 力场3：远离对方球员（排斥） ---
  // 取离控球者附近最近的3名对手
  std::vector<Player*> opponents;
  AI_GetClosestPlayers(match->GetTeam(abs(team->GetID() - 1)), mainManPos * 0.3f + currentPos + 0.7f, false, opponents, 3);
  for (unsigned int i = 0; i < opponents.size(); i++) {
    DO_VALIDATION;
    const PlayerImage &oppImg = mentalImage->GetPlayerImage(opponents[i]);
    ForceSpot spot;
    Vector3 oppPos = oppImg.position + oppImg.movement * 0.1f;
    // 排斥源放在对手身后，确保传球路线畅通
    spot.origin = oppPos + (oppPos - mainManPos).GetNormalized(0) * 2.0f;
    spot.magnetType = e_MagnetType_Repel;
    spot.decayType = e_DecayType_Variable;
    spot.power = 1.0f * opponentRepelWeight;
    spot.scale = 5.0f;
    if (adaptedMakeRun) {
      // 前插时对对手排斥减弱（更激进）
      DO_VALIDATION;
      spot.scale = 2.0f;
      spot.power *= 0.5f;
    }
    spot.exp = 0.7f;
    forceField.push_back(spot);
  }

  // --- 力场4：远离己方队友（排斥，拉开间距） ---
  // 仅在己方控球时生效（控球量≥1.02）
  if (team->GetFadingTeamPossessionAmount() >= 1.02f) {
    DO_VALIDATION;
    std::vector<Player*> players;
    AI_GetClosestPlayers(team, currentPos, false, players, 6);
    for (unsigned int i = 0; i < players.size(); i++) {
      DO_VALIDATION;
      if (players[i] != CastPlayer()) {
        DO_VALIDATION;
        const PlayerImage &mateImg = mentalImage->GetPlayerImage(players[i]);
        ForceSpot spot;
        spot.origin = mateImg.position + mateImg.movement * 0.1f;
        spot.magnetType = e_MagnetType_Repel;
        spot.decayType = e_DecayType_Variable;
        spot.power = 1.0f * teammateRepelWeight;
        spot.scale = 14.0f * webScale;
        spot.exp = 1.0f;
        forceField.push_back(spot);
      }
    }
  }

  // --- 力场5：远离球的预测轨迹（排斥，不挡传球路线） ---
  // 仅在己方有明显控球优势时生效（控球量≥1.06）
  if (CastPlayer() != designatedPlayer &&
      team->GetFadingTeamPossessionAmount() >= 1.06f) {
    DO_VALIDATION;
    ForceSpot spot;
    spot.magnetType = e_MagnetType_Repel;
    spot.decayType = e_DecayType_Variable;
    spot.power = 1.0f * ballRepelWeight;
    spot.scale = 2.0f;
    spot.exp = 0.5f;
    // 在未来200~650ms的球的预测位置上各放一个排斥源
    spot.origin = mentalImage->GetBallPrediction(200).Get2D();
    forceField.push_back(spot);
    spot.origin = mentalImage->GetBallPrediction(350).Get2D();
    forceField.push_back(spot);
    spot.origin = mentalImage->GetBallPrediction(500).Get2D();
    forceField.push_back(spot);
    spot.origin = mentalImage->GetBallPrediction(650).Get2D();
    forceField.push_back(spot);
  }

  // --- 力场6/7：靠近控球队友（吸引）但不要太近（排斥） ---
  if (CastPlayer() != designatedPlayer) {
    DO_VALIDATION;

    // 吸引：靠近控球队友（保持传球距离）
    {
      ForceSpot spot;
      spot.origin = mainManPos;
      spot.magnetType = e_MagnetType_Attract;
      spot.decayType = e_DecayType_Variable;
      spot.power = 1.0f * flockToPossessionPlayerWeight;
      spot.scale = 28.0f * webScale;
      spot.exp = 1.0f;
      forceField.push_back(spot);
    }

    // 排斥：不要离控球队友太近（避免挡路）
    {
      ForceSpot spot;
      spot.origin = mainManPos;
      spot.magnetType = e_MagnetType_Repel;
      spot.decayType = e_DecayType_Variable;
      spot.power = 1.0f * flockToPossessionPlayerWeight;
      spot.scale = 16.0f * webScale;
      spot.exp = 1.0f;
      forceField.push_back(spot);
    }
  }

  // 合力计算：当前位置 + 力场合力移动量
  Vector3 forceFieldPosition = currentPos + AI_GetForceFieldMovement(forceField, currentPos, 7);

  // 越位线约束：确保不越位（留0.08m余量）
  float margin = 0.08f;
  if (forceNoOffside)
    if (forceFieldPosition.coords[0] * -team->GetDynamicSide() >
        (offsideX * -team->GetDynamicSide()) - margin)
      forceFieldPosition.coords[0] =
          offsideX - (margin * -team->GetDynamicSide());

  // 边界约束：不能出界
  forceFieldPosition.coords[0] = clamp(forceFieldPosition.coords[0], -pitchHalfW, pitchHalfW);
  forceFieldPosition.coords[1] = clamp(forceFieldPosition.coords[1], -pitchHalfH, pitchHalfH);

  return forceFieldPosition;
}

void ElizaController::Reset() {
  DO_VALIDATION;
  lastDesiredDirection = Vector3(0);
  lastDesiredVelocity = 0;
}

void ElizaController::ProcessState(EnvState *state) {
  DO_VALIDATION;
  ProcessPlayerController(state);
  goalieStrategy.ProcessState(state);
  state->process(lastDesiredDirection);
  state->process(lastDesiredVelocity);
}

// ============================================================
// GetOnTheBallCommands —— 控球球员的传/射/带球决策
//
// 决策流程：
//   Step 1: 计算自身战术评分（向前空间、空间、向前性）
//   Step 2: 遍历所有队友，找战术评分更好且传球成功率高的人
//   Step 3: 若在己方危险区域且没有好选择 → 慌乱解围（panic pass）
//   Step 4: 若找到合适传球目标 → 传球
//   Step 5: 若在射门区域且射门成功率足够 → 射门
//   Step 6: 带球移动（占用rawInputDirection/rawInputVelocityFloat）
// ============================================================
void ElizaController::GetOnTheBallCommands(
    std::vector<PlayerCommand> &commandQueue, Vector3 &rawInputDirection,
    float &rawInputVelocityFloat) {
  DO_VALIDATION;
  auto _mentalImage = match->GetMentalImage(_mentalImageTime);
  // 一脚触球难度：球速与球员速度差越大、技术越差，一触难度越高
  float oneTouchIsHard = 0.0f;
  float movementDiff = NormalizedClamp((match->GetBall()->GetMovement() - CastPlayer()->GetMovement()).GetLength(), 0.0f, 10.0f);
  oneTouchIsHard = movementDiff - CastPlayer()->GetStat(technical_shortpass) * movementDiff * 0.8f;

  // 对方球员的心理快照位置（用于评估传球成功率）
  auto opponentPlayerImages = _mentalImage->GetTeamPlayerImages(abs(team->GetID() - 1));


  // ---- Step 1: 当前局势评估 ----

  // 持球时长因子：持球越久越急于传球（指数增长）
  float longPossessionFactor = pow(NormalizedClamp(CastPlayer()->GetPossessionDuration_ms(), 0, 5000), 2.0f);

  // 战术评分权重（第一轮筛选：目标必须比自己强）
  float forwardSpaceWeight = 0.4f; // 向前有空间的权重
  float spaceWeight = 0.3f;        // 整体空间的权重
  float forwardWeight = 2.0f + AI_GetMindSet(CastPlayer()->GetDynamicFormationEntry().role) * 6.0f; // 进攻性越强越重视向前传

  float totalWeight1 = forwardSpaceWeight + spaceWeight + forwardWeight;
  // 战术改善阈值：目标队友必须比自己高这么多才传（防守型球员要求更高）
  float tacticalImprovementThreshold = 0.06f * (1.0f - AI_GetMindSet(CastPlayer()->GetDynamicFormationEntry().role));

  // 传球总评分权重（第二轮筛选：战术差距 × 权重 + 传球成功率 × 权重）
  float tacticalDiffWeight =
      1.0f +
      std::pow(AI_GetMindSet(CastPlayer()->GetDynamicFormationEntry().role),
               2.0f) *
          10.0f; // 进攻型球员：战术差距权重高（宁可冒险传好位置）
  float passWeight = 1.0f;
  // 传球成功率最低阈值（持球越久越愿意接受低成功率传球）
  float passMinimum = 0.2f * (1.0f - AI_GetMindSet(CastPlayer()->GetDynamicFormationEntry().role)) - longPossessionFactor * 0.1f;

  float totalWeight2 = tacticalDiffWeight + passWeight;

  // 传球总评分必须超过此阈值才会传球（持球越久阈值越低）
  float passThreshold = 0.1f - longPossessionFactor * 0.05f;

  // 自身战术评分
  const TacticalPlayerSituation &sit = CastPlayer()->GetTacticalSituation();
  float tacticalRating = sit.forwardSpaceRating * forwardSpaceWeight +
                         sit.spaceRating * spaceWeight +
                         sit.forwardRating * forwardWeight;
  tacticalRating /= totalWeight1;

  // ---- Step 2: 遍历队友，找最佳传球目标 ----
  std::vector<Player*> mates;
  team->GetActivePlayers(mates);

  struct MateRating {
    Player *player;
    float tacticalRating = 0.0f;
    float tacticalDiffRating = 0.0f; // 相对自身的战术改善程度
    float passRating = 0.0f;          // 传球成功率
    float proximityRating = 0.0f;
    e_FunctionType passType;          // 最优传球类型（短/长/高）
  };

  float bestTotalRating = 0.0f;
  MateRating bestMateRating;
  TacticalPlayerSituation bestMateSit;
  bestMateRating.player = 0;
  bestMateRating.passRating = 0.0f;
  bestMateRating.passType = e_FunctionType_ShortPass;
  for (unsigned int i = 0; i < mates.size(); i++) {
    DO_VALIDATION;

    if (mates[i] != CastPlayer()) {
      DO_VALIDATION;

      const TacticalPlayerSituation &mateSit = mates[i]->GetTacticalSituation();

      float mateTacticalRating = mateSit.forwardSpaceRating * forwardSpaceWeight +
                                 mateSit.spaceRating * spaceWeight +
                                 mateSit.forwardRating * forwardWeight;

      mateTacticalRating /= totalWeight1;
      if (mates[i]->GetFormationEntry().role == e_PlayerRole_GK) mateTacticalRating *= 0.7f; // 不喜欢回传门将

      MateRating mateRating;
      mateRating.player = mates[i];
      mateRating.tacticalRating = mateTacticalRating;

      // 只考虑战术评分比自己高的队友
      if (mateTacticalRating > tacticalRating + tacticalImprovementThreshold) {
        DO_VALIDATION;

        float tacticalDiffRating = mateRating.tacticalRating - tacticalRating;

        mateRating.tacticalDiffRating = tacticalDiffRating;
        // 分别计算短传/长传/高球的传球成功率，选最高的
        float passingOddsShort = _GetPassingOdds(mates[i], e_FunctionType_ShortPass, opponentPlayerImages);
        float passingOddsLong  = _GetPassingOdds(mates[i], e_FunctionType_LongPass,  opponentPlayerImages);
        float passingOddsHigh  = _GetPassingOdds(mates[i], e_FunctionType_HighPass,  opponentPlayerImages);
        if (passingOddsShort >= passingOddsLong &&
            passingOddsShort >= passingOddsHigh) {
          DO_VALIDATION;
          mateRating.passRating = passingOddsShort;
          mateRating.passType = e_FunctionType_ShortPass;
        } else if (passingOddsLong >= passingOddsHigh) {
          DO_VALIDATION;
          mateRating.passRating = passingOddsLong;
          mateRating.passType = e_FunctionType_LongPass;
        } else {
          mateRating.passRating = passingOddsHigh;
          mateRating.passType = e_FunctionType_HighPass;
        }

        // 综合评分 = 战术改善贡献 + 传球成功率贡献 - 一触难度惩罚
        float totalRating = mateRating.tacticalDiffRating * tacticalDiffWeight +
                            mateRating.passRating * passWeight -
                            oneTouchIsHard;
        totalRating /= totalWeight2;

        // 只有超过阈值且传球成功率足够才记录为候选
        if (totalRating > bestTotalRating && totalRating > passThreshold &&
            mateRating.passRating > passMinimum) {
          DO_VALIDATION;
          bestTotalRating = totalRating;
          bestMateRating = mateRating;
          bestMateSit = mateSit;
        }
      }

    }  // !self
  }

  // ---- Step 3: 慌乱解围（后卫/门将在危险区域时） ----
  float mindSet = AI_GetMindSet(CastPlayer()->GetDynamicFormationEntry().role);
  if (mindSet < 0.25f) { // 防守型球员（后卫/守门员）
    DO_VALIDATION;
    float panicProneness = 1.0f - mindSet * 2.0f; // 防守越纯，越容易慌乱
    // 离己方球门越近，越慌乱
    float goalCloseness =
        1.0f -
        NormalizedClamp(
            (CastPlayer()->GetPosition() -
             Vector3(pitchHalfW * CastPlayer()->GetTeam()->GetDynamicSide(), 0,
                     0))
                .GetLength(),
            2.0f, 16.0f);
    if (CastPlayer()->GetDynamicFormationEntry().role != e_PlayerRole_GK) {
      DO_VALIDATION;
      // 没有好的传球目标 或 控球不稳 → 慌乱解围
      if ((bestMateRating.player == 0 ||
           bestMateRating.passRating < panicProneness * goalCloseness) &&
          possessionAmount < 0.9f + panicProneness * goalCloseness * 0.8f) {
        DO_VALIDATION;
        _AddPanicPass(commandQueue);
      }
    } else {  // 门将：控球量低于3.0时随时触发慌乱解围
      if (possessionAmount < 3.0f) {
        DO_VALIDATION;
        _AddPanicPass(commandQueue);
      }
    }
  }

  // ---- Step 4: 执行传球 ----
  if (bestMateRating.player != 0) {
    DO_VALIDATION;
    _AddPass(commandQueue, bestMateRating.player, bestMateRating.passType);
  }

  // ---- Step 5: 射门判断 ----
  // 到对方球门的归一化距离（0=紧贴球门，1=32米外）
  float goalDist =
      NormalizedClamp((Vector3(pitchHalfW * -team->GetDynamicSide(), 0, 0) -
                       player->GetPosition())
                          .GetLength(),
                      0.0f, 32.0f);
  // 理想射门位置因子：距离球门7米以内为最佳射门区
  float idealShotPosFactor =
      1.0f - NormalizedClamp(
                 (Vector3((pitchHalfW - 7.0f) * -team->GetDynamicSide(), 0, 0) -
                  player->GetPosition())
                     .GetLength(),
                 0.0f, 16.0f);
  idealShotPosFactor = curve(idealShotPosFactor, 1.0f);
  if (idealShotPosFactor > 0.1f) {
    DO_VALIDATION;
    // 分别计算球门左中右三点的射门成功率（球速倍率3.0）
    float odds1 = _GetPassingOdds(
        Vector3((pitchHalfW + 1.0f) * -team->GetDynamicSide(), -3.6f, 0),
        e_FunctionType_Shot, opponentPlayerImages, 3.0f);
    float odds2 = _GetPassingOdds(
        Vector3((pitchHalfW + 1.0f) * -team->GetDynamicSide(), 0.0f, 0),
        e_FunctionType_Shot, opponentPlayerImages, 3.0f);
    float odds3 = _GetPassingOdds(
        Vector3((pitchHalfW + 1.0f) * -team->GetDynamicSide(), 3.6f, 0),
        e_FunctionType_Shot, opponentPlayerImages, 3.0f);
    // 选择成功率最高的射门角度
    float odds = odds2; float y = 0.0f;
    if (odds1 > odds) {
      DO_VALIDATION;
      odds = odds1;
      y = -3.5f;
    }
    if (odds3 > odds) {
      DO_VALIDATION;
      odds = odds3;
      y = 3.5f;
    }

    odds = std::pow(odds, 0.5f); // 平方根：让低odds也有一定机会

    // 加随机因子模拟球员判断失误；超过0.5才真正射门
    if (odds + boostrandom(0.0f, 0.5f) > 0.5f) {
      DO_VALIDATION;
      PlayerCommand command;
      command.desiredFunctionType = e_FunctionType_Shot;
      command.useDesiredMovement = false;
      command.useDesiredLookAt = false;
      command.desiredVelocityFloat = rawInputVelocityFloat;
      // 射门方向：球门目标点 + 技术属性决定的随机偏差（技术差偏差大）
      command.touchInfo.desiredDirection =
          (Vector3((pitchHalfW + 1.0f) * -team->GetDynamicSide(),
                   y + boostrandom(-1.0f + player->GetStat(technical_shot),
                                   1.0f - player->GetStat(technical_shot)),
                   0) -
           (CastPlayer()->GetPosition() + CastPlayer()->GetMovement() * 0.2f))
              .GetNormalized(Vector3(-team->GetDynamicSide(), 0, 0));
      // 混合球员移动方向（模拟带速度射门时方向的微小偏移）
      command.touchInfo.desiredDirection = (command.touchInfo.desiredDirection * 0.7f + -CastPlayer()->GetDirectionVec() * (CastPlayer()->GetFloatVelocity() / sprintVelocity) * 0.3f).GetNormalized();
      command.touchInfo.autoDirectionBias = 1.0f;
      // 射门力度随距离调整：越远越需要大力
      command.touchInfo.desiredPower = boostrandom(
          0.7f * (0.6f + goalDist * 0.4f), 1.0f * (0.6f + goalDist * 0.4f));
      commandQueue.push_back(command);
    }
  }

  // ---- Step 6: 带球移动（通过rawInputDirection/rawInputVelocityFloat输出） ----
  e_Velocity enumVelocity = e_Velocity_Idle;
  AI_GetBestDribbleMovement(match, player, _mentalImage,
                            rawInputDirection, rawInputVelocityFloat,
                            team->GetTeamData()->GetTactics());
}

void ElizaController::_AddPass(std::vector<PlayerCommand> &commandQueue,
                               Player *target, e_FunctionType passType) {
  DO_VALIDATION;
  PlayerCommand command;
  command.desiredFunctionType = passType;
  command.useDesiredMovement = false;
  command.useDesiredLookAt = false;
  command.touchInfo.targetPlayer = 0;
  command.touchInfo.forcedTargetPlayer = target;
  command.touchInfo.inputDirection = Vector3(0);
  command.touchInfo.inputPower = 0;
  command.touchInfo.autoDirectionBias = 1.0f;
  command.touchInfo.autoPowerBias = 1.0f;
  AI_GetPass(CastPlayer(), passType, command.touchInfo.inputDirection, command.touchInfo.inputPower, command.touchInfo.autoDirectionBias, command.touchInfo.autoPowerBias, command.touchInfo.desiredDirection, command.touchInfo.desiredPower, command.touchInfo.targetPlayer, command.touchInfo.forcedTargetPlayer);
  commandQueue.push_back(command);
}

void ElizaController::_AddPanicPass(std::vector<PlayerCommand> &commandQueue) {
  DO_VALIDATION;

  int yside = signSide(player->GetDirectionVec().coords[1]); // > 0 ? 1 : -1;
  Vector3 sensibleAwayDir =
      ((player->GetDirectionVec() * Vector3(0.8f, 1.0f, 0.0f)).GetNormalized() +
       Vector3(-team->GetDynamicSide() * 0.7f, yside * 0.5f, 0))
          .GetNormalized(0) +
      Vector3(0, 0, 0.3f);
  sensibleAwayDir.Normalize(player->GetDirectionVec());

  PlayerCommand command;
  command.useDesiredMovement = false;
  command.useDesiredLookAt = false;

  command.touchInfo.inputDirection = sensibleAwayDir;
  command.touchInfo.desiredDirection = sensibleAwayDir;
  command.touchInfo.autoDirectionBias = 0.0f;
  command.touchInfo.autoPowerBias = 0.0f;

  command.desiredFunctionType = e_FunctionType_HighPass;
  command.touchInfo.inputPower = 0.7f;
  command.touchInfo.desiredPower = 0.7f;
  commandQueue.push_back(command);

  command.desiredFunctionType = e_FunctionType_Shot;
  command.touchInfo.inputPower = 0.6f;
  command.touchInfo.desiredPower = 0.6f;
  commandQueue.push_back(command);

  command.desiredFunctionType = e_FunctionType_LongPass;
  command.touchInfo.inputPower = 0.8f;
  command.touchInfo.desiredPower = 0.8f;
  commandQueue.push_back(command);
}

// ============================================================
// _GetPassingOdds（重载1）—— 以目标球员为目标计算传球成功率
// 考虑目标球员的预测位置（减速时间）和长传的向前偏置
// ============================================================
float ElizaController::_GetPassingOdds(
    Player *targetPlayer, e_FunctionType passType,
    const std::vector<PlayerImagePosition> &opponentPlayerImages,
    float ballVelocityMultiplier) {
  DO_VALIDATION;

  float initialTargetDistance = (targetPlayer->GetPosition() - player->GetPosition()).GetLength();
  // 高球传球距离不足10米无意义
  if (passType == e_FunctionType_HighPass && initialTargetDistance < 10.0f) return 0.0f;
  // 估算球到达目标的时间
  float estimatedTime_sec = 0.7f + initialTargetDistance * 0.03f;

  // 目标位置 = 队友当前位置 + 刹车时间内的移动量
  Vector3 target = targetPlayer->GetPosition() + targetPlayer->GetMovement() * clamp(estimatedTime_sec, 0.0f, 0.5f);
  if (passType == e_FunctionType_LongPass)
    // 长传目标前置：球员跑到接球点时已经向前跑了一段
    target +=
        Vector3(-team->GetDynamicSide() * initialTargetDistance * 0.2f, 0, 0);

  return _GetPassingOdds(target, passType, opponentPlayerImages, ballVelocityMultiplier);
}

// ============================================================
// _GetPassingOdds（重载2）—— 以空间位置为目标计算传球成功率
//
// 算法：在传球路线上画一条线，对每个对手：
//   1. 计算对手到这条线的最近点（截球点u）和距离
//   2. 计算对手跑到截球点的时间 vs 球到截球点的时间
//   3. 若对手能比球先到（或差不多），则增加危险值
// odds = 1 - 归一化危险值
// ============================================================
float ElizaController::_GetPassingOdds(
    const Vector3 &target, e_FunctionType passType,
    const std::vector<PlayerImagePosition> &opponentPlayerImages,
    float ballVelocityMultiplier) {
  DO_VALIDATION;

  float secondScale = 1.0f; // 危险度测量的时间窗口（秒）

  // 起点：发球者预测位置（0.12秒后）
  Vector3 origin = player->GetPosition() + player->GetMovement() * 0.12f;

  // 传球路线（从发球者到目标）
  Line line(origin, target);

  float danger = 0.0f;
  for (unsigned int opp = 0; opp < opponentPlayerImages.size(); opp++) {
    DO_VALIDATION;
    // 对手预测位置（0.2秒后，考虑刹车）
    Vector3 oppPos = opponentPlayerImages.at(opp).position + opponentPlayerImages.at(opp).movement * 0.2f;
    float u = 0.0f; // 对手在传球线上的最近点比例（0=发球端，1=接球端）
    float oppDistance = 0.0f;
    oppDistance = line.GetDistanceToPoint(oppPos, u);

    // 只考虑在传球路线范围内的对手（稍微延伸到1.2以防止接球点被忽略）
    if (u >= 0.0f && u <= 1.0f + 0.2f) {
      DO_VALIDATION;
      // 高球：路线的中间段（0.2~0.65）是低空飞行段，对手较难截；
      // 起点和终点段正常计算
      if ((passType == e_FunctionType_HighPass && (u < 0.2f || u > 0.65f)) ||
          passType != e_FunctionType_HighPass) {
        DO_VALIDATION;
        float clampedU = clamp(u, 0.0f, 1.0f);
        // 截球点（对手最可能截到球的位置）
        Vector3 intersect = origin * (1.0f - clampedU) + target * clampedU;

        // 对手跑到截球点的时间（距离+1加速修正）/全速
        float oppToIntersect_sec = (oppDistance + 1.0f) / sprintVelocity;

        Vector3 originToBallPos = (intersect - origin);
        // 高球接球需要额外时间停球（落点后半段才算）
        float penaltyTime = (passType == e_FunctionType_HighPass && u > 0.5f) ? 2.5f : 0.0f;
        float ballToIntersect_sec = 0.7f + originToBallPos.GetLength() * u * 0.03f + penaltyTime;
        ballToIntersect_sec *= 1.0f / ballVelocityMultiplier;

        // 时间差→危险度：球比对手先到0.5秒以上才是安全的
        danger += clamp(ballToIntersect_sec - oppToIntersect_sec + (secondScale * 0.5f), 0.0f, secondScale);
      }
    }
  }

  // 高球天然有额外危险（比地面传球更难控制）
  if (passType == e_FunctionType_HighPass) danger += 0.4f;

  // 归一化：1个极其危险的对手 ≈ 100%危险
  danger = NormalizedClamp(danger, 0.0f, secondScale);
  float odds = 1.0f - danger;

  return odds;
}

void ElizaController::_AddCelebration(
    std::vector<PlayerCommand> &commandQueue) {
  DO_VALIDATION;

  signed int xSide = (match->GetBall()->Predict(0).Get2D().coords[0] > 0) ? 1 : -1;
  signed int ySide = team->GetDynamicSide();
  if (team->GetLastTouchPlayer()) ySide = (team->GetLastTouchPlayer()->GetPosition().coords[1] > 0) ? 1 : -1;
  Vector3 celebrationPosition = Vector3(pitchHalfW * xSide, pitchHalfH * ySide, 0);

  Vector3 desiredDirection = (celebrationPosition - player->GetPosition()).GetNormalized();
  float desiredVelocityFloat = ClampVelocity((celebrationPosition - player->GetPosition()).GetLength() / 4.0f);
  Vector3 desiredLookAt = player->GetPosition() + player->GetDirectionVec() * 1000;

  int celebrationType = 1;
  if (match->GetLastGoalTeam() != team) {
    DO_VALIDATION;
    celebrationType = 2;
    desiredVelocityFloat = idleVelocity;
  } else {
    celebrationType = 1;
  }

  int madeGoal = 1;
  if (celebrationType == 1) {
    DO_VALIDATION;
    if (team->GetLastTouchPlayer() == player) {
      DO_VALIDATION;
      madeGoal = 2;
      desiredVelocityFloat = sprintVelocity;
    } else {
      madeGoal = 1;
      if (team->GetLastTouchPlayer() != 0) {
        DO_VALIDATION;
        if ((team->GetLastTouchPlayer()->GetPosition() - player->GetPosition())
                .GetLength() < 20) {
          DO_VALIDATION;
          celebrationPosition = team->GetLastTouchPlayer()->GetPosition();
          desiredDirection = ((team->GetLastTouchPlayer()->GetPosition() * 0.5 + celebrationPosition * 0.5) - player->GetPosition()).GetNormalized();
          desiredVelocityFloat = ClampVelocity((team->GetLastTouchPlayer()->GetPosition() - player->GetPosition()).GetLength() / 2.0);
        } else {
          desiredVelocityFloat = idleVelocity;
        }
        desiredLookAt = player->GetPosition() + team->GetLastTouchPlayer()->GetDirectionVec() * 100;
      }
    }
  }

  // celebration

  if (match->GetActualTime_ms() - match->GetReferee()->GetBuffer().stopTime >
          2000 &&
      match->GetActualTime_ms() - match->GetReferee()->GetBuffer().stopTime <
          4000) {
    DO_VALIDATION;
    PlayerCommand command;
    command.desiredFunctionType = e_FunctionType_Special;
    command.useSpecialVar1 = true;
    command.specialVar1 = celebrationType;
    command.useSpecialVar2 = true;
    command.specialVar2 = madeGoal;
    command.useDesiredMovement = false;
    command.useDesiredLookAt = false;
    commandQueue.push_back(command);
  }

  // movement

  {
    PlayerCommand command;
    command.desiredFunctionType = e_FunctionType_Movement;
    command.useDesiredMovement = true;
    command.desiredDirection = desiredDirection;
    command.desiredVelocityFloat = desiredVelocityFloat;
    command.useDesiredLookAt = true;
    command.desiredLookAt = desiredLookAt;
    commandQueue.push_back(command);
  }
}
