/* ============================================================
 * EPets · 互动小游戏模组
 *   20 个互动小游戏：16 个宠物专属（每种宠物 2 个）+ 4 个通用玩具
 *   注册 KIND_MINIGAME，外加玩耍菜单（KIND_MENU）
 *
 * 编译: cl /LD /utf-8 /I. Minigame.c /Fe:Minigame.dll
 *
 * run 回调里用加载器服务跑完整交互（ui_title / ui_print / key / delay / rnd），
 * 直接改 pet 属性作为奖励：返回 1=玩成功，0=失败或放弃。
 * 玩法与数值沿用 game_interact.c 原版。
 *
 * 加载器 v7 提供 key_poll()（非阻塞读键）与 now_ms()（毫秒时钟），
 * 三个实时玩法已改回真·限时，判定参数和原版一致：
 *   雪球大战 5 秒内连按 / 力量比拼 4 秒内连按 / 火焰烧烤 0~6 秒火候窗口；
 *   轮询间隔仍是原版的 100ms 一拍。
 * 蓄力充电仍是“连按 Z 蓄力”的等价实现（控制台没有可靠的按住状态），
 * 其余游戏本来就不依赖实时输入。
 * ============================================================ */

#define _CRT_SECURE_NO_WARNINGS 1

#include "Defineds.h"
#include "Loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const LoaderApi *g_api = 0;
static char g_err[192];

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    return 0;
}

static void pause_key(void)
{
    g_api->ui_print("\nZ:继续\n");
    g_api->key();
}

/* ============ 通用工具 ============ */

/* 夹取到 MIN_STAT ~ MAX_STAT（原版 clamp 的替代） */
static uint8_t clamp_stat(int v)
{
    if (v < MIN_STAT) return (uint8_t)MIN_STAT;
    if (v > MAX_STAT) return (uint8_t)MAX_STAT;
    return (uint8_t)v;
}

/* 绝对值（省掉 <stdlib.h>，名字避开 abs） */
static int iabs(int v)
{
    return v < 0 ? -v : v;
}

/**
 * 通用奖励函数（照原版 game_interact.c 16-27 行）
 *
 * @param pet 宠物
 * @param success 是否成功
 * @param happy_gain 成功时心情增加值
 * @param clean_gain 成功时清洁增加值
 */
static void reward(Pet *pet, int success, int happy_gain, int clean_gain)
{
    if (success)
    {
        pet->happy = clamp_stat((int)pet->happy + happy_gain);
        pet->clean = clamp_stat((int)pet->clean + clean_gain);
    }
    else
    {
        pet->happy = clamp_stat((int)pet->happy + 15);
    }
}

/*
 * 真·限时连按：limit_ms 毫秒内数确认键，按返回键提前结束。
 * 和原版一样 100ms 一拍（sys_key_nb + sys_delay(100)），
 * 所以阈值（雪球 8/4、推 10/5）可以沿用原版数值。
 */
static int mash_timed(uint32_t limit_ms)
{
    uint32_t start = g_api->now_ms();
    int count = 0;

    while (g_api->now_ms() - start < limit_ms)
    {
        int k = g_api->key_poll();
        if (k == KEY_OK) count++;
        else if (k == KEY_CANCEL) break;
        g_api->delay(100);
    }
    return count;
}

/*
 * 蓄力充电用：连按 Z 计数，按其他键结束。
 * 控制台没有可靠的“按住”状态，这里保留原版的等价写法。
 */
static int mash_c(int budget)
{
    int count = 0;
    int i;

    for (i = 0; i < budget; i++)
    {
        if (g_api->key() != KEY_OK) break;
        count++;
    }
    return count;
}

/* ============ 暗影猫 ============ */

/* 捉迷藏：多轮猜方向，越早猜中奖励越高 */
static int game_shadow_hide(Pet *pet)
{
    int pos;
    int attempts = 0;
    int guess;

    g_api->ui_title("暗影捉迷藏");
    g_api->ui_print("\n  %s化作黑影消失了！\n", pet->name);
    g_api->ui_print("  选择它出现的方向：\n\n");
    g_api->ui_print("  ↑:上 ↓:下\n");
    g_api->ui_print("  ←:左 →:右\n\n");

    int pos_map[] = {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT};
    pos = pos_map[(int)g_api->rnd(0, 3)];
    
    while (1)
    {
        guess = g_api->key();
        attempts++;

        if (guess == pos)
        {
            int bonus = (attempts == 1) ? 60 : (attempts == 2 ? 40 : 25);
            g_api->ui_print("  抓到了！%s开心地蹭你~\n", pet->name);
            reward(pet, 1, bonus, 0);
            return 1;
        }
        if (g_api->is_headless() && attempts >= 6)
        {
            /* 无头模式按键恒定，猜不中就自动放弃，别把自检挂死 */
            g_api->ui_print("  （自检：自动放弃这一局）\n");
            reward(pet, 0, 0, 0);
            return 0;
        }
        g_api->ui_print("  扑了个空...再试试~\n");
    }
}

/* 影子模仿：记忆并复现 3 段方向序列 */
static int game_shadow_mimic(Pet *pet)
{
    int seq[3];
    int correct = 1;
    int i;

    g_api->ui_title("影子模仿");
    g_api->ui_print("\n  %s做出了动作！\n", pet->name);
    g_api->ui_print("  记住顺序：");

    for (i = 0; i < 3; i++)
    {
        int seq_map[] = {KEY_UP, KEY_DOWN, KEY_OK, KEY_CANCEL, KEY_TERM, KEY_LEFT, KEY_RIGHT};
        seq[i] = seq_map[(int)g_api->rnd(0, 6)];
        g_api->ui_print("%d ", seq[i]);
    }
    g_api->ui_print("\n  ↑:1 ↓:2 Z:3 X:4 C:5 ←:6 →:7\n");
    g_api->delay(2000);

    g_api->ui_title("影子模仿");
    g_api->ui_print("\n  按顺序复现：\n\n");

    for (i = 0; i < 3; i++)
    {
        if (g_api->key() != seq[i]) { correct = 0; break; }
    }

    if (correct)
    {
        g_api->ui_print("  完美复现！%s很兴奋~\n", pet->name);
        pet->personality[1] = clamp_stat((int)pet->personality[1] + 3);
        reward(pet, 1, 55, 0);
        return 1;
    }

    g_api->ui_print("  模仿失败...%s歪头看你\n", pet->name);
    reward(pet, 0, 0, 0);
    return 0;
}

/* ============ 冰霜兔 ============ */

/* 冰晶跳跃：单次方向猜测 */
static int game_ice_jump(Pet *pet)
{
    int jump_dir;
    int guess;
    int success;

    g_api->ui_title("冰晶跳跃");
    g_api->ui_print("\n  %s跳到了浮冰上！\n", pet->name);
    g_api->ui_print("  选择方向接住它：\n\n");
    g_api->ui_print("  ←:左 →:右\n\n");

    jump_dir = (int)g_api->rnd(KEY_LEFT, KEY_RIGHT);
    guess = g_api->key();
    success = (guess == jump_dir);

    if (success)
        g_api->ui_print("  稳稳接住！%s撒娇~\n", pet->name);
    else
        g_api->ui_print("  没接住...掉雪堆里~\n");

    reward(pet, success, success ? 50 : 15, success ? 10 : 5);
    return success;
}

/* 雪球大战：5 秒内限时连按投掷（原版真实时间） */
static int game_ice_snowball(Pet *pet)
{
    int count;

    g_api->ui_title("雪球大战");
    g_api->ui_print("\n  和%s打雪仗！\n", pet->name);
    g_api->ui_print("  5 秒内快速按 Z 投掷雪球！\n");
    g_api->ui_print("  按得越多越好，X 提前结束\n\n");

    count = mash_timed(5000);

    g_api->ui_title("雪球大战");
    g_api->ui_print("\n  投掷了%d个雪球！\n", count);

    if (count >= 8)
    {
        g_api->ui_print("  大获全胜！%s认输了~\n", pet->name);
        reward(pet, 1, 60, 0);
        return 1;
    }
    if (count >= 4)
    {
        g_api->ui_print("  势均力敌！很精彩~\n");
        reward(pet, 1, 40, 0);
        return 1;
    }

    g_api->ui_print("  %s的雪球更多...\n", pet->name);
    reward(pet, 0, 0, 0);
    return 0;
}

/* ============ 雷龙 ============ */

/* 雷电反应：随机等待后限时按键 */
static int game_thunder_react(Pet *pet)
{
    int key;
    int success;

    g_api->ui_title("雷电反应");
    g_api->ui_print("\n  %s身上闪烁雷电！\n", pet->name);
    g_api->ui_print("  闪光后按Z防御！\n\n");
    g_api->ui_print("  准备...\n");

    g_api->delay(g_api->rnd(1000, 3000));
    g_api->ui_print("  电闪！\n");

    key = g_api->key();
    success = (key == 3);

    if (success)
    {
        g_api->ui_print("  成功防御！%s很满意~\n", pet->name);
        pet->health = clamp_stat((int)pet->health + 5);
    }
    else
    {
        g_api->ui_print("  被电到了！%s调皮地笑~\n", pet->name);
        pet->health = clamp_stat((int)pet->health - 5);
    }
    reward(pet, success, success ? 50 : 25, 0);
    return success;
}

/* 蓄力充电：连按蓄力，15~25 完美、超过 25 过充（原版是按住 C 蓄力） */
static int game_thunder_charge(Pet *pet)
{
    int charge;

    g_api->ui_title("蓄力充电");
    g_api->ui_print("\n  帮%s充电！\n", pet->name);
    g_api->ui_print("  连按Z蓄力，按↓释放\n");
    g_api->ui_print("  蓄力 15~25 最完美\n\n");

    charge = mash_c(40);

    g_api->ui_title("蓄力结果");
    g_api->ui_print("\n  蓄力值：%d\n", charge);

    if (charge >= 15 && charge <= 25)
    {
        g_api->ui_print("  完美充电！%s活力满满~\n", pet->name);
        pet->energy = clamp_stat((int)pet->energy + 30);
        reward(pet, 1, 50, 0);
        return 1;
    }
    if (charge > 25)
    {
        g_api->ui_print("  过充了！%s有点晕~\n", pet->name);
        pet->energy = clamp_stat((int)pet->energy + 10);
        reward(pet, 0, 0, 0);
        return 0;
    }

    g_api->ui_print("  充电不足...再来~\n");
    reward(pet, 0, 0, 0);
    return 0;
}

/* ============ 宝藏鼠 ============ */

/* 寻宝：猜挖掘地点，中了给鲁尼 */
static int game_treasure_dig(Pet *pet)
{
    int treasure;
    int guess;
    int success;
    int coins;

    g_api->ui_title("寻宝大作战");
    g_api->ui_print("\n  %s闻到了宝藏气味！\n", pet->name);
    g_api->ui_print("  选择挖掘地点：\n\n");
    g_api->ui_print("  ←:草地 →:石堆\n");
    g_api->ui_print("  ↑:枯树 ↓:小溪\n\n");

    int treasure_map[] = {KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN};
    treasure = treasure_map[(int)g_api->rnd(0, 3)];
    guess = g_api->key();
    success = (guess == treasure);

    if (success)
    {
        coins = (int)g_api->rnd(10, 30);
        g_api->ui_print("  挖到了！+%d鲁尼！\n", coins);
        pet->coins += (uint32_t)coins;
        pet->personality[0] = clamp_stat((int)pet->personality[0] + 2);
    }
    else
    {
        g_api->ui_print("  什么都没找到...\n");
    }
    reward(pet, success, success ? 40 : 10, 0);
    return success;
}

/* 迷宫探险：3 轮方向选择，中 2 轮以上算成功 */
static int game_treasure_maze(Pet *pet)
{
    int correct = 0;
    int dir;
    int coins;
    int i;

    g_api->ui_title("迷宫探险");
    g_api->ui_print("\n  %s进入了迷宫！\n", pet->name);
    g_api->ui_print("  选择前进方向：\n\n");
    g_api->ui_print("  ←:左转 →:右转\n");
    g_api->ui_print("  ↑:直行 ↓:回头\n\n");

    /* 3 轮选择，每轮正确方向随机 */
    for (i = 0; i < 3; i++)
    {
        int dir_map[] = {KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN};
        dir = dir_map[(int)g_api->rnd(0, 3)];
        if (g_api->key() == dir) correct++;
    }

    g_api->ui_title("迷宫结果");
    g_api->ui_print("\n  正确次数：%d/3\n", correct);

    if (correct >= 2)
    {
        coins = (int)g_api->rnd(15, 40);
        g_api->ui_print("  找到出口！+%d鲁尼！\n", coins);
        pet->coins += (uint32_t)coins;
        reward(pet, 1, 45, 0);
        return 1;
    }

    g_api->ui_print("  迷路了...%s带你出来\n", pet->name);
    reward(pet, 0, 0, 0);
    return 0;
}

/* ============ 烈焰鸡 ============ */

/* 火焰接力：随机等待后限时按键 */
static int game_fire_catch(Pet *pet)
{
    int key;
    int success;

    g_api->ui_title("火焰接力");
    g_api->ui_print("\n  %s吐出小火球！\n", pet->name);
    g_api->ui_print("  闪光后按Z接住！\n\n");
    g_api->ui_print("  准备...\n");

    g_api->delay(g_api->rnd(800, 2300));
    g_api->ui_print("  火球！\n");

    key = g_api->key();
    success = (key == KEY_OK);

    if (success)
        g_api->ui_print("  完美接住！配合默契~\n");
    else
        g_api->ui_print("  火球熄灭了...\n");

    reward(pet, success, success ? 55 : 15, 0);
    return success;
}

/*
 * 烤食物：0~6000ms 内按 Z 起锅，和随机好的火候时刻比较。
 * 原版参数：完美 ±500ms、尚可 ±1500ms。
 */
static int game_fire_cook(Pet *pet)
{
    int perfect;
    uint32_t start;
    uint32_t elapsed = 0;
    int diff;

    g_api->ui_title("火焰烧烤");
    g_api->ui_print("\n  %s帮忙烤食物！\n", pet->name);
    g_api->ui_print("  火候正好时按 Z 起锅，↑继续烤\n");
    g_api->ui_print("  太早不熟 太晚会焦\n\n");
    g_api->ui_print("  开始烤...\n");

    perfect = (int)g_api->rnd(2000, 4000);
    start = g_api->now_ms();

    while (elapsed < 6000)
    {
        if (g_api->key_poll() == KEY_OK)
        {
            elapsed = g_api->now_ms() - start;
            break;
        }
        g_api->delay(100);
        elapsed = g_api->now_ms() - start;
    }

    diff = iabs((int)elapsed - perfect);

    g_api->ui_title("烧烤结果");

    if (elapsed >= 6000)
    {
        g_api->ui_print("  烤焦了...好可惜~\n");
        reward(pet, 0, 0, 0);
        return 0;
    }

    if (diff <= 500)
    {
        g_api->ui_print("  完美火候！美味~\n");
        pet->hunger = clamp_stat((int)pet->hunger + 20);
        reward(pet, 1, 50, 0);
        return 1;
    }
    if (diff <= 1500)
    {
        g_api->ui_print("  还不错！勉强能吃~\n");
        pet->hunger = clamp_stat((int)pet->hunger + 10);
        reward(pet, 1, 30, 0);
        return 1;
    }

    g_api->ui_print("  烤焦了...好可惜~\n");
    reward(pet, 0, 0, 0);
    return 0;
}

/* ============ 水灵鲸 ============ */

/* 水中泡泡：记忆并复现 3 段泡泡顺序（只 1~3） */
static int game_water_bubble(Pet *pet)
{
    int seq[3];
    int correct = 1;
    int i;

    g_api->ui_title("水中泡泡");
    g_api->ui_print("\n  %s吹出了一串泡泡！\n", pet->name);
    g_api->ui_print("  记住顺序：");

    for (i = 0; i < 3; i++)
    {
        seq[i] = (int)g_api->rnd(KEY_UP, KEY_RIGHT);
        g_api->ui_print("%d ", seq[i]);
    }
    g_api->ui_print("\n  ↑:1 ↓:2 Z:3 X:4 C:5 ←:6 →:7\n");
    g_api->delay(2000);

    g_api->ui_title("水中泡泡");
    g_api->ui_print("\n  按顺序输入：\n\n");

    for (i = 0; i < 3; i++)
    {
        if (g_api->key() != seq[i]) { correct = 0; break; }
    }

    if (correct)
    {
        g_api->ui_print("  全部正确！%s转圈~\n", pet->name);
        pet->health = clamp_stat((int)pet->health + 10);
        pet->personality[2] = clamp_stat((int)pet->personality[2] + 3);
        reward(pet, 1, 50, 0);
        return 1;
    }

    g_api->ui_print("  记错了...有点遗憾~\n");
    reward(pet, 0, 0, 0);
    return 0;
}

/* 潜水寻物：猜下潜深度，差一级也算找到点东西 */
static int game_water_dive(Pet *pet)
{
    int depth;
    int guess;
    int coins;

    g_api->ui_title("潜水寻物");
    g_api->ui_print("\n  %s准备潜水！\n", pet->name);
    g_api->ui_print("  选择下潜深度：\n\n");
    g_api->ui_print("  ←:浅 →:中\n");
    g_api->ui_print("  ↑:深 ↓:极深\n\n");

    int depth_map[] = {KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN};
    depth = depth_map[(int)g_api->rnd(0, 3)];
    guess = g_api->key();

    g_api->ui_title("潜水结果");

    if (guess == depth)
    {
        coins = (int)g_api->rnd(10, 35);
        g_api->ui_print("  找到宝物！+%d鲁尼~\n", coins);
        pet->coins += (uint32_t)coins;
        reward(pet, 1, 45, 0);
        return 1;
    }
    if (iabs(guess - depth) == 1)
    {
        g_api->ui_print("  差一点！%s还是找到了些东西\n", pet->name);
        reward(pet, 1, 25, 0);
        return 1;
    }

    g_api->ui_print("  深度不对...什么都没找到\n");
    reward(pet, 0, 0, 0);
    return 0;
}

/* ============ 疾风狐 ============ */

/* 疾风追逐：方向猜测，成功额外消耗 5 点饱食度 */
static int game_wind_chase(Pet *pet)
{
    int flee;
    int guess;
    int success;

    g_api->ui_title("疾风追逐");
    g_api->ui_print("\n  %s跑了出去！\n", pet->name);
    g_api->ui_print("  选择方向追上它：\n\n");
    g_api->ui_print("  ←:左 →:右\n");
    g_api->ui_print("  ↑:前 ↓:后\n\n");

    int dir_map[] = {KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN};
    flee = dir_map[(int)g_api->rnd(0, 3)];
    guess = g_api->key();
    success = (guess == flee);

    if (success)
        g_api->ui_print("  追上了！%s扑进你怀里~\n", pet->name);
    else
        g_api->ui_print("  %s跑太快了...\n", pet->name);

    reward(pet, success, success ? 50 : 20, 0);
    if (success) pet->hunger = clamp_stat((int)pet->hunger - 5);
    return success;
}

/* 疾风躲避：随机等待后限时按键 */
static int game_wind_dodge(Pet *pet)
{
    int key;
    int success;

    g_api->ui_title("疾风躲避");
    g_api->ui_print("\n  %s向你冲来！\n", pet->name);
    g_api->ui_print("  看到信号按Z躲避！\n\n");
    g_api->ui_print("  准备...\n");

    g_api->delay(g_api->rnd(1000, 2500));
    g_api->ui_print("  冲来了！\n");

    key = g_api->key();
    success = (key == KEY_OK);

    if (success)
    {
        g_api->ui_print("  完美躲避！%s很佩服~\n", pet->name);
        pet->personality[2] = clamp_stat((int)pet->personality[2] + 2);
        reward(pet, 1, 55, 0);
        return 1;
    }

    g_api->ui_print("  被撞倒了...好疼~\n");
    reward(pet, 0, 0, 0);
    return 0;
}

/* ============ 岩甲熊 ============ */

/* 岩石防御：三选一，拥抱最好、抚摸次之、轻拍失败 */
static int game_earth_defend(Pet *pet)
{
    int guess;

    g_api->ui_title("岩石防御");
    g_api->ui_print("\n  %s蜷缩成一团！\n", pet->name);
    g_api->ui_print("  选择互动方式：\n\n");
    g_api->ui_print("  ←:轻拍 →:抚摸\n");
    g_api->ui_print("  Z:拥抱\n\n");

    guess = g_api->key();

    if (guess == KEY_OK)
    {
        g_api->ui_print("  温暖拥抱！%s放下防御~\n", pet->name);
        pet->health = clamp_stat((int)pet->health + 10);
        reward(pet, 1, 55, 0);
        return 1;
    }
    if (guess == KEY_RIGHT)
    {
        g_api->ui_print("  轻轻抚摸，%s很舒服~\n", pet->name);
        reward(pet, 1, 40, 0);
        return 1;
    }

    g_api->ui_print("  %s的铠甲太硬了...\n", pet->name);
    reward(pet, 0, 0, 0);
    return 0;
}

/* 力量比拼：4 秒内限时连按推（原版真实时间） */
static int game_earth_push(Pet *pet)
{
    int count;

    g_api->ui_title("力量比拼");
    g_api->ui_print("\n  和%s比力气！\n", pet->name);
    g_api->ui_print("  4 秒内快速按 Z 推它！\n");
    g_api->ui_print("  按得越多越好，X 提前结束\n\n");

    count = mash_timed(4000);

    g_api->ui_title("比拼结果");
    g_api->ui_print("\n  推了%d下！\n", count);

    if (count >= 10)
    {
        g_api->ui_print("  力大无穷！%s认输了~\n", pet->name);
        pet->personality[2] = clamp_stat((int)pet->personality[2] + 3);
        reward(pet, 1, 60, 0);
        return 1;
    }
    if (count >= 5)
    {
        g_api->ui_print("  势均力敌！好精彩~\n");
        reward(pet, 1, 35, 0);
        return 1;
    }

    g_api->ui_print("  %s纹丝不动...\n", pet->name);
    reward(pet, 0, 0, 0);
    return 0;
}

/* ============ 通用玩具游戏 ============ */

/* 玩飞盘：方向猜测，成功额外消耗 10 点活力 */
static int game_frisbee(Pet *pet)
{
    int dir;
    int guess;
    int success;

    g_api->ui_title("玩飞盘");
    g_api->ui_print("\n  向%s扔出飞盘！\n", pet->name);
    g_api->ui_print("  选择扔出的方向：\n\n");
    g_api->ui_print("  ←:左 →:右\n");
    g_api->ui_print("  ↑:高 ↓:低\n\n");

    int dir_map[] = {KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN};
    dir = dir_map[(int)g_api->rnd(0, 3)];
    guess = g_api->key();
    success = (guess == dir);

    if (success)
    {
        g_api->ui_print("  %s接住了飞盘！好棒~\n", pet->name);
        reward(pet, 1, 45, 0);
        pet->energy = clamp_stat((int)pet->energy - 10);
    }
    else
    {
        g_api->ui_print("  没接住...%s追了半天\n", pet->name);
        reward(pet, 0, 0, 0);
    }
    g_api->delay(1000);
    return success;
}

/* 玩皮球：先按 C 滚球，等一会儿再按 C 接住 */
static int game_ball(Pet *pet)
{
    int key;
    int success;

    g_api->ui_title("玩皮球");
    g_api->ui_print("\n  和%s滚皮球！\n", pet->name);
    g_api->ui_print("  按Z滚球给对方\n");
    g_api->ui_print("  球回来时按Z接住！\n\n");

    g_api->ui_print("  滚球！按Z\n");
    while (g_api->key() != KEY_OK) { }

    g_api->delay(g_api->rnd(1000, 3000));
    g_api->ui_print("  球回来了！\n");

    key = g_api->key();
    success = (key == KEY_OK);

    if (success)
    {
        g_api->ui_print("  接住了！%s很开心~\n", pet->name);
        reward(pet, 1, 40, 0);
    }
    else
    {
        g_api->ui_print("  没接住...球滚走了~\n");
        reward(pet, 0, 0, 0);
    }
    g_api->delay(1000);
    return success;
}

/* 画画：选择题材，质量随机 */
static int game_draw(Pet *pet)
{
    static const char *const ART[] = { "美丽的风景画", "可爱的肖像画", "神秘的抽象画", "随意的涂鸦" };
    int choice;
    int quality;
    int idx;

    g_api->ui_title("画画时间");
    g_api->ui_print("\n  和%s一起画画！\n", pet->name);
    g_api->ui_print("  选择画什么：\n\n");
    g_api->ui_print("  ↑:风景 ↓:肖像\n");
    g_api->ui_print("  Z:抽象 X:涂鸦\n\n");

    choice = g_api->key();

    g_api->ui_title("作品完成");
    g_api->ui_print("\n  ");

    idx = (choice >= 1 && choice <= 4) ? choice - 1 : 3;
    g_api->ui_print("完成了%s！\n", ART[idx]);

    quality = (int)g_api->rnd(1, 10);
    if (quality >= 7)
    {
        g_api->ui_print("  画得真好！%s很满意~\n", pet->name);
        pet->personality[0] = clamp_stat((int)pet->personality[0] + 3);
        reward(pet, 1, 45, 0);
        g_api->delay(1000);
        return 1;
    }
    if (quality >= 4)
    {
        g_api->ui_print("  画得还不错~\n");
        reward(pet, 1, 30, 0);
        g_api->delay(1000);
        return 1;
    }

    g_api->ui_print("  画得有点...独特~\n");
    reward(pet, 0, 0, 0);
    g_api->delay(1000);
    return 0;
}

/* 打扮：选择风格，搭配随机；心情直加，不走 reward */
static int game_dress(Pet *pet)
{
    static const char *const STYLE[] = { "可爱风", "帅气风", "优雅风", "搞怪风" };
    int choice;
    int match;
    int idx;

    g_api->ui_title("打扮时间");
    g_api->ui_print("\n  给%s打扮！\n", pet->name);
    g_api->ui_print("  选择装饰风格：\n\n");
    g_api->ui_print("  ↑:可爱 ↓:帅气\n");
    g_api->ui_print("  Z:优雅 X:搞怪\n\n");

    choice = g_api->key();

    g_api->ui_title("打扮完成");
    g_api->ui_print("\n  ");

    idx = (choice >= 1 && choice <= 4) ? choice - 1 : 3;
    g_api->ui_print("%s的%s造型！\n", pet->name, STYLE[idx]);

    match = (int)g_api->rnd(1, 10);
    if (match >= 6)
    {
        g_api->ui_print("  好看极了！%s很开心~\n", pet->name);
        pet->personality[0] = clamp_stat((int)pet->personality[0] + 2);
        pet->happy = clamp_stat((int)pet->happy + 35);
        g_api->delay(1000);
        return 1;
    }

    g_api->ui_print("  %s不太喜欢这个风格...\n", pet->name);
    pet->happy = clamp_stat((int)pet->happy + 15);
    g_api->delay(1000);
    return 0;
}

/* ============ 游戏注册表 ============ */

/* 注册顺序即本模组的 Id 顺序（1~20）；pet_type=NULL 表示通用玩具游戏 */
static const Minigame_t GAMES[] = {
    /* 暗影猫 */
    { "暗影捉迷藏", "猜它从哪个方向出现",   "暗影猫", game_shadow_hide },
    { "影子模仿",   "复现三段动作顺序",     "暗影猫", game_shadow_mimic },
    /* 冰霜兔 */
    { "冰晶跳跃",   "接住浮冰上的它",       "冰霜兔", game_ice_jump },
    { "雪球大战",   "连按投掷比数量",       "冰霜兔", game_ice_snowball },
    /* 雷龙 */
    { "雷电反应",   "闪光后立刻防御",       "雷龙",   game_thunder_react },
    { "蓄力充电",   "连按蓄力别过充",       "雷龙",   game_thunder_charge },
    /* 宝藏鼠 */
    { "寻宝大作战", "猜挖掘地点找宝藏",     "宝藏鼠", game_treasure_dig },
    { "迷宫探险",   "三轮方向选择",         "宝藏鼠", game_treasure_maze },
    /* 烈焰鸡 */
    { "火焰接力",   "闪光后接住火球",       "烈焰鸡", game_fire_catch },
    { "火焰烧烤",   "火候正好时起锅",       "烈焰鸡", game_fire_cook },
    /* 水灵鲸 */
    { "水中泡泡",   "记住泡泡顺序",         "水灵鲸", game_water_bubble },
    { "潜水寻物",   "猜下潜深度",           "水灵鲸", game_water_dive },
    /* 疾风狐 */
    { "疾风追逐",   "猜它跑的方向",         "疾风狐", game_wind_chase },
    { "疾风躲避",   "冲来瞬间按 Z",         "疾风狐", game_wind_dodge },
    /* 岩甲熊 */
    { "岩石防御",   "选最暖的互动",         "岩甲熊", game_earth_defend },
    { "力量比拼",   "连按推它比力气",       "岩甲熊", game_earth_push },
    /* 通用玩具游戏 */
    { "玩飞盘",     "接住飞盘",             0,        game_frisbee },
    { "玩皮球",     "接住滚回来的球",       0,        game_ball },
    { "画画时间",   "选题材看发挥",         0,        game_draw },
    { "打扮时间",   "选风格看搭配",         0,        game_dress }
};

/* ============ 玩耍菜单 ============ */

static int menu_play(Pet *pet)
{
    SelectOpt_t *opts;
    Id *ids;
    uint64_t cap, n, i;
    Id picked;
    int sel;

    cap = g_api->count(KIND_MINIGAME);
    if (cap == 0)
    {
        g_api->ui_print("\n这只宠物暂时没有小游戏\n");
        pause_key();
        return 1;
    }

    ids = (Id *)malloc((size_t)cap * sizeof(*ids));
    opts = (SelectOpt_t *)calloc((size_t)cap, sizeof(*opts));
    if (!ids || !opts)
    {
        free(ids);
        free(opts);
        g_api->ui_print("\n小游戏列表分配失败\n");
        return 1;
    }

    n = g_api->minigame_list(pet->type_id, ids, cap);
    if (n == 0)
    {
        free(ids);
        free(opts);
        g_api->ui_print("\n这只宠物暂时没有小游戏\n");
        pause_key();
        return 1;
    }

    for (i = 0; i < n; i++)
    {
        const Minigame_t *g = (const Minigame_t *)g_api->get(KIND_MINIGAME, ids[i]);
        g_api->opt_text(&opts[i], "%s", (g && g->name) ? g->name : "?");
        opts[i].value = (int)i;
    }

    sel = g_api->ui_menu("玩耍", opts, n);
    free(opts); /* 文本已被 ui_menu 释放 */
    if (sel < 0 || (uint64_t)sel >= n)
    {
        free(ids);
        return 1;
    }
    picked = ids[sel];
    free(ids);

    g_api->ui_print("\n");
    g_api->minigame_run(pet, picked);

    pet->task_play++;
    if (pet->task_play >= 3) pet->task_complete |= TASK_BIT_PLAY;
    pet->total_play++;

    g_api->exp_add(pet, 8);
    g_api->ach_poll(pet);
    g_api->save(pet);
    pause_key();
    return 1;
}

/* ============ 模组入口 ============ */

static const Menu_t MENUS[] = {
    {"玩耍", "玩耍", 0, 50, MENU_FLAG_NONE, menu_play}
};

static const char *const REQUIRES[] = { "Core:宠物类型/暗影猫" };

MOD_EXPORT const ModInfo_t *mod_info(void)
{
    static const ModInfo_t info = { "Minigames", "1.1.0", MOD_API_VERSION, MOD_REQUIRES(REQUIRES) };
    return &info;
}

MOD_EXPORT int mod_init(const LoaderApi *api)
{
    if (!api) return fail("mod_init 收到空接口表");
    if (api->version != LOADER_API_VERSION)
        return fail("接口版本不符：模组按 v%u 编译，加载器是 v%u", (unsigned)MOD_API_VERSION, (unsigned)api->version);
    if (api->size < (uint32_t)sizeof(LoaderApi))
        return fail("LoaderApi 大小不足（%u < %u）：Defineds.h/Loader.h 与加载器不一致",
                    (unsigned)api->size, (unsigned)sizeof(LoaderApi));

    g_api = api;

    if (MOD_ADD(api, KIND_MINIGAME, GAMES) == ID_NONE)
        return fail("KIND_MINIGAME 注册失败：内容表已满，或 Minigame_t 尺寸与加载器不一致");
    if (MOD_ADD(api, KIND_MENU, MENUS) == ID_NONE)
        return fail("KIND_MENU 注册失败：内容表已满，或 Menu_t 尺寸与加载器不一致");

    /* 自检：注册后按名字能查回 Id */
    if (g_api->find(KIND_MINIGAME, "暗影捉迷藏") == ID_NONE)
        return fail("自检失败：注册后按名字查不到小游戏「暗影捉迷藏」");
    if (g_api->find(KIND_MINIGAME, "玩飞盘") == ID_NONE)
        return fail("自检失败：注册后按名字查不到小游戏「玩飞盘」");
    if (g_api->find(KIND_MENU, "玩耍") == ID_NONE)
        return fail("自检失败：注册后按名字查不到菜单「玩耍」");
    return 1;
}

MOD_EXPORT const char *mod_error(void)
{
    return g_err[0] ? g_err : "mod_init 失败（未提供原因）";
}

MOD_EXPORT void mod_exit(void)
{
    g_api = 0;
}
