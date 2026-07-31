#pragma once

// Servo index enum — each physical servo for the dinosaur
enum ServoIndex {
    SERVO_NECK_TILT = 0,   // IO15: neck tilt front/back
    SERVO_NECK_LEAN = 1,   // IO16: neck lean left/right
    SERVO_HEAD_TURN = 2,   // IO17: head turn left/right
    SERVO_TAIL_UD   = 3,   // IO18: tail up/down
    SERVO_TAIL_LR   = 4,   // IO8:  tail left/right
};

void InitServos();
void SetServoAngle(int idx, int angle);

extern const int kServoCount;
