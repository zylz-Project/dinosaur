/*
 * servo.h — 五路舵机 PWM 驱动（LEDC 50Hz，唯一写舵机寄存器的模块）
 *
 * 职责：InitServos 依次给舵机通电(IO4)、防浮空、配 PWM、回中位；
 * SetServoAngle(idx, angle) 设置某一路角度（内部钳位，IO8 尾巴上下限 55°~180°）。
 * 不负责：动作编排（auto_run.cc）、对话时的动作（chat.cc）——它们都只是
 * 定期调用 SetServoAngle 的"写入者"。
 */
#pragma once

// Servo index enum — each physical servo for the dinosaur
enum ServoIndex {
    SERVO_NECK_TILT = 0,   // IO17: neck up/down
    SERVO_NECK_LEAN = 1,   // IO16: neck lean left/right
    SERVO_HEAD_TURN = 2,   // IO15: head turn left/right
    SERVO_TAIL_UD   = 3,   // IO8:  tail up/down
    SERVO_TAIL_LR   = 4,   // IO18: tail left/right
};

void InitServos();
void SetServoAngle(int idx, int angle);

extern const int kServoCount;
