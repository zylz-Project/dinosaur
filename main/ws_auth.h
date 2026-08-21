/*
 * ws_auth.h — 鉴权: 登录换取会话 cookie, 供 WebSocket 握手使用
 *
 * 实测(2026-08-11): 服务端实时对话接口已改为 cookie + Origin 鉴权,
 * 单独带 Bearer token 会被 CLOSE(1008)/返回 AUTH_REQUIRED。
 * 正确流程:
 *   1. POST /auth/login {"username","password"} → Set-Cookie: mem_dialog_session=<v>
 *   2. WS 握手头: Cookie: mem_dialog_session=<v> + Origin: https://www.mmemoryy.xyz
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 登录接口地址 / 账号配置 */
#ifndef AUTH_API_URL
#define AUTH_API_URL "https://www.mmemoryy.xyz/api-proxy/api/v1/auth/login"
#endif
#ifndef AUTH_USERNAME
#define AUTH_USERNAME "admin"
#endif
#ifndef AUTH_PASSWORD
#define AUTH_PASSWORD "admin123"
#endif

#define WS_AUTH_COOKIE_MAX_LEN 256

/**
 * @brief 获取并缓存会话 cookie (mem_dialog_session=<value>)
 *
 * POST {"username":..., "password":...} JSON 到 AUTH_API_URL,
 * 从响应 Set-Cookie 头中提取 mem_dialog_session 并缓存。
 * 之后调用直接返回缓存, 不发网络请求。
 *
 * @param cookie_out  [OUT] 拷贝 "mem_dialog_session=<value>" 的缓冲区 (可为 NULL)
 * @param cookie_size cookie_out 大小
 * @return 0 成功 (cookie 已缓存), -1 失败
 */
int ws_auth_get_cookie(char *cookie_out, int cookie_size);

/** 清除内存中的会话 Cookie，使下一次 get_cookie() 强制重新登录。 */
void ws_auth_invalidate_cookie(void);

#ifdef __cplusplus
}
#endif
