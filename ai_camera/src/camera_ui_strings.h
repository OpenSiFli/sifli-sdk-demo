/* SPDX-License-Identifier: Apache-2.0
 *
 * Every word the assistant UI shows, in one place: change the text here and no
 * layout code has to move.
 *
 * Note on the font: camera_ui_font.c is a fixed Source Han Sans subset that has
 * ASCII plus only these Han characters:
 *   一上下不中保储加可取回图在失存就已库开打拍摄无暂未机正照片用相绪读败载返页
 * So 聆听中 / 发送中 / 回答中 are NOT renderable yet. The voice control therefore
 * uses short ASCII words as placeholders; replace them below as soon as the font
 * subset carries the characters you want.
 */
#ifndef AI_CAMERA_UI_STRINGS_H
#define AI_CAMERA_UI_STRINGS_H

/* XiaoZhi link state, shown in the sidebar panel. That panel is also the session
 * switch: pressed while not ready it connects and keeps the session alive,
 * pressed while ready it closes the session. Only two states are shown, so a
 * reconnect in flight still reads as 未就绪. */
#define CAMERA_UI_TEXT_XIAOZHI_READY   "XiaoZhi 已就绪"
#define CAMERA_UI_TEXT_XIAOZHI_OFFLINE "XiaoZhi 未就绪"

/* Voice control, in the sidebar under the pending photo. IDLE doubles as the
 * hint for how to start talking; set it to "" to show an empty bar. */
#define CAMERA_UI_TEXT_VOICE_IDLE      "hold KEY3"
#define CAMERA_UI_TEXT_VOICE_LISTENING "listening"
#define CAMERA_UI_TEXT_VOICE_SENT      "sent"
#define CAMERA_UI_TEXT_VOICE_ANSWERING "answering"
/* Push-to-talk was refused because there is no session: nothing was recorded or
 * sent, so the bar must not claim "sent" (中文建议: "未连接"，需 连/接 字形). */
#define CAMERA_UI_TEXT_VOICE_OFFLINE   "no session"

/* Album delete mode, entered with the delete button next to 返回. English
 * placeholders because the font subset above has no glyph for
 * 删/除/确/认/取/消: regenerate camera_ui_font.c, then only these five macros
 * have to change. Suggested Chinese: "删除" / "点照片删除" / "删除中..." /
 * "已删除" / "删除失败". */
#define CAMERA_UI_TEXT_DELETE_BUTTON "delete"
#define CAMERA_UI_TEXT_DELETE_ARMED  "tap a photo"
#define CAMERA_UI_TEXT_DELETE_BUSY   "deleting..."
#define CAMERA_UI_TEXT_DELETE_DONE   "deleted"
#define CAMERA_UI_TEXT_DELETE_FAILED "delete failed"

#endif
