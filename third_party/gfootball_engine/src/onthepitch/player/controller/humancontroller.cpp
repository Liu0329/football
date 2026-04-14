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

#include "humancontroller.hpp"
#include <math.h>
#include <cmath>

#include "../../AIsupport/AIfunctions.hpp"

#include "../../../main.hpp"

HumanController::HumanController(Match *match, AIControlledKeyboard *hid)
    : PlayerController(match), hid(hid) {
  DO_VALIDATION;
  Reset();
}

HumanController::~HumanController() { DO_VALIDATION; }

void HumanController::SetPlayer(PlayerBase *player) {
  DO_VALIDATION;
  lastSwitchTime_ms = match->GetActualTime_ms();

  PlayerController::SetPlayer(player);
}

void HumanController::RequestCommand(PlayerCommandQueue &commandQueue) {
  DO_VALIDATION;
  auto _mentalImage = match->GetMentalImage(_mentalImageTime);
  CastPlayer()->SetDesiredTimeToBall_ms(0);

  _Preprocess(); // calculate some variables


  // human input

  Vector3 rawInputDirection;
  float rawInputVelocityFloat = 0;
  _GetHidInput(rawInputDirection, rawInputVelocityFloat);
  _SetInput(rawInputDirection, rawInputVelocityFloat);


  // 清除动作缓冲区：若当前动画已是对应动作（短传/长传/高球/射门）且触球已完成，则重置状态
  e_FunctionType functionType = CastPlayer()->GetCurrentFunctionType();
  if (actionMode == 2 &&
      (functionType == e_FunctionType_ShortPass ||
       functionType == e_FunctionType_LongPass ||
       functionType == e_FunctionType_HighPass ||
       functionType == e_FunctionType_Shot) &&
      !CastPlayer()->TouchPending()) {
    DO_VALIDATION;
    actionMode = 0;
    gauge_ms = 0;
    actionBufferTime_ms = 0;
    shotPressStartTime_ms = -1;
    shotQueuedGauge_ms = -1;
    passQueuedGauge_ms = -1;
  }

  if (actionMode == 1 &&
      (functionType == e_FunctionType_Sliding ||
       functionType == e_FunctionType_Interfere) &&
      !CastPlayer()->TouchPending()) {
    DO_VALIDATION;
    actionMode = 0;
    gauge_ms = 0;
    actionBufferTime_ms = 0;
  }

  // 取消操作：按下短传键可取消射门或高球的蓄力
  // shot cancel
  if (actionMode == 2 && actionButton == e_ButtonFunction_Shot &&
      hid->GetButton(e_ButtonFunction_ShortPass) && !match->IsInSetPiece()) {
    DO_VALIDATION;
    actionMode = 0;
    gauge_ms = 0;
    actionBufferTime_ms = 0;
    shotPressStartTime_ms = -1;
    shotQueuedGauge_ms = -1;
    passQueuedGauge_ms = -1;
  }

  // high pass cancel
  if (actionMode == 2 && actionButton == e_ButtonFunction_HighPass &&
      hid->GetButton(e_ButtonFunction_ShortPass) && !match->IsInSetPiece()) {
    DO_VALIDATION;
    actionMode = 0;
    gauge_ms = 0;
    actionBufferTime_ms = 0;
  }

  // 动作缓冲超时（2秒）或球越来越远时取消
  if (actionMode == 2 && !match->IsInSetPiece() &&
      (actionBufferTime_ms > 2000 ||
       CastPlayer()->GetTimeNeededToGetToBall_ms() >
           CastPlayer()->GetTimeNeededToGetToBall_previous_ms() + 700 ||
       (CastPlayer()->GetCurrentFunctionType() == e_FunctionType_Interfere))) {
    DO_VALIDATION;
    actionMode = 0;
    gauge_ms = 0;
    actionBufferTime_ms = 0;
  }

  // 防守类动作（逼抢等）缓冲超时（1秒）取消
  if (actionMode == 1 && actionBufferTime_ms > 1000) {
    DO_VALIDATION;
    actionMode = 0;
    gauge_ms = 0;
    actionBufferTime_ms = 0;
  }

  // 执行缓冲中的动作：
  // 满足以下任一条件时触发动作：
  //   1. 按键已松开
  //   2. 蓄力超过500ms（提前触发，动画还有时间追上触球）
  //   3. 没有球权且等待时间已 > 0（非定位球情况）
  if (actionMode == 2) {
    DO_VALIDATION;

    // 用时间戳计算实际按压时长，避免依赖 gauge_ms（每帧被 ResetNotSticky 清除）
    int currentEffectiveGauge_ms = gauge_ms;
    if (shotPressStartTime_ms >= 0) {
      int elapsed = match->GetActualTime_ms() - shotPressStartTime_ms;
      currentEffectiveGauge_ms = clamp(elapsed, 10, 1000);
    }
    // 射门：500ms 自动触发（留时间给动画，不必等到松键）
    // 传球（短传/长传/高传）：只在松键或满蓄1000ms时触发，玩家完全控制时机
    int autoTriggerThreshold_ms = (actionButton == e_ButtonFunction_Shot) ? 500 : 1000;
    if (!hid->GetButton(actionButton) ||
        (hid->GetButton(actionButton) && currentEffectiveGauge_ms >= autoTriggerThreshold_ms) ||
        (!CastPlayer()->HasPossession() && !match->IsInSetPiece() &&
         actionBufferTime_ms > 0)) {
      DO_VALIDATION;

      // 用时间戳计算真实按压时长（跨环境步，不受 ResetNotSticky 影响）
      // 传球满蓄上限 1000ms；短按（~100ms）→ 约 4%；长按（~1000ms）→ 100%
      int baseTime_ms = 60;
      int effectivePress_ms = gauge_ms;
      if (shotPressStartTime_ms >= 0) {
        int elapsed = match->GetActualTime_ms() - shotPressStartTime_ms;
        effectivePress_ms = clamp(elapsed, 10, 1000);
      }
      float gaugeFactor = (effectivePress_ms - baseTime_ms) * (1.0f / float(1000 - baseTime_ms));
      gaugeFactor = clamp(gaugeFactor, 0.0f, 1.0f);

      // action button released!

      // force set piece methods
      if (match->IsInSetPiece() &&
          team->GetController()->GetPieceTaker() == player &&
          team->GetController()->GetSetPieceType() == e_GameMode_KickOff) {
        DO_VALIDATION;

        PlayerCommand command;

        command.desiredFunctionType = e_FunctionType_ShortPass;
        command.touchInfo.autoDirectionBias = 1.0f;
        command.touchInfo.autoPowerBias = 1.0f;
        command.touchInfo.inputDirection = player->GetDirectionVec(); // dud
        command.touchInfo.inputPower = 0.1f; // dud

        Vector3 desiredTargetPosition = player->GetPosition() + player->GetDirectionVec() * 1.0f;
        command.touchInfo.forcedTargetPlayer = AI_GetClosestPlayer(team, desiredTargetPosition, false, CastPlayer());

        AI_GetPass(CastPlayer(), command.desiredFunctionType, command.touchInfo.inputDirection, command.touchInfo.inputPower, command.touchInfo.autoDirectionBias, command.touchInfo.autoPowerBias, command.touchInfo.desiredDirection, command.touchInfo.desiredPower, command.touchInfo.targetPlayer, command.touchInfo.forcedTargetPlayer);

        commandQueue.push_back(command);

      } else if (actionButton == e_ButtonFunction_HighPass) {
        DO_VALIDATION;

        // ===== 高传：与射门完全一致的逻辑 =====
        // gaugeFactor [0,1] 线性映射到目标距离 [6m, 100m]
        // 完全绕过 AI_GetPass 的队友查找和方向修正，直接调用 AI_GetAutoPass：
        //   - 方向：完全等于玩家输入方向，右就是右，不找任何队友
        //   - 弧度：由 AI_GetAutoPass 根据距离自动计算高球弧线 heightOffset
        //   - 球速：由 AI_GetAutoPass 根据距离计算，与玩家 gaugeFactor 完全正比
        const float highPassMinDist = 6.0f;
        const float highPassMaxDist = 100.0f;
        float targetDist = highPassMinDist + gaugeFactor * (highPassMaxDist - highPassMinDist);
        // targetVec = 玩家输入方向 × 目标距离，完全无 AI 修正
        Vector3 targetVec = inputDirection * targetDist;

        PlayerCommand command;
        command.desiredFunctionType = e_FunctionType_HighPass;
        command.useDesiredMovement = false;
        command.useDesiredLookAt = false;
        command.touchInfo.inputDirection = inputDirection;
        command.touchInfo.inputPower = clamp(targetDist / 60.0f, 0.01f, 2.0f);
        command.touchInfo.targetPlayer = nullptr;
        // 直接从目标向量计算弧线方向和球速，不经过任何 AI 队友选择
        AI_GetAutoPass(e_FunctionType_HighPass, targetVec,
                       command.touchInfo.desiredDirection, command.touchInfo.desiredPower);

        printf("[HighPass] gaugeFactor=%.3f targetDist=%.1fm desiredPower=%.3f\n",
               gaugeFactor, targetDist, command.touchInfo.desiredPower);

        passQueuedGauge_ms = effectivePress_ms;
        commandQueue.push_back(command);

      } else if (actionButton == e_ButtonFunction_ShortPass ||
                 actionButton == e_ButtonFunction_LongPass) {
        DO_VALIDATION;

        // ===== 短传/长传：与高传完全相同的纯手动逻辑 =====
        // 完全绕过 AI_GetPass，直接调用 AI_GetAutoPass：
        //   - 方向：100% 等于玩家输入方向，不找任何队友
        //   - 距离：gaugeFactor 线性映射到 [minDist, maxDist]
        //   - 球速：由 AI_GetAutoPass 根据距离计算
        // 短传 (S)：4m ~ 30m，贴地滚动（heightOffset≈0）
        // 长传 (W)：6m ~ 60m，稍微弹起（heightOffset=0.11）
        {
          float minDist, maxDist;
          e_FunctionType funcType;
          if (actionButton == e_ButtonFunction_ShortPass) {
            minDist  = 4.0f;
            maxDist  = 30.0f;
            funcType = e_FunctionType_ShortPass;
          } else {
            minDist  = 6.0f;
            maxDist  = 60.0f;
            funcType = e_FunctionType_LongPass;
          }

          float targetDist = minDist + gaugeFactor * (maxDist - minDist);
          Vector3 targetVec = inputDirection * targetDist;

          PlayerCommand command;
          command.desiredFunctionType = funcType;
          command.useDesiredMovement = false;
          command.useDesiredLookAt = false;
          command.touchInfo.inputDirection = inputDirection;
          command.touchInfo.inputPower = clamp(targetDist / 60.0f, 0.01f, 2.0f);
          command.touchInfo.targetPlayer = nullptr;
          AI_GetAutoPass(funcType, targetVec,
                         command.touchInfo.desiredDirection, command.touchInfo.desiredPower);

          printf("[%s] gaugeFactor=%.3f targetDist=%.1fm desiredPower=%.3f\n",
                 (actionButton == e_ButtonFunction_ShortPass) ? "ShortPass" : "LongPass",
                 gaugeFactor, targetDist, command.touchInfo.desiredPower);

          passQueuedGauge_ms = effectivePress_ms;
          commandQueue.push_back(command);
        }

      } else if (actionButton == e_ButtonFunction_Shot) {
        DO_VALIDATION;

        // ===== 射门逻辑 =====
        // 用时间戳计算真实按压时长，避免 ResetNotSticky() 每帧清除按键导致 gauge_ms 永远很小
        // shotPressStartTime_ms 在 Process() 里按键首次按下时记录，跨环境步不丢失
        // 按压时长上限 500ms（满蓄），归一化到 [0,1]
        int shotBaseTime_ms = 60;
        int shotMaxTime_ms = 500;
        int effectiveGauge_ms = gauge_ms;
        if (shotPressStartTime_ms >= 0) {
          int elapsed = match->GetActualTime_ms() - shotPressStartTime_ms;
          effectiveGauge_ms = clamp(elapsed, 10, 1000);
        }
        float shotGaugeFactor = (effectiveGauge_ms - shotBaseTime_ms) *
                                (1.0f / float(shotMaxTime_ms - shotBaseTime_ms));
        shotGaugeFactor = clamp(shotGaugeFactor, 0.0f, 1.0f);

        PlayerCommand command;
        command.desiredFunctionType = e_FunctionType_Shot;
        command.useDesiredMovement = false;
        command.useDesiredLookAt = false;
        command.desiredVelocityFloat = inputVelocityFloat; // this is so we can use sprint/dribble buttons as shot modifiers
        command.touchInfo.inputDirection = inputDirection;
        command.touchInfo.autoDirectionBias = GetConfiguration()->GetReal("gameplay_shot_autodirection", _default_Shot_AutoDirection);
        command.touchInfo.autoDirectionBias = 1.0f;
        command.touchInfo.desiredDirection = AI_GetShotDirection(CastPlayer(), command.touchInfo.inputDirection, command.touchInfo.autoDirectionBias);
        // pow 指数 2.0：曲线上凸，短按力量极弱，需要长按才能打出强力射门
        // 100ms → dp≈0.008 → ~20 m/s；300ms → dp≈0.30 → ~32 m/s；500ms → dp=1.0 → 60 m/s
        command.touchInfo.desiredPower =
            clamp(std::pow(shotGaugeFactor, 2.0f), 0.01f, 1.0f);

        printf("[SHOT] effectiveGauge_ms=%d  shotGaugeFactor=%.3f  desiredPower=%.3f\n",
               effectiveGauge_ms, shotGaugeFactor, command.touchInfo.desiredPower);

        // 记录推入时的 gauge（UI 冻结用），每次推入都更新
        shotQueuedGauge_ms = effectiveGauge_ms;
        commandQueue.push_back(command);
      }
    }

  } else if (actionMode == 1) {
    DO_VALIDATION;

    if (hid->GetButton(actionButton)) {
      DO_VALIDATION;

      if (actionButton == e_ButtonFunction_Sliding) {
        DO_VALIDATION;

        PlayerCommand command;
        command.desiredFunctionType = e_FunctionType_Sliding;
        command.useDesiredMovement = true;
        command.desiredDirection = inputDirection;
        command.desiredVelocityFloat = inputVelocityFloat;
        command.useDesiredLookAt = true;
        command.desiredLookAt = CastPlayer()->GetPosition() + CastPlayer()->GetMovement() * 0.1f + command.desiredDirection * 10.0f;
        commandQueue.push_back(command);
      }

      if (actionButton == e_ButtonFunction_TeamPressure) {
        DO_VALIDATION;

        team->GetController()->ApplyTeamPressure();
      }

      if (actionButton == e_ButtonFunction_KeeperRush) {
        DO_VALIDATION;

        team->GetController()->ApplyKeeperRush();
      }

    } else {
      // action button released!
      actionMode = 0;
    }
  }

  // set piece?
  if ((match->IsInSetPiece() &&
       team->GetController()->GetPieceTaker() == player &&
       (actionMode != 2 || (actionMode == 2 && hid->GetButton(actionButton)) ||
        match->GetBallRetainer() == player)) ||
      (match->IsInSetPiece() &&
       team->GetController()->GetPieceTaker() != player &&
       match->GetBallRetainer() == 0)) {
    DO_VALIDATION;
    _SetPieceCommand(commandQueue);
    //if (team->GetController()->GetPieceTaker() == player) printf("waiting to take set piece!\n");
    //if (team->GetController()->GetPieceTaker() != player) printf("waiting for teammate to take set piece!\n");
    return;
  }

  // delay direction input until we have chosen a steady direction.
  // this is because humans can only move an analog stick so fast, and we don't want requeues to happen mid-analogstick-movement.
  Vector3 inputDirectionSaveNonsteady = inputDirection;
  //SetGreenDebugPilon(player->GetPosition() + inputDirection * (inputVelocityFloat * 0.5f + 0.5f));
  inputDirection = steadyDirection;
  //SetBlueDebugPilon(player->GetPosition() + inputDirection * (inputVelocityFloat * 0.5f + 0.6f));

  if (match->IsInPlay() && !match->IsInSetPiece()) {
    DO_VALIDATION;

    bool idleTurnToOpponentGoal = false;
    bool knockOn = false;
    if (hid->GetButton(e_ButtonFunction_Dribble)) idleTurnToOpponentGoal = true;
    if (hid->GetButton(e_ButtonFunction_Dribble) && hid->GetButton(e_ButtonFunction_Sprint)) knockOn = true;

    // 蓄力时不限制移动：玩家可以自由移动和改变方向
    Vector3 inputDirectionSave2 = inputDirection;
    float inputVelocitySave2 = inputVelocityFloat;

    // ball control?
    bool keepCurrentBodyDirection = false;
    // sidestep dribble disabled for now, too quirky: if (hid->GetButton(e_ButtonFunction_Dribble)) keepCurrentBodyDirection = true;
    _BallControlCommand(commandQueue, idleTurnToOpponentGoal, knockOn, true, keepCurrentBodyDirection);

    // trap?
    _TrapCommand(commandQueue, idleTurnToOpponentGoal, knockOn);

    // reload original input
    if (actionMode == 2) {
      DO_VALIDATION;
      inputDirection = inputDirectionSave2;
      inputVelocityFloat = inputVelocitySave2;
    }

    // interfere?
    bool byAnyMeans = false;
    if (hid->GetButton(e_ButtonFunction_Pressure)) byAnyMeans = true;
    _InterfereCommand(commandQueue, byAnyMeans);
  }

  // movement
  bool forceMagnet = false;
  bool extraHaste = false;
  if (actionMode != 2 && hid->GetButton(e_ButtonFunction_Pressure)) {
    DO_VALIDATION;
    forceMagnet = true;
    extraHaste = true;
  }
  // 蓄力传球/射门时不启用 forceMagnet，让玩家自由移动不被吸向球
  _MovementCommand(commandQueue, forceMagnet, extraHaste);

  if (commandQueue.size() > 0) {
    DO_VALIDATION;
    PlayerCommand &command = commandQueue.at(commandQueue.size() - 1);
    assert(command.desiredFunctionType == e_FunctionType_Movement); // make sure this is the movement command (is probably guaranteed, check out _MovementCommand)

    if (!match->GetUseMagnet()) {
      DO_VALIDATION;
      // no magnet
      command.desiredDirection = inputDirection;
      command.desiredVelocityFloat = inputVelocityFloat;
      if (command.desiredVelocityFloat < idleDribbleSwitch) command.desiredDirection = (_mentalImage->GetBallPrediction(500).Get2D() - player->GetPosition()).GetNormalized(inputDirection);
      //command.desiredLookAt = CastPlayer()->GetPosition() + inputDirection * 10;
      command.desiredLookAt = _mentalImage->GetBallPrediction(500).Get2D();
    }

    // super cancel
    if (match->IsInPlay() && !match->IsInSetPiece() &&
        hid->GetButton(e_ButtonFunction_Dribble) &&
        hid->GetButton(e_ButtonFunction_Sprint)) {
      DO_VALIDATION;
      if (!hasBestPossession) {
        DO_VALIDATION;
        command.desiredDirection = inputDirection;
        command.desiredVelocityFloat = inputVelocityFloat;
      }
    }
  }

  // reload original input
  inputDirection = inputDirectionSaveNonsteady;
}

void HumanController::Process() {
  DO_VALIDATION;

  // just doesn't work so well (fixes the 'humans can't change stick pos instantly' problem, but introduces too much lag). maybe revisit/update later
  bool enableSteadyDirectionSystem = false;

  PlayerController::Process();
  DO_VALIDATION;

  Vector3 currentDirection;
  float dud = 0.0f;
  _GetHidInput(currentDirection, dud);
  radian angle = fabs(currentDirection.GetAngle2D(previousDirection));
  previousDirection = currentDirection;
  DO_VALIDATION;

  // only set steadydirection if angle is small (= human probably reaching his intended direction)
  // or very large (= maybe the stick has been in deadzone space; humans can't move this fast)
  if (enableSteadyDirectionSystem) {
    DO_VALIDATION;
    if (angle < 0.01f * pi || angle > 0.65f * pi ||
        (match->GetActualTime_ms() - lastSteadyDirectionSnapshotTime_ms) >
            100) {
      DO_VALIDATION;
      steadyDirection = currentDirection;
      lastSteadyDirectionSnapshotTime_ms = match->GetActualTime_ms();
    }
  } else {
    DO_VALIDATION;
    steadyDirection = currentDirection;
    lastSteadyDirectionSnapshotTime_ms = match->GetActualTime_ms();
  }

  _CalculateSituation();

  // action?

  if (actionMode == 0 && (!match->IsInSetPiece() ||
                          team->GetController()->GetPieceTaker() == player)) {
    DO_VALIDATION;



    // what is the context: do we want defend buttons or pass/shot buttons?
    float possessionContext = possessionAmount - 1.0f;
    if (match->GetDesignatedPossessionPlayer() == player) {
      DO_VALIDATION;
      possessionContext = 1.0f; // new (keeper was allowed doing slidings before free kick sometimes lol, sign something was wrong)
    } else {
      // in situations where we aren't the designated player, we sometimes still want to do ball stuff, because we could try to extend our leg to pass, for example
      if (hid->GetButton(e_ButtonFunction_ShortPass)) possessionContext += 0.15f;
      if (hid->GetButton(e_ButtonFunction_LongPass)) possessionContext += 0.15f;
      if (hid->GetButton(e_ButtonFunction_Shot)) possessionContext += 0.15f;

      if (hid->GetButton(e_ButtonFunction_Pressure)) possessionContext -= 0.15f;
      if (hid->GetButton(e_ButtonFunction_Sliding)) possessionContext -= 0.15f;
      if (hid->GetButton(e_ButtonFunction_TeamPressure)) possessionContext -= 0.15f;

      if (match->GetBall()->Predict(0).coords[2] > 1.5f) possessionContext += 0.2f;
    }

    if (possessionContext < 0.0f) {
      DO_VALIDATION;
      bool allowPressure = true;
      bool allowSliding = true;
      bool allowTeamPressure = true;
      bool allowKeeperRush = true;

      if (match->IsInSetPiece()) {
        DO_VALIDATION;
        allowPressure = false;
        allowSliding = false;
        allowTeamPressure = false;
        allowKeeperRush = false;
      }

      if (hid->GetButton(e_ButtonFunction_Pressure) &&
          !hid->GetPreviousButtonState(e_ButtonFunction_Pressure) &&
          allowPressure) {
        DO_VALIDATION;
        actionMode = 1;
        actionButton = e_ButtonFunction_Pressure;
      }

      if (hid->GetButton(e_ButtonFunction_Sliding) &&
          !hid->GetPreviousButtonState(e_ButtonFunction_Sliding) &&
          allowSliding) {
        DO_VALIDATION;  // we don't want high passes to turn into slidings
        actionMode = 1;
        actionButton = e_ButtonFunction_Sliding;
      }

      if (hid->GetButton(e_ButtonFunction_TeamPressure) &&
          !hid->GetPreviousButtonState(e_ButtonFunction_TeamPressure) &&
          allowTeamPressure) {
        DO_VALIDATION;
        actionMode = 1;
        actionButton = e_ButtonFunction_TeamPressure;
      }

      if (hid->GetButton(e_ButtonFunction_KeeperRush) &&
          !hid->GetPreviousButtonState(e_ButtonFunction_KeeperRush) &&
          allowKeeperRush) {
        DO_VALIDATION;
        actionMode = 1;
        actionButton = e_ButtonFunction_KeeperRush;
      }

    } else {
      bool allowShortPass = true;
      bool allowLongPass = true;
      bool allowHighPass = true;
      bool allowShot = true;

      if (team->GetController()->GetPieceTaker() == player &&
          team->GetController()->GetSetPieceType() == e_GameMode_ThrowIn) {
        DO_VALIDATION;
        allowHighPass = false;
        allowShot = false;
      }

      if (hid->GetButton(e_ButtonFunction_ShortPass) &&
          !hid->GetPreviousButtonState(e_ButtonFunction_ShortPass) &&
          allowShortPass) {
        DO_VALIDATION;
        actionMode = 2;
        actionButton = e_ButtonFunction_ShortPass;
        shotPressStartTime_ms = match->GetActualTime_ms();
      }

      if (hid->GetButton(e_ButtonFunction_LongPass) &&
          !hid->GetPreviousButtonState(e_ButtonFunction_LongPass) &&
          allowLongPass) {
        DO_VALIDATION;
        actionMode = 2;
        actionButton = e_ButtonFunction_LongPass;
        shotPressStartTime_ms = match->GetActualTime_ms();
      }

      if (hid->GetButton(e_ButtonFunction_HighPass) &&
          !hid->GetPreviousButtonState(e_ButtonFunction_HighPass) &&
          allowHighPass) {
        DO_VALIDATION;
        actionMode = 2;
        actionButton = e_ButtonFunction_HighPass;
        shotPressStartTime_ms = match->GetActualTime_ms();
      }

      if (hid->GetButton(e_ButtonFunction_Shot) &&
          !hid->GetPreviousButtonState(e_ButtonFunction_Shot) && allowShot) {
        DO_VALIDATION;
        actionMode = 2;
        actionButton = e_ButtonFunction_Shot;
        // 记录射门按键按下的游戏时间，用于跨环境步计算实际按压时长
        // ResetNotSticky() 每个环境步结束后会清除按键状态，但我们保留时间戳
        shotPressStartTime_ms = match->GetActualTime_ms();
      }
    }
  }

  if (actionMode == 2) {
    DO_VALIDATION;
    if (hid->GetButton(actionButton)) {
      DO_VALIDATION;
      // 每帧（10ms）增加蓄力时间，上限 1000ms（满力）
      gauge_ms += 10;
      gauge_ms = clamp(gauge_ms, 10, 1000);
      // 射门按键：同步更新基于游戏时间的实际按压时长（覆盖物理步累积值）
      // GetActualTime_ms() 跨越环境步边界不会重置，可以准确计量持续时间
      if (actionButton == e_ButtonFunction_Shot && shotPressStartTime_ms >= 0) {
        int elapsed = match->GetActualTime_ms() - shotPressStartTime_ms;
        gauge_ms = clamp(elapsed, 10, 1000);
      }
      actionBufferTime_ms = 0;
    } else {
      // 按键已松开：保持 actionMode=2 直到缓冲超时，等待执行动作
      actionBufferTime_ms += 10;
    }
  }

  if (hid->GetButton(e_ButtonFunction_Switch) && hasPossession) team->GetController()->ApplyAttackingRun();
}

Vector3 HumanController::GetDirection() {
  DO_VALIDATION;
  Vector3 direction = CastPlayer()->GetDirectionVec();
  return hid->GetDirection().GetNormalized(direction);
}

float HumanController::GetFloatVelocity() {
  DO_VALIDATION;
  Vector3 rawInputDirection;
  float rawInputVelocityFloat = 0;
  _GetHidInput(rawInputDirection, rawInputVelocityFloat);
  return rawInputVelocityFloat;
}

int HumanController::GetReactionTime_ms() {
  DO_VALIDATION;
  return IController::GetReactionTime_ms(); // already have human reaction time to contend with
}

void HumanController::Reset() {
  DO_VALIDATION;
  actionMode = 0;
  gauge_ms = 0;
  actionButton = e_ButtonFunction_ShortPass;
  actionBufferTime_ms = 0;
  shotPressStartTime_ms = -1;
  shotQueuedGauge_ms = -1;
  passQueuedGauge_ms = -1;

  lastSwitchTime_ms = -10000;
  lastSwitchTimeDuration_ms = 300;

  lastSteadyDirectionSnapshotTime_ms = 0;
  steadyDirection = Vector3(0, -1, 0);
  previousDirection = Vector3(0, -1, 0);

  fadingTeamPossessionAmount = 1.0;
  hid->ResetNotSticky();
}

void HumanController::_GetHidInput(Vector3 &rawInputDirection,
                                   float &rawInputVelocityFloat) {
  DO_VALIDATION;
  rawInputDirection = hid->GetDirection();
  if (CastPlayer()->GetPosition().coords[0] > pitchHalfW) {
    DO_VALIDATION;
    rawInputDirection.coords[0] = std::min(0.0f, rawInputDirection.coords[0]);
  }
  if (CastPlayer()->GetPosition().coords[0] < -pitchHalfW) {
    DO_VALIDATION;
    rawInputDirection.coords[0] = std::max(0.0f, rawInputDirection.coords[0]);
  }
  if (CastPlayer()->GetPosition().coords[1] > pitchHalfH) {
    DO_VALIDATION;
    rawInputDirection.coords[1] = std::min(0.0f, rawInputDirection.coords[1]);
  }
  if (CastPlayer()->GetPosition().coords[1] < -pitchHalfH) {
    DO_VALIDATION;
    rawInputDirection.coords[1] = std::max(0.0f, rawInputDirection.coords[1]);
  }
  if (rawInputDirection.GetLength() < analogStickDeadzone) {
    DO_VALIDATION;
    rawInputDirection = CastPlayer()->GetDirectionVec();
    rawInputVelocityFloat = idleVelocity;
  } else {
    DO_VALIDATION;
    if (hid->GetButton(e_ButtonFunction_Sprint)) rawInputVelocityFloat = sprintVelocity;
    else if (hid->GetButton(e_ButtonFunction_Dribble)) rawInputVelocityFloat = dribbleVelocity;
    else if (hid->GetButton(e_ButtonFunction_Switch) && match->GetDesignatedPossessionPlayer() == CastPlayer()) rawInputVelocityFloat = idleVelocity;
    else rawInputVelocityFloat = walkVelocity;
    assert(rawInputDirection.GetLength() > 0.001f);
    rawInputDirection.Normalize(); // hid should do this, but still
  }

  if (GetLastSwitchBias() > 0.0f) {
    DO_VALIDATION;
    float switchInfluence = 0.5f;
    float switchBias = std::pow(GetLastSwitchBias(), 0.7f);
    Vector3 currentMovement = player->GetDirectionVec() * player->GetFloatVelocity();
    Vector3 manualMovement = rawInputDirection * rawInputVelocityFloat;
    Vector3 resultMovement = currentMovement * switchBias * switchInfluence +
                             manualMovement * (1.0f - switchBias * switchInfluence);
    rawInputDirection = resultMovement.GetNormalized(rawInputDirection);
    rawInputVelocityFloat = resultMovement.GetLength();
  }
}
