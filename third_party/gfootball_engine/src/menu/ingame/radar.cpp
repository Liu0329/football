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

#include "radar.hpp"

#include "../../utils/gui2/windowmanager.hpp"

#include "wrap_SDL.h"

#include "../../gamedefines.hpp"

#include "../../onthepitch/match.hpp"

#include <cmath>
#include <algorithm>

namespace blunted {

Gui2Radar::Gui2Radar(Gui2WindowManager *windowManager, const std::string &name,
                     float x_percent, float y_percent, float width_percent,
                     float height_percent, Match *match,
                     const Vector3 &color1_1, const Vector3 &color1_2,
                     const Vector3 &color2_1, const Vector3 &color2_2)
    : Gui2View(windowManager, name, x_percent, y_percent, width_percent,
               height_percent),
      match(match),
      color1_1(color1_1),
      color1_2(color1_2),
      color2_1(color2_1),
      color2_2(color2_2) {
  DO_VALIDATION;



  bg = new Gui2Image(windowManager, "bg_radar", 0, 0, width_percent,
                     height_percent);
  this->AddView(bg);
  bg->LoadImage("media/menu/radar/radar.png");
  bg->Show();

  ball = new Gui2Image(windowManager, "radar_ball", 0, 0, 1, 1.2);
  this->AddView(ball);
  ball->LoadImage("media/menu/radar/ball.png");
  ball->Show();

  // 创建射门力量槽：位于小地图下方的窄条
  // 小地图占 x=38%~62%，y=78%~96%；力量槽紧贴小地图底部
  // 宽度为小地图一半，高度约1.25%（窄条，缩短一半）
  {
    // 力量槽宽度为小地图宽的一半，水平居中
    float barW = width_percent * 0.5f;
    float barX = x_percent + width_percent * 0.25f;  // 居中对齐
    float barY = y_percent + height_percent;          // 紧贴小地图底部
    float barH = 1.25f;           // 高度约1.25%屏幕高（原2.5%缩短一半）
    int px, py, pw, ph;
    windowManager->GetCoordinates(barX, barY, barW, barH, px, py, pw, ph);
    // pw/ph 是像素尺寸，用于创建底层 Image2D 缓冲区
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;
    // sceneRegister=true：注册到场景并自动添加到渲染列表，初始为 Disable 状态（不渲染）
    shotPowerBar = windowManager->CreateImage2D("shot_power_bar", pw, ph, true);
    // 将图像定位到小地图下方的对应屏幕坐标
    shotPowerBar->SetPosition(px, py);
    // 初始填充黑色背景（Disable 状态下不可见，仅初始化缓冲区内容）
    shotPowerBar->DrawRectangle(0, 0, pw, ph, Vector3(0, 0, 0), 200);
    shotPowerBar->OnChange();
    shotPowerBarVisible = false;  // 初始不显示，只有蓄力射门时才 Enable
  }

  this->Show();
}

Gui2Radar::~Gui2Radar() {
  DO_VALIDATION;
  // 从场景中移除力量槽图像，避免悬空渲染对象
  if (shotPowerBar) {
    DO_VALIDATION;
    windowManager->RemoveImage(shotPowerBar);
    shotPowerBar.reset();
  }
}

void Gui2Radar::ReloadAvatars(int teamID, unsigned int playerCount) {
  DO_VALIDATION;

  if (teamID == 0) {
    DO_VALIDATION;
    for (unsigned int i = 0; i < team1avatars.size(); i++) {
      DO_VALIDATION;
      team1avatars[i]->Exit();
      delete team1avatars[i];
    }
    team1avatars.clear();
    for (unsigned int i = 0; i < playerCount; i++) {
      DO_VALIDATION;
      Gui2Image *avatar = new Gui2Image(
          windowManager,
          "radar_avatar_" + int_to_str(teamID) + "_" + int_to_str(i), 0, 0, 1.2,
          1.6);
      this->AddView(avatar);
      avatar->LoadImage("media/menu/radar/p1.png");
      avatar->Show();
      team1avatars.push_back(avatar);
    }
  }

  // oof ugly c/p'ed code
  if (teamID == 1) {
    DO_VALIDATION;
    for (unsigned int i = 0; i < team2avatars.size(); i++) {
      DO_VALIDATION;
      team2avatars[i]->Exit();
      delete team2avatars[i];
    }
    team2avatars.clear();
    for (unsigned int i = 0; i < playerCount; i++) {
      DO_VALIDATION;
      Gui2Image *avatar = new Gui2Image(
          windowManager,
          "radar_avatar_" + int_to_str(teamID) + "_" + int_to_str(i), 0, 0, 1.2,
          1.6);
      this->AddView(avatar);
      avatar->LoadImage("media/menu/radar/p2.png");
      avatar->Show();
      team2avatars.push_back(avatar);
    }
  }
}

void Gui2Radar::Process() { DO_VALIDATION; }

void Gui2Radar::Put() {
  DO_VALIDATION;

  Vector3 position = match->GetBall()->Predict(0).Get2D();
  Vector3 pos2d =
      position * Vector3(1 / (pitchHalfW * 2), -(1 / (pitchHalfH * 2)), 0);
  pos2d = pos2d + Vector3(0.5, 0.5, 0);
  pos2d =
      pos2d * Vector3(0.96f, 0.96f, 0) + Vector3(0.02f, 0.02f, 0);  // margin
  ball->SetPosition(pos2d.coords[0] * width_percent - 0.5f,
                    pos2d.coords[1] * height_percent - 0.6f);

  // get player positions
  std::vector<Player *> team1players;
  match->GetActiveTeamPlayers(0, team1players);
  std::vector<Player *> team2players;
  match->GetActiveTeamPlayers(1, team2players);

  if (team1players.size() != team1avatars.size())
    ReloadAvatars(0, team1players.size());
  if (team2players.size() != team2avatars.size())
    ReloadAvatars(1, team2players.size());
  ball->SetZPriority(1);  // ball on top

  for (unsigned int i = 0; i < team1players.size(); i++) {
    DO_VALIDATION;
    Vector3 position = team1players[i]->GetPosition();
    Vector3 pos2d =
        position * Vector3(1 / (pitchHalfW * 2), -(1 / (pitchHalfH * 2)), 0);
    pos2d = pos2d + Vector3(0.5, 0.5, 0);
    pos2d =
        pos2d * Vector3(0.96f, 0.96f, 0) + Vector3(0.02f, 0.02f, 0);  // margin

    team1avatars[i]->SetPosition(pos2d.coords[0] * width_percent - 0.6f,
                                 pos2d.coords[1] * height_percent - 0.8f);
  }

  for (unsigned int i = 0; i < team2players.size(); i++) {
    DO_VALIDATION;
    Vector3 position = team2players[i]->GetPosition();
    position *= Vector3(-1, -1, 0);
    Vector3 pos2d =
        position * Vector3(1 / (pitchHalfW * 2), -(1 / (pitchHalfH * 2)), 0);
    pos2d = pos2d + Vector3(0.5, 0.5, 0);
    pos2d =
        pos2d * Vector3(0.96f, 0.96f, 0) + Vector3(0.02f, 0.02f, 0);  // margin

    team2avatars[i]->SetPosition(pos2d.coords[0] * width_percent - 0.6f,
                                 pos2d.coords[1] * height_percent - 0.8f);
  }

  // ===== 射门力量槽更新 =====
  // 遍历所有人类控制器，找到正在蓄力射门的玩家
  // 只有 actionMode==2 且 actionButton==Shot 时才显示
  {
    std::vector<HumanGamer *> humanGamers;
    match->GetTeam(0)->GetHumanControllers(humanGamers);
    match->GetTeam(1)->GetHumanControllers(humanGamers);

    // 找第一个正在蓄力射门的人类控制器
    // actionMode==2 且 actionButton==Shot 表示玩家正在蓄力射门
    // 使用 shotPressStartTime_ms 计算跨环境步的真实按压时长
    int shotGauge_ms = 0;
    bool isShooting = false;
    for (auto *gamer : humanGamers) {
      DO_VALIDATION;
      HumanController *hc = gamer->GetHumanController();
      if (hc && !hc->Disabled() && hc->GetActionMode() == 2 &&
          hc->GetActionButton() == e_ButtonFunction_Shot) {
        DO_VALIDATION;
        int queued = hc->GetShotQueuedGauge_ms();
        if (queued >= 0) {
          // 射门命令已推入队列：冻结在推入时的 gauge，不再随时间增长
          shotGauge_ms = queued;
        } else {
          // 仍在蓄力中：用时间戳计算真实按压时长
          int pressStart = hc->GetShotPressStartTime_ms();
          if (pressStart >= 0) {
            int elapsed = match->GetActualTime_ms() - pressStart;
            shotGauge_ms = clamp(elapsed, 10, 1000);
          } else {
            shotGauge_ms = hc->GetGauge_ms();
          }
        }
        isShooting = true;
        break;
      }
    }

    // 获取力量槽的像素尺寸
    Vector3 barSize = shotPowerBar->GetSize();
    int pw = (int)barSize.coords[0];
    int ph = (int)barSize.coords[1];
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;

    if (isShooting) {
      DO_VALIDATION;
      // 首次进入蓄力状态：启用（显示）力量槽
      if (!shotPowerBarVisible) {
        DO_VALIDATION;
        shotPowerBar->Enable();
        shotPowerBarVisible = true;
      }

      // 力量槽显示比例，与 humanoid_utils.cpp 射门球速公式完全一致：
      //   shotGaugeFactor = (gauge_ms - 60) / (500 - 60)   [0~1]
      //   desiredPower    = pow(shotGaugeFactor, 0.6)       [0~1]
      //   estimatedSpeed  = 20 + desiredPower * 40          [20~60 m/s]
      //   displayRatio    = estimatedSpeed / 60             [33%~100%]
      // 短按（gauge_ms→0）→ desiredPower≈0 → 33%（20 m/s）
      // 长按（gauge_ms→500ms）→ desiredPower=1 → 100%（60 m/s）
      const int baseTime_ms = 60;
      const int maxTrigger_ms = 500;
      float shotGaugeFactor = (shotGauge_ms - baseTime_ms) *
                              (1.0f / float(maxTrigger_ms - baseTime_ms));
      shotGaugeFactor = std::max(0.0f, std::min(1.0f, shotGaugeFactor));

      // pow(2.0) 与 humancontroller.cpp desiredPower 计算公式一致
      // 上凸曲线：短按力量极弱，长按才线性增强
      float desiredPower = std::pow(shotGaugeFactor, 2.0f);

      // 与 humanoid_utils.cpp 中 adaptedDesiredPower 公式一致：20 + dp*40
      const float maxBallSpeed = 60.0f;
      float estimatedSpeed = 20.0f + desiredPower * 40.0f;
      float power = std::max(0.0f, std::min(1.0f, estimatedSpeed / maxBallSpeed));

      printf("[RADAR] shotGauge_ms=%d  shotGaugeFactor=%.3f  desiredPower=%.3f  estimatedSpeed=%.2f  displayRatio=%.3f\n",
             shotGauge_ms, shotGaugeFactor, desiredPower, estimatedSpeed, power);


      // 绘制力量槽：先清空（深灰色背景，半透明），再用绿色填充已积攒的部分
      // 外框：深灰色（稍透明）
      shotPowerBar->DrawRectangle(0, 0, pw, ph, Vector3(30, 30, 30), 200);
      // 1px 边框留白（视觉效果）
      int innerX = 1;
      int innerY = 1;
      int innerW = pw - 2;
      int innerH = ph - 2;
      if (innerW < 1) { innerW = pw; innerX = 0; }
      if (innerH < 1) { innerH = ph; innerY = 0; }

      // 绿色填充：宽度等比于当前力量值（等比例显示）
      int fillW = (int)std::round(innerW * power);
      if (fillW > 0) {
        DO_VALIDATION;
        // 根据力量大小选色：低力量为暗绿，高力量为亮绿（满力接近黄绿）
        int r = (int)(50 + 150 * power);   // 随力量增大变亮
        int g = (int)(180 + 50 * power);   // 绿色主色调
        int b = 20;
        r = std::min(r, 255);
        g = std::min(g, 255);
        shotPowerBar->DrawRectangle(innerX, innerY, fillW, innerH,
                                    Vector3(r, g, b), 230);
      }
      shotPowerBar->OnChange();
    } else if (shotPowerBarVisible) {
      DO_VALIDATION;
      // 射门结束：禁用（隐藏）力量槽
      shotPowerBar->Disable();
      shotPowerBarVisible = false;
    }
  }
}
}
