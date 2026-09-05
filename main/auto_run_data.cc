/*
 * auto_run_data.cc — 动作库数据表（动作库本体）
 *
 * 改动作/加动作只改这个文件：
 *   1. 每张 kXxxFrames[] 是一段编舞：每行一个关键帧
 *      {持续ms, {五轴角度}, 缓动}；kKeep = 该轴保持上一帧。
 *      五轴顺序 = {neck_tilt 小头俯仰, neck_lean 长颈前后,
 *      head_turn 小头左右, tail_ud 尾巴上下, tail_lr 尾巴左右}。
 *      编舞内部 tail_ud 用 0=上、125=下；行尾注释是这段的表演意图。
 *   2. 把表挂进对应的 kXxxClips[]（一个动作的随机池，名字任意但别重复）。
 *   3. 若新动作分组（改 DinoAction 枚举），同步 kActionClips[] 的顺序。
 * 引擎怎么插值/怎么输出舵机见 auto_run.cc。
 */
#include "auto_run_data.h"

// -------------------------------------------------------------------------
// Character motion library
// -------------------------------------------------------------------------
// KEEP leaves that axis at the preceding keyframe. This is what creates the
// animal-like timing: attention (head) moves before posture (neck), and the
// tail reveals emotion last.

const Keyframe kDiscoverFrames[] = {
    {140, {kKeep, kKeep, 128, kKeep, kKeep}, EASE_FAST_OUT}, // small head finds movement
    {170, {kKeep, kKeep, 128, kKeep, kKeep}, EASE_LINEAR},   // dinosaur freeze
    {320, {62, 118, 124, 32, 64}, EASE_SMOOTH},              // long neck rises, tail braces
    {440, {70, 150, 56, 28, 146}, EASE_SMOOTH},              // survey one horizon edge
    {520, {78, 34, 142, 34, 40}, EASE_SMOOTH},               // whole neck crosses the centre
    {380, {108, 106, 84, 42, 122}, EASE_SMOOTH},             // decide it is safe, reach to user
    {560, {94, 96, 90, 58, 90}, EASE_SMOOTH},                // alert sauropod stance
};

// A slower peek: retreat first, cautiously inspect, then suddenly decide the
// user is a friend. It reads very differently from the direct greeting above.
const Keyframe kDiscoverPeekFrames[] = {
    {110, {kKeep, kKeep, 134, kKeep, kKeep}, EASE_FAST_OUT},
    {240, {48, 76, 138, 34, 126}, EASE_SMOOTH},              // retract into an S-like guard
    {210, {48, 76, 138, 30, 126}, EASE_LINEAR},
    {390, {66, 146, 48, 26, 42}, EASE_SMOOTH},               // tall-neck peek
    {520, {76, 36, 144, 32, 148}, EASE_SMOOTH},              // verify the other side
    {420, {116, 112, 82, 40, 62}, EASE_SMOOTH},              // curious forward reach
    {620, {94, 98, 90, 58, 90}, EASE_SMOOTH},
};

// 仰望发现：IO15小头先看到高处，IO17随后抬到接近0°，整条长颈
// 再横跨两侧追踪。尾巴故意比头颈晚一拍，形成第三种发现节奏。
const Keyframe kDiscoverSkyFrames[] = {
    {100, {kKeep, kKeep, 166, kKeep, kKeep}, EASE_FAST_OUT},
    {190, {kKeep, kKeep, 166, kKeep, kKeep}, EASE_LINEAR},
    {340, {8, 118, 150, 18, 24}, EASE_SMOOTH},               // neck looks upward after head
    {520, {16, 174, 24, 0, 162}, EASE_SMOOTH},               // high sweep across one edge
    {580, {38, 6, 174, 12, 18}, EASE_SMOOTH},                // cross 90° to the other edge
    {260, {54, 82, 90, 16, 146}, EASE_LINEAR},               // head reconnects with user first
    {460, {116, 108, 78, 34, 42}, EASE_SMOOTH},              // curious forward reach
    {620, {92, 96, 90, 56, 90}, EASE_SMOOTH},
};

const Keyframe kAffectionFrames[] = {
    {260, {104, 100, 90, 48, 92}, EASE_SMOOTH},              // make eye contact
    {520, {142, 118, 78, 32, 66}, EASE_SMOOTH},              // long-neck greeting bow
    {360, {164, 110, 86, 28, 122}, EASE_SMOOTH},             // lower forehead toward the hand
    {440, {150, 124, 78, 30, 58}, EASE_LINEAR},              // stay close instead of rubbing
    {520, {116, 104, 88, 38, 114}, EASE_SMOOTH},             // lift and check the user's face
    {720, {96, 96, 90, 58, 90}, EASE_SMOOTH},
};

const Keyframe kAffectionCuddleFrames[] = {
    {300, {96, 66, 124, 42, 128}, EASE_SMOOTH},              // head notices before neck
    {560, {132, 142, 58, 28, 40}, EASE_SMOOTH},              // broad approach arc
    {520, {170, 116, 82, 24, 144}, EASE_SMOOTH},             // deep bow near the user
    {520, {158, 92, 90, 26, 52}, EASE_LINEAR},               // calm contact/hover
    {580, {118, 56, 132, 38, 138}, EASE_SMOOTH},             // withdraw through the other side
    {760, {96, 94, 90, 58, 90}, EASE_SMOOTH},
};

// 萌宠化惊喜变体：保留一次跨中位的脸颊轻蹭和更明显的摇尾。
// 它与两套长颈问候随机混用，不会每次触摸都复读猫式动作。
const Keyframe kAffectionNuzzleFrames[] = {
    {240, {104, 102, 90, 48, 90}, EASE_SMOOTH},
    {420, {126, 132, 72, 32, 42}, EASE_SMOOTH},              // offer one cheek
    {480, {138, 48, 138, 26, 148}, EASE_SMOOTH},             // one broad nuzzle across 90°
    {360, {150, 112, 82, 24, 52}, EASE_LINEAR},              // stay close for touch
    {520, {116, 76, 118, 36, 136}, EASE_SMOOTH},             // playful after-glance
    {720, {96, 96, 90, 58, 90}, EASE_SMOOTH},
};

// 害羞贴近：先缩高偷看，再突然把长颈完整放低到用户手边；靠近后
// 不是反复蹭，而是安静停留并用尾巴做一次延迟回应。
const Keyframe kAffectionShyFrames[] = {
    {180, {52, 132, 18, 24, 132}, EASE_FAST_OUT},             // shy side glance
    {320, {28, 156, 12, 10, 42}, EASE_SMOOTH},               // retreat upward
    {240, {28, 156, 12, 8, 42}, EASE_LINEAR},
    {620, {176, 118, 82, 4, 154}, EASE_SMOOTH},              // full low approach
    {460, {180, 92, 90, 0, 28}, EASE_LINEAR},                // forehead waits for touch
    {360, {154, 72, 112, 8, 168}, EASE_SMOOTH},              // one delayed happy-tail answer
    {620, {112, 108, 78, 32, 52}, EASE_SMOOTH},
    {760, {94, 96, 90, 58, 90}, EASE_SMOOTH},
};

const Keyframe kHappyFrames[] = {
    {240, {54, 92, 90, 24, 90}, EASE_FAST_OUT},              // proud juvenile neck lift
    {440, {88, 154, 54, 20, 36}, EASE_SMOOTH},               // broad display to one side
    {620, {118, 28, 148, 30, 152}, EASE_SMOOTH},             // one complete cross-body sweep
    {500, {142, 108, 82, 24, 54}, EASE_SMOOTH},              // playful forward reach
    {720, {96, 94, 90, 56, 90}, EASE_SMOOTH},
};

const Keyframe kHappyDanceFrames[] = {
    {280, {42, 72, 116, 22, 136}, EASE_FAST_OUT},            // rise and show the long silhouette
    {560, {108, 166, 42, 28, 30}, EASE_SMOOTH},              // large diagonal sweep
    {680, {158, 34, 146, 24, 154}, EASE_SMOOTH},             // low opposite reach, tail balances
    {540, {74, 116, 66, 20, 46}, EASE_SMOOTH},               // lift tall again
    {760, {96, 96, 90, 56, 90}, EASE_SMOOTH},
};

// 两路脖子相差约四分之一圈，形成真正的空间画圆；尾巴以反相椭圆
// 做身体平衡。圆周速度故意不完全一致，避免像展台上的重复机构。
const Keyframe kHappyOrbitFrames[] = {
    {180, {42, 90, 108, 34, 90}, EASE_FAST_OUT},
    {240, {68, 166, 112, 58, 154}, EASE_SMOOTH},
    {240, {146, 174, 78, 132, 92}, EASE_SMOOTH},
    {260, {178, 92, 66, 148, 24}, EASE_SMOOTH},
    {250, {136, 12, 102, 76, 70}, EASE_SMOOTH},
    {240, {54, 4, 122, 30, 158}, EASE_SMOOTH},
    {280, {18, 86, 108, 48, 88}, EASE_SMOOTH},
    {310, {96, 142, 76, 122, 32}, EASE_SMOOTH},
    {520, {98, 94, 90, 72, 90}, EASE_SMOOTH},
};

// 追尾游戏：头、长颈、尾巴依次启动，三个部位使用不同的换向时刻。
// 它比空间圆更像幼龙突然兴奋起来追逐自己的长尾。
const Keyframe kHappyChaseFrames[] = {
    {110, {kKeep, kKeep, 12, kKeep, kKeep}, EASE_FAST_OUT},  // head darts first
    {210, {42, 142, 172, kKeep, kKeep}, EASE_SMOOTH},        // neck follows opposite gaze
    {360, {68, 176, 28, 0, 18}, EASE_SMOOTH},               // tail launches last
    {520, {156, 22, 162, 22, 176}, EASE_SMOOTH},            // large diagonal chase
    {260, {112, 88, 90, 8, 34}, EASE_LINEAR},               // tiny surprise pause
    {480, {18, 118, 36, 0, 162}, EASE_FAST_OUT},             // spring upward, tail still crossing
    {560, {138, 164, 146, 18, 12}, EASE_SMOOTH},             // playful second reach, not a repeat
    {760, {96, 94, 90, 54, 90}, EASE_SMOOTH},
};

const Keyframe kProudCallFrames[] = {
    {260, {34, 90, 90, 30, 90}, EASE_SMOOTH},                // inhale and raise neck
    {180, {0, 90, 90, 0, 90}, EASE_FAST_OUT},                // IO17=0°, skyward howl
    {520, {0, 90, 90, 0, 138}, EASE_LINEAR},                 // hold the full proud call
    {400, {118, 106, 116, 38, 125}, EASE_SMOOTH},
    {420, {112, 74, 66, 40, 58}, EASE_SMOOTH},               // survey other side
    {420, {116, 101, 108, 42, 118}, EASE_SMOOTH},
    {500, {105, 92, 94, 50, 85}, EASE_SMOOTH},
    {650, {97, 92, 92, 70, 90}, EASE_SMOOTH},                // proud settle
};

const Keyframe kProudSweepFrames[] = {
    {360, {42, 126, 132, 18, 132}, EASE_SMOOTH},             // diagonal inhale
    {180, {16, 136, 142, 8, 146}, EASE_LINEAR},
    {240, {0, 118, 126, 0, 42}, EASE_FAST_OUT},              // IO17=0° skyward call
    {500, {6, 46, 34, 2, 154}, EASE_LINEAR},                 // hold, head finds horizon
    {580, {20, 164, 30, 8, 28}, EASE_SMOOTH},                // large high sweep
    {620, {34, 18, 152, 12, 160}, EASE_SMOOTH},              // cross to the other side
    {620, {62, 122, 64, 28, 42}, EASE_SMOOTH},
    {760, {84, 94, 90, 50, 90}, EASE_SMOOTH},
};

// 长叫声的慢速大幅巡视：不是原地小摆，而是脖子与尾巴各走一条
// 空间椭圆，中间加入两次凝视，让整段有“寻找—展示—确认”的叙事。
const Keyframe kProudOrbitFrames[] = {
    {320, {24, 92, 96, 16, 88}, EASE_SMOOTH},
    {220, {0, 92, 90, 0, 92}, EASE_FAST_OUT},                // all long-call clips reach skyward
    {480, {2, 150, 48, 4, 158}, EASE_LINEAR},                // hold the call before orbit
    {420, {48, 170, 118, 24, 158}, EASE_SMOOTH},
    {440, {126, 178, 84, 138, 106}, EASE_SMOOTH},
    {300, {154, 150, 72, 152, 42}, EASE_LINEAR},             // side gaze hold
    {460, {178, 78, 68, 118, 18}, EASE_SMOOTH},
    {440, {132, 6, 106, 42, 76}, EASE_SMOOTH},
    {420, {46, 0, 122, 24, 164}, EASE_SMOOTH},
    {300, {16, 34, 116, 58, 138}, EASE_LINEAR},              // opposite gaze hold
    {480, {62, 118, 80, 142, 30}, EASE_SMOOTH},
    {600, {100, 92, 90, 66, 90}, EASE_SMOOTH},
};

// 双段长啸：第一次正面仰天长啸，巡视确认用户后从另一侧做一次更短
// 的回应叫。两个叫声之间头、颈、尾巴的启动顺序不同。
const Keyframe kProudEchoFrames[] = {
    {300, {36, 90, 90, 18, 90}, EASE_SMOOTH},
    {180, {0, 90, 90, 0, 90}, EASE_FAST_OUT},                // first skyward call
    {560, {0, 104, 118, 0, 154}, EASE_LINEAR},
    {620, {48, 172, 18, 16, 24}, EASE_SMOOTH},               // broad response check
    {360, {72, 18, 176, 24, 168}, EASE_SMOOTH},              // head finds opposite side first
    {220, {18, 26, 154, 8, 142}, EASE_FAST_OUT},
    {420, {0, 34, 138, 0, 28}, EASE_LINEAR},                 // shorter echo call
    {620, {42, 150, 32, 18, 164}, EASE_SMOOTH},
    {780, {84, 94, 90, 48, 90}, EASE_SMOOTH},
};

const Keyframe kEatFrames[] = {
    {420, {138, 103, 96, 80, 104}, EASE_SMOOTH},             // track food downward
    {230, {178, 100, 92, 82, 76}, EASE_FAST_OUT},            // first decisive bite
    {260, {148, 104, 98, 80, 110}, EASE_SMOOTH},             // lift just enough to chew
    {240, {180, 108, 102, 82, 68}, EASE_FAST_OUT},           // second bite, different side
    {290, {151, 96, 86, 80, 118}, EASE_SMOOTH},
    {250, {176, 92, 84, 82, 72}, EASE_FAST_OUT},             // final smaller bite
    {320, {147, 104, 102, 80, 112}, EASE_SMOOTH},
    {420, {153, 102, 96, 80, 108}, EASE_SMOOTH},             // low chewing, do not stand up
    {340, {142, 94, 90, 78, 90}, EASE_SMOOTH},
};

const Keyframe kEatSniffFrames[] = {
    {320, {126, 124, 116, 76, 112}, EASE_SMOOTH},            // approach food from one side
    {240, {145, 132, 108, 78, 66}, EASE_SMOOTH},             // sniff 1
    {220, {130, 126, 112, 76, 116}, EASE_SMOOTH},            // sniff back
    {250, {151, 118, 104, 78, 62}, EASE_SMOOTH},             // sniff 2, closer
    {280, {180, 110, 98, 82, 126}, EASE_FAST_OUT},           // commit to a full-depth bite
    {300, {150, 104, 94, 80, 54}, EASE_SMOOTH},              // chew with cheek turned
    {280, {177, 86, 80, 82, 132}, EASE_FAST_OUT},            // bite from opposite side
    {320, {148, 78, 84, 80, 50}, EASE_SMOOTH},
    {420, {156, 92, 90, 80, 118}, EASE_SMOOTH},              // satisfied low chewing
    {380, {140, 100, 94, 78, 90}, EASE_SMOOTH},
};

const Keyframe kEatLookUpFrames[] = {
    {170, {180, 86, 82, 82, 58}, EASE_FAST_OUT},             // already hungry: bite immediately
    {160, {148, 104, 100, 78, 128}, EASE_FAST_OUT},
    {180, {176, 114, 108, 82, 52}, EASE_FAST_OUT},
    {180, {150, 120, 106, 78, 136}, EASE_FAST_OUT},
    {330, {84, 102, 90, 58, 72}, EASE_FAST_OUT},             // lift fully and look at the user
    {420, {78, 90, 90, 52, 116}, EASE_LINEAR},               // eye contact: “this is tasty”
    {360, {132, 76, 78, 72, 62}, EASE_SMOOTH},               // smell a new spot
    {190, {180, 82, 84, 82, 132}, EASE_FAST_OUT},            // return for a deep bite
    {170, {149, 104, 98, 80, 54}, EASE_FAST_OUT},
    {420, {158, 96, 92, 80, 104}, EASE_SMOOTH},              // remain near the food
};

// 低位横向取食：IO17保持接近180°，主要用IO16沿地面从一侧吃到
// 另一侧；IO15在每个食物点先定位，避免只有上下啄食一种节奏。
const Keyframe kEatGrazeFrames[] = {
    {360, {142, 154, 18, 58, 142}, EASE_SMOOTH},              // spot food at one edge
    {260, {180, 166, 8, 34, 24}, EASE_FAST_OUT},             // deep first bite
    {340, {154, 142, 38, 28, 154}, EASE_SMOOTH},             // chew while moving sideways
    {520, {176, 92, 90, 18, 36}, EASE_SMOOTH},               // graze through the centre
    {260, {180, 24, 172, 12, 166}, EASE_FAST_OUT},           // opposite-edge bite
    {380, {150, 42, 146, 8, 18}, EASE_SMOOTH},
    {300, {178, 72, 118, 4, 152}, EASE_FAST_OUT},            // final smaller bite
    {460, {148, 108, 76, 18, 42}, EASE_SMOOTH},              // satisfied low chew
    {620, {116, 96, 90, 46, 90}, EASE_SMOOTH},
};

const Keyframe kListenFrames[] = {
    {100, {kKeep, kKeep, kKeep, 42, 90}, EASE_FAST_OUT},     // tail stiffens first
    {220, {76, 90, 128, 42, 90}, EASE_FAST_OUT},             // locate sound
    {260, {82, 108, 126, 40, 100}, EASE_SMOOTH},
    {260, {86, 113, 105, 42, 108}, EASE_SMOOTH},             // tilted listening pause
    {380, {88, 113, 105, 42, 108}, EASE_LINEAR},
    {320, {82, 72, 52, 42, 76}, EASE_SMOOTH},                // check opposite side
    {360, {84, 90, 90, 45, 90}, EASE_SMOOTH},
    {420, {88, 94, 94, 50, 90}, EASE_SMOOTH},
};

const Keyframe kListenTrackFrames[] = {
    {90, {kKeep, kKeep, 142, 32, 112}, EASE_FAST_OUT},
    {180, {66, 112, 138, 28, 122}, EASE_FAST_OUT},
    {420, {72, 126, 118, 26, 132}, EASE_SMOOTH},             // hold one ear toward sound
    {240, {88, 118, 102, 30, 58}, EASE_SMOOTH},
    {180, {108, 104, 94, 34, 138}, EASE_FAST_OUT},           // sound seems closer
    {260, {98, 64, 48, 30, 42}, EASE_SMOOTH},
    {420, {80, 76, 58, 34, 126}, EASE_SMOOTH},
    {500, {90, 94, 94, 52, 90}, EASE_SMOOTH},
};

const Keyframe kListenDoubleCheckFrames[] = {
    {100, {62, 90, 90, 36, 90}, EASE_FAST_OUT},              // whole neck pulls back
    {220, {70, 132, 142, 30, 138}, EASE_FAST_OUT},
    {260, {76, 138, 116, 28, 48}, EASE_SMOOTH},
    {160, {76, 138, 116, 28, 48}, EASE_LINEAR},
    {220, {82, 52, 38, 32, 142}, EASE_FAST_OUT},             // rapid second check
    {300, {92, 62, 58, 36, 44}, EASE_SMOOTH},
    {360, {104, 106, 112, 42, 132}, EASE_SMOOTH},
    {520, {92, 94, 94, 56, 90}, EASE_SMOOTH},
};

// 高空声源：头部先走接近完整行程，IO17随后仰到0°寻找天空中的声音；
// 尾巴先保持不动，确认声源移动后才做一次跨中位配重。
const Keyframe kListenOverheadFrames[] = {
    {90, {kKeep, kKeep, 4, kKeep, kKeep}, EASE_FAST_OUT},
    {220, {34, 64, 8, 42, 90}, EASE_SMOOTH},
    {260, {0, 48, 22, 18, 90}, EASE_FAST_OUT},               // neck reaches sky after head
    {460, {0, 48, 22, 8, 90}, EASE_LINEAR},                 // listen with tail still centred
    {180, {8, 118, 176, 4, 28}, EASE_FAST_OUT},              // sound crosses overhead
    {520, {18, 172, 154, 0, 166}, EASE_SMOOTH},              // delayed tail balance
    {420, {46, 12, 18, 14, 42}, EASE_SMOOTH},                // verify far edge
    {680, {86, 94, 90, 50, 90}, EASE_SMOOTH},
};

const Keyframe kStartledFrames[] = {
    {80, {kKeep, kKeep, kKeep, kKeep, kKeep}, EASE_LINEAR},  // freeze
    {140, {42, 74, 62, 24, 142}, EASE_FAST_OUT},             // retract neck, tail becomes a high brace
    {380, {42, 74, 62, 22, 142}, EASE_LINEAR},               // assess danger
    {260, {54, 92, 126, 24, 54}, EASE_SMOOTH},               // small head checks first
    {380, {66, 142, 44, 28, 148}, EASE_SMOOTH},              // whole long neck locates it
    {460, {88, 42, 142, 34, 38}, EASE_SMOOTH},               // scan across before approaching
    {560, {108, 104, 86, 44, 118}, EASE_SMOOTH},             // cautious re-emergence
    {620, {94, 96, 90, 58, 90}, EASE_SMOOTH},
};

const Keyframe kStartledJumpFrames[] = {
    {70, {kKeep, kKeep, kKeep, kKeep, kKeep}, EASE_LINEAR},
    {110, {32, 136, 142, 18, 36}, EASE_FAST_OUT},            // diagonal recoil, tail lashes opposite
    {280, {30, 140, 148, 16, 32}, EASE_LINEAR},
    {220, {48, 116, 112, 20, 148}, EASE_FAST_OUT},
    {420, {62, 42, 150, 26, 40}, EASE_SMOOTH},               // guarded horizon scan
    {460, {78, 138, 38, 30, 152}, EASE_SMOOTH},
    {520, {110, 112, 82, 42, 54}, EASE_SMOOTH},              // brave little approach
    {700, {94, 96, 90, 58, 90}, EASE_SMOOTH},
};

// 低伏躲避：与“向上缩颈”相反，先把头压到地面附近躲开，再从一侧
// 抬头确认。尾巴先向一侧绷紧，颈部恢复以后才跨到另一侧。
const Keyframe kStartledDuckFrames[] = {
    {70, {kKeep, kKeep, kKeep, kKeep, kKeep}, EASE_LINEAR},
    {120, {180, 54, 8, 0, 12}, EASE_FAST_OUT},               // full low duck, tail high-left
    {320, {180, 54, 8, 0, 12}, EASE_LINEAR},
    {220, {156, 18, 172, 8, 12}, EASE_FAST_OUT},             // head checks first
    {460, {108, 168, 24, 18, 176}, EASE_SMOOTH},             // neck rises across, tail delayed
    {520, {24, 132, 48, 4, 158}, EASE_SMOOTH},               // stand tall to verify
    {620, {118, 28, 162, 28, 24}, EASE_SMOOTH},              // cautious forward check
    {760, {94, 96, 90, 56, 90}, EASE_SMOOTH},
};

const Keyframe kSleepyFrames[] = {
    {720, {108, 104, 94, 96, 104}, EASE_SMOOTH},
    {900, {146, 126, 72, 112, 64}, EASE_SMOOTH},             // long neck settles into a broad S
    {720, {158, 112, 82, 122, 116}, EASE_SMOOTH},
    {980, {142, 64, 128, 116, 72}, EASE_SMOOTH},             // slow breath across the centre
    {1050, {162, 106, 86, 124, 108}, EASE_SMOOTH},
    {820, {150, 102, 88, 120, 90}, EASE_SMOOTH},
};

const Keyframe kSleepyNodFrames[] = {
    {560, {112, 68, 132, 98, 64}, EASE_SMOOTH},
    {560, {156, 78, 118, 118, 54}, EASE_SMOOTH},             // first long-neck nod
    {320, {118, 94, 88, 108, 112}, EASE_FAST_OUT},           // wakes a little
    {760, {172, 128, 58, 124, 136}, EASE_SMOOTH},            // deeper second nod
    {420, {132, 108, 82, 114, 68}, EASE_SMOOTH},
    {900, {164, 58, 136, 126, 132}, EASE_SMOOTH},            // rest to the other side
    {1100, {152, 104, 88, 122, 104}, EASE_SMOOTH},
};

// 绕身入睡：长颈在低位慢慢横跨身体寻找舒服位置，尾巴比脖子晚很久
// 才落下；中途只有一次微弱抬头，不做重复点头。
const Keyframe kSleepyWrapFrames[] = {
    {760, {122, 158, 24, 72, 154}, EASE_SMOOTH},
    {980, {168, 174, 8, 92, 132}, EASE_SMOOTH},              // lower along one body edge
    {620, {178, 132, 42, 108, 42}, EASE_SMOOTH},             // tail begins to settle late
    {360, {132, 96, 90, 104, 90}, EASE_FAST_OUT},            // one sleepy half-wake
    {1080, {176, 18, 174, 118, 22}, EASE_SMOOTH},            // wrap across the body
    {920, {162, 54, 146, 124, 154}, EASE_SMOOTH},
    {1180, {154, 102, 90, 122, 90}, EASE_SMOOTH},            // quiet breathing rest
};

#define CLIP(label, frames, continued) {label, frames, static_cast<uint8_t>(sizeof(frames) / sizeof(frames[0])), continued}
const MotionClip kDiscoverClips[] = {
    CLIP("discover-direct", kDiscoverFrames, false),
    CLIP("discover-peek", kDiscoverPeekFrames, false),
    CLIP("discover-sky-track", kDiscoverSkyFrames, false),
};
const MotionClip kAffectionClips[] = {
    CLIP("affection-neck-bow", kAffectionFrames, false),
    CLIP("affection-gentle-reach", kAffectionCuddleFrames, false),
    CLIP("affection-playful-nuzzle", kAffectionNuzzleFrames, false),
    CLIP("affection-shy-approach", kAffectionShyFrames, false),
};
const MotionClip kHappyClips[] = {
    CLIP("happy-juvenile-display", kHappyFrames, false),
    CLIP("happy-long-neck-sweep", kHappyDanceFrames, false),
    CLIP("happy-orbit", kHappyOrbitFrames, false),
    CLIP("happy-tail-chase", kHappyChaseFrames, false),
};
const MotionClip kProudClips[] = {
    // 叫声音频最长约 6.9 秒，一段表演结束后换另一段继续，不回到呆站。
    CLIP("proud-front", kProudCallFrames, true),
    CLIP("proud-sweep", kProudSweepFrames, true),
    CLIP("proud-orbit", kProudOrbitFrames, true),
    CLIP("proud-echo-call", kProudEchoFrames, true),
};
const MotionClip kEatClips[] = {
    CLIP("eat-peck", kEatFrames, true),
    CLIP("eat-sniff", kEatSniffFrames, true),
    CLIP("eat-look-up", kEatLookUpFrames, true),
    CLIP("eat-low-graze", kEatGrazeFrames, true),
};
const MotionClip kListenClips[] = {
    CLIP("listen-locate", kListenFrames, true),
    CLIP("listen-track", kListenTrackFrames, true),
    CLIP("listen-double-check", kListenDoubleCheckFrames, true),
    CLIP("listen-overhead", kListenOverheadFrames, true),
};
const MotionClip kStartledClips[] = {
    CLIP("startled-recoil", kStartledFrames, false),
    CLIP("startled-jump", kStartledJumpFrames, false),
    CLIP("startled-low-duck", kStartledDuckFrames, false),
};
const MotionClip kSleepyClips[] = {
    CLIP("sleepy-neck-rest", kSleepyFrames, false),
    CLIP("sleepy-long-neck-nod", kSleepyNodFrames, false),
    CLIP("sleepy-neck-wrap", kSleepyWrapFrames, false),
};
#undef CLIP

#define CLIP_SET(clips) {clips, static_cast<uint8_t>(sizeof(clips) / sizeof(clips[0]))}
const ClipSet kActionClips[DINO_ACTION_COUNT] = {
    CLIP_SET(kDiscoverClips), CLIP_SET(kAffectionClips),
    CLIP_SET(kHappyClips), CLIP_SET(kProudClips),
    CLIP_SET(kEatClips), CLIP_SET(kListenClips),
    CLIP_SET(kStartledClips), CLIP_SET(kSleepyClips),
};
#undef CLIP_SET
