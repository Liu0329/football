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

  // ===== 力量槽更新（射门/短传/高传复用同一槽，颜色区分） =====
  // 射门：绿色；短传：蓝色；高传：青色
  // actionMode==2 时显示，命令推入后冻结，动画完成后隐藏
  // 传球均使用 shotPressStartTime_ms 时间戳（跨环境步），与射门相同机制
  {
    std::vector<HumanGamer *> humanGamers;
    match->GetTeam(0)->GetHumanControllers(humanGamers);
    match->GetTeam(1)->GetHumanControllers(humanGamers);

    int displayGauge_ms = 0;
    float displayPower = 0.0f;
    int colorR = 0, colorG = 200, colorB = 20;
    bool isActive = false;

    for (auto *gamer : humanGamers) {
      DO_VALIDATION;
      HumanController *hc = gamer->GetHumanController();
      if (!hc || hc->Disabled() || hc->GetActionMode() != 2) continue;
      DO_VALIDATION;

      e_ButtonFunction btn = hc->GetActionButton();

      // 计算真实按压时长（推入后冻结，否则用时间戳实时计算）
      auto getPassGauge = [&](int queued) -> int {
        if (queued >= 0) return queued;
        int pressStart = hc->GetShotPressStartTime_ms();
        if (pressStart >= 0) {
          int elapsed = match->GetActualTime_ms() - pressStart;
          return clamp(elapsed, 10, 1000);
        }
        return hc->GetGauge_ms();
      };

      if (btn == e_ButtonFunction_Shot) {
        displayGauge_ms = getPassGauge(hc->GetShotQueuedGauge_ms());
        // 射门：[60,500]ms，指数 2.0，球速 20~60 m/s
        float gf = clamp((displayGauge_ms - 60) * (1.0f / 440.0f), 0.0f, 1.0f);
        float dp = std::pow(gf, 2.0f);
        displayPower = std::max(0.0f, std::min(1.0f, (20.0f + dp * 40.0f) / 60.0f));
        colorR = (int)std::min(255.0f, 50 + 150 * displayPower);
        colorG = (int)std::min(255.0f, 180 + 50 * displayPower);
        colorB = 20;
        isActive = true;
        break;

      } else if (btn == e_ButtonFunction_ShortPass) {
        displayGauge_ms = getPassGauge(hc->GetPassQueuedGauge_ms());
        // 短传：线性显示 gaugeFactor，代表"在最近～最远队友之间选了多远"
        float gf = clamp((displayGauge_ms - 60) * (1.0f / 940.0f), 0.0f, 1.0f);
        displayPower = gf;
        colorR = 20; colorG = (int)std::min(255.0f, 100 + 100 * displayPower); colorB = 220;
        isActive = true;
        break;

      } else if (btn == e_ButtonFunction_LongPass) {
        displayGauge_ms = getPassGauge(hc->GetPassQueuedGauge_ms());
        // 长传：线性
        float gf = clamp((displayGauge_ms - 60) * (1.0f / 940.0f), 0.0f, 1.0f);
        displayPower = gf;
        colorR = 220; colorG = (int)std::min(255.0f, 120 + 80 * displayPower); colorB = 20;
        isActive = true;
        break;

      } else if (btn == e_ButtonFunction_HighPass) {
        displayGauge_ms = getPassGauge(hc->GetPassQueuedGauge_ms());
        // 高传：线性
        float gf = clamp((displayGauge_ms - 60) * (1.0f / 940.0f), 0.0f, 1.0f);
        displayPower = gf;
        colorR = 20; colorG = (int)std::min(255.0f, 200 + 55 * displayPower); colorB = (int)std::min(255.0f, 180 + 75 * displayPower);
        isActive = true;
        break;
      }
    }

    // 获取力量槽的像素尺寸
    Vector3 barSize = shotPowerBar->GetSize();
    int pw = (int)barSize.coords[0];
    int ph = (int)barSize.coords[1];
    if (pw < 1) pw = 1;
    if (ph < 1) ph = 1;

    if (isActive) {
      DO_VALIDATION;
      if (!shotPowerBarVisible) {
        DO_VALIDATION;
        shotPowerBar->Enable();
        shotPowerBarVisible = true;
      }

      // 外框：深灰色背景
      shotPowerBar->DrawRectangle(0, 0, pw, ph, Vector3(30, 30, 30), 200);
      int innerX = 1, innerY = 1;
      int innerW = pw - 2, innerH = ph - 2;
      if (innerW < 1) { innerW = pw; innerX = 0; }
      if (innerH < 1) { innerH = ph; innerY = 0; }

      // 填充：宽度等比于 displayPower
      int fillW = (int)std::round(innerW * displayPower);
      if (fillW > 0) {
        DO_VALIDATION;
        shotPowerBar->DrawRectangle(innerX, innerY, fillW, innerH,
                                    Vector3(colorR, colorG, colorB), 230);
      }
      shotPowerBar->OnChange();
    } else if (shotPowerBarVisible) {
      DO_VALIDATION;
      shotPowerBar->Disable();
      shotPowerBarVisible = false;
    }
  }
}
}
