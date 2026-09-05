/*
 * power.h — 电源管理：开机锁存、按键长按关机、电池电量监测
 *
 * 职责：InitPower 后 IO7 持续拉高维持供电（松开物理按键不断电）；
 * dino_power 任务轮询按键（长按 1.5s 松手 → 关机序列 → IO7 拉低断电），
 * 每 5s 采样电池电压。双击电源键通过 PowerSetButtonCallback 通知外部
 * （main.cc 接线到 ChatToggle，本模块不知道"对话"的存在）。
 */
#pragma once

void InitPower();
int GetBatteryLevel();
int GetBatteryVoltageMv();

/** 注册"双击电源键"回调（main.cc 接线到 ChatToggle）。在 InitPower 之后调用。 */
void PowerSetButtonCallback(void (*cb)(void));
