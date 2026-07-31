#pragma once

/**
 * 启动 Audio Hub 设备注册、激活轮询和心跳后台任务。
 * 调用前必须已连接 Wi-Fi；重复调用不会创建重复任务。
 */
void device_registry_start(void);
