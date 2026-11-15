/* ============================================================
 * EPets · 剧情模组（全部剧情）
 *   20 段剧情 = 领养流程 1 + 首次剧情 10 + 结识剧情 4 + 其他剧情 5
 *   5 个 NPC + 7 条术语
 *   注册 KIND_STORY / KIND_NPC / KIND_TERM，外加社交/教程菜单（KIND_MENU）
 *
 * 编译: cl /LD /utf-8 /I. Story.c /Fe:Story.dll
 *
 * 剧情 = 场景标签 + 对话页 + 可选条件；引擎负责播放、置位 story_flag。
 * 文本里的 {pet} 由引擎替换成当前宠物名；NPC 名是跨模组引用用的键。
 * 流程复杂的剧情（领养问答）只挂一个节点，在 on_play 里用 dialog() 自己跑。
 * 剧情条目一共 20 条（story_flag 是变长位图，条数不限）。
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

/* 旧版 pet.h 里的成长门槛，这里按同样的语义保留（不引入引擎头文件） */
#define LV_AMUSE_OPEN  12  /* 游乐园正式开放等级 */
#define LV_GOOD_END    100 /* 好结局: 满级 */
#define AGE_REBEL      10  /* 叛逆期开始: 10 天 */
#define AGE_REBEL_END  18  /* 叛逆期结束: 18 天（原版 rebellious 窗口） */
#define AGE_OLD_LETTER 20  /* 旧友的信: 20 天以后 */
#define AGE_100DAY     100 /* 百日纪念 */

/* 只读数组的元素个数（sizeof 是常量表达式，可直接当静态初值） */
#define COUNT_OF(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

/* 把数值钳到 MIN_STAT~MAX_STAT */
static uint8_t stat_of(int v)
{
    if (v < MIN_STAT) return (uint8_t)MIN_STAT;
    if (v > MAX_STAT) return (uint8_t)MAX_STAT;
    return (uint8_t)v;
}

/* 播一段没有选项的分页对话（取消也直接返回） */
static void say(const char *speaker, const char *const *pages, uint32_t n)
{
    g_api->dialog(speaker, pages, n, 0, 0);
}

/* ============ NPC ============ */

static const Npc_t NPCS[] = {
    {"向导", "  §S桥屋的向导。\n  性格很有特点,\n  总是神出鬼没。§0", 0, 0, 0},
    {"小春", "  §S桥屋前台。\n  永远在笑，\n  不知道是真开心还是职业笑容。§0",
     "  §S欢迎来到宠物园区！\n  我是小春，今天由我带你挑选小伙伴~§0",
     "  §S这只小家伙怎么样？\n  让我给你介绍介绍~§0",
     "  §S太好了！要好好照顾它哦~\n  有问题随时来找小春姐姐！§0"},
    {"老陈", "  §S桥屋后勤。\n  话不多，脸挺臭。但活干得利索。\n  大概是好人？§0",
     "  §S来了。我是老陈。\n  跟我走吧，带你看看园区里的宠物。§0",
     "  §S这只。看看合不合眼缘。\n  我来说说它的特点。§0",
     "  §S嗯，好好养。有什么事来园区找我。§0"},
    {"阿夜", "  §S桥屋训练员。\n  说话总带谜语,笑起来怪怪的。\n  有点怕他。§0",
     "  §S哦？新来的。。。\n  我是阿夜。这里的小家伙们都认识我。§0",
     "  §S嗯。。。这只在叫你呢。\n  它的故事，我知道一些。§0",
     "  §S它会选你，不简单。。。\n  走好，后会有期。§0"},
    {"老板娘", "  §S便利店老板娘。\n  说话很爽快，总是笑呵呵的。\n  *感觉什么都瞒不住她。§0", 0, 0, 0}
};

/* ============ 术语 ============ */

static const TermDef_t TERMS[] = {
    {"伴兽",     "  §S这个世界对宠物的\n  称呼。能干活能比试，\n  跟普通宠物不一样。§0"},
    {"桥屋",     "  §S养伴兽的机构。\n  也接差事, 做训练，\n  算是伴兽管理中心。§0"},
    {"司养人",   "  §S §OA5§0§S 给你起的称号。\n  *她自己编的，\n  专门管饲养的人。§0"},
    {"鲁尼",     "  §S这里的货币。\n  买东西都要用它。§0"},
    {"差事",     "  §S伴兽可以接的任务。\n  跑腿，搬运之类。\n  能赚鲁尼。§0"},
    {"记事册",   "  §S A5给你的册子。\n  记录你做过的事。\n  *她叫它记事册，\n  别问为什么。§0"},
    {"伴兽中心", "  §S就是桥屋。\n  * §OA5§0§S 有时候混着叫。§0"}
};

/* ============================================================
 *  领养流程文本
 *  整段流程挂在「领养流程」一个节点上，on_play 里按顺序跑完：
 *  序章 → 打招呼 → 自我介绍(Amber→A5) → Q1 循环问答 → 迷茫讲解
 *  → 领养邀请(2 选项) → [拒绝: 隐藏成就 + 游戏结束] → Q2 取名
 *  → 授予「司养人」称号 → Q3 对称号的反应 → 收尾
 * ============================================================ */

/* 序章 */
static const char *const P_PROLOGUE[] = {
    "  §S你不知道为什么\n  来到了这座城镇。§0",
    "  §S路过一家叫 §T桥屋§0§S 的机构，\n  门口的向导向你招手。。。§0"
};

/* 向导打招呼 */
static const char *const P_GUIDE_HELLO[] = {
    "  §S欢迎来到 §T桥屋§0§S 。\n  我是这里的向导。§0",
    "  §S新来的吧？\n  去挑一只 §T伴兽§0§S 做伴吧~§0"
};

/* 向导自我介绍: 我叫Amber, 也可以叫我A5 */
static const char *const P_GUIDE_INTRO[] = {
    "  §S对了，还没自我介绍。§0",
    "  §S我叫 §OAmber§0§S 。\n  也可以叫我 §OA5§0§S 。§0",
    "  §S*别问为什么是5不是其他数字。§0"
};

/* Q1 的三种提示语: 还没问过 / 问过一些 / 全问过了 */
static const char *const P_Q1_PROMPT_NEW[] = {
    "  §S挑 §T伴兽§0§S 之前，\n  有什么想问的吗？§0"
};
static const char *const P_Q1_PROMPT_SOME[] = {
    "  §S还有别的想问的吗？§0"
};
static const char *const P_Q1_PROMPT_ALL[] = {
    "  §S还有想问的吗？§0"
};

/* Q1 选项: RPG 式循环问答，可以反复问 */
static const StoryOpt_t O_ADOPT_Q1[] = {
    {"伴兽是什么？", 1},
    {"怎么养伴兽？", 2},
    {"能赚钱吗？",   3},
    {"没什么想问了", 4}
};

/* Q1 三个回答 */
static const char *const P_A1[] = {
    "  §S就是你们说的宠物，\n  不过这边都叫§T伴兽§0。§0"
};
static const char *const P_A2[] = {
    "  §S跟养小孩差不多。\n  饿了喂，脏了洗，\n  病了别硬扛。§0"
};
static const char *const P_A3[] = {
    "  §S它接 §T差事§0§S 赚的可能比你多。\n  *你是来养伴兽的还是找工作的？§0"
};

/* 迷茫讲解: 向导发现主控有点迷茫 */
static const char *const P_LOST_Q[] = {
    "  §S你看起来有点迷茫。§0",
    "  §S需要我给你讲讲这里的事吗？§0"
};
static const StoryOpt_t O_LOST[] = {
    {"需要",   1},
    {"不用了", 2}
};
static const char *const P_LOST_YES[] = {
    "  §S §T桥屋§0§S 是养 §T伴兽§0§S 的地方，也接 §T差事§0§S 。§0",
    "  §S §T伴兽§0§S 不是普通宠物，\n  它们能干活能比试。§0",
    "  §S你负责养好它就行，\n  其他的慢慢来。§0"
};
static const char *const P_LOST_NO[] = {
    "  §S也行。\n  有不懂的随时问。§0"
};

/* 领养邀请 */
static const char *const P_INVITE[] = {
    "  §S怎么样, 想不想养养看？§0",
    "  §S*我看咱俩有缘，\n  就送你一只比较特别的小家伙。§0",
    "  §S现在领养还有新手福利哦~§0"
};
static const StoryOpt_t O_INVITE[] = {
    {"领养",   1},
    {"不领养", 2}
};

/* 拒绝领养: 官方吐槽（之后解锁隐藏成就「铁石心肠」并结束游戏） */
static const char *const P_REFUSE[] = {
    "  §S。。。。。。§0",
    "  §S（ §OA5§0§S 歪了歪头，\n  似乎在认真思考你的选择）§0",
    "  §S不领养的话，\n  你玩\"这个游戏\"的目的是什么？§0",
    "  §S*不过嘛，我也不能强求你就是了。§0"
};

/* Q2: 取名字（名字已由主程序摇好，这里只走问答与反应） */
static const char *const P_Q2[] = {
    "  §S看样子它跟你\n  挺投缘的。§0",
    "  §S要取个名字吗？§0"
};
static const StoryOpt_t O_Q2[] = {
    {"好",         1},
    {"它自己取？", 2}
};
static const char *const P_Q2_OK[] = {
    "  §S那就这么定了。§0",
    "  §S以后就叫它 §P 吧。§0"
};
static const char *const P_Q2_SILLY[] = {
    "  §S。。。。。。它要是会取名，\n  就不需要你了。§0",
    "  §S（它打了个哈欠，\n   好像没什么意见）§0",
    "  §S行吧，那就叫 §P 好了。§0"
};

/* 授予「司养人」称号 */
static const char *const P_TITLE[] = {
    "  §S你对 §T伴兽§0§S 挺有天赋的。§0",
    "  §S以后我们就叫你 §T司养人§0§S 吧。§0"
};

/* Q3: 对称号的反应 */
static const char *const P_Q3[] = {
    "  §S怎么样？\n  以后就这么叫了。§0"
};
static const StoryOpt_t O_Q3[] = {
    {"认可",       1},
    {"那是什么？", 2},
    {"（点头）",     3}
};
static const char *const P_Q3_OK[] = {
    "  §S是吧？\n  挺有派头的。§0"
};
static const char *const P_Q3_WHAT[] = {
    "  §S就是我们编的。§0",
    "  §S专门管饲养的人就叫 §T司养人§0§S 。§0",
    "  §S挺好听的吧？\n  *反正已经这么叫了。§0"
};
static const char *const P_Q3_NOD[] = {
    "  §S好高冷啊。§0§C  §S话不多啊。行。§0",
    "  §S（对伴兽说）你的饲主挺酷的, 好好相处。§0"
};

/* 领养收尾 */
static const char *const P_ADOPT_END[] = {
    "  §S好了, 去吧。\n  有空来 §T桥屋§0§S 坐坐。§0"
};

/* ============================================================
 *  首次剧情文本
 * ============================================================ */

/* ---- 首次差事（按当值员工分 3 个分支 + 差事地点便利店） ---- */
static const char *const P_WORK_1[] = {
    "  §S来接差事？\n  正好有张跑腿单,\n  赚的不多但够一天饭钱。§0"
};
static const char *const P_WORK_2[] = {
    "  §S差事啊……\n  都是别人挑剩的。\n  要不给你张加急单？§0"
};
static const char *const P_WORK_3[] = {
    "  §S你说差事？\n  有意思。就让这只小家伙试试吧。§0",
    "  §S（从桌下抽出个木箱）§0"
};
static const char *const P_WORK_SHOP[] = {
    "  §S（差事地点是附近一家便利店）§0",
    "  §S（门口站着个爽朗的女人）§0",
    "  §S §O老板娘§0§S ：哟！\n  新面孔啊？\n  我这什么都卖，价格公道！§0"
};
static const Dialog_t D_WORK[] = {
    {"小春", "小春", P_WORK_1, COUNT_OF(P_WORK_1)},
    {"老陈", "老陈", P_WORK_2, COUNT_OF(P_WORK_2)},
    {"阿夜", "阿夜", P_WORK_3, COUNT_OF(P_WORK_3)},
    {0, "老板娘", P_WORK_SHOP, COUNT_OF(P_WORK_SHOP)}
};

/* ---- 首次出门 ---- */
static const char *const P_TRAVEL[] = {
    "  §S（第一次带它去远一点的地方）§0",
    "  §S §P 似乎有点兴奋。\n  希望能遇到有趣的事。§0"
};
static const Dialog_t D_TRAVEL[] = {
    {0, "司养人独白", P_TRAVEL, COUNT_OF(P_TRAVEL)}
};

/* ---- 首次商场 ---- */
static const char *const P_MALL[] = {
    "  §S（走到一家商场门口，\n   §OA5§0§S 正好在旁边）§0",
    "  §S §OA5§0§S ：这商场东西挺全，\n  食品玩具药品都有。\n  就是价格嘛……\n  死贵。§0§C   §S§OA5§0§S：这商场东西挺全，\n  食品玩具药品都有。\n  就是价格嘛……\n  你懂的。§0",
    "  §S（A5指了指招牌，\n  好像想说什么又咽了回去）§0"
};
static const Dialog_t D_MALL[] = {
    {0, "A5", P_MALL, COUNT_OF(P_MALL)}
};

/* ---- 首次医院 ---- */
static const char *const P_HOSPITAL[] = {
    "  §S（路过一家医院，\n   §OA5§0§S 从拐角冒出）§0",
    "  §S §OA5§0§S ：生病了就来这。\n  治一次200鲁尼,\n  不便宜————所以平时照顾好它。§0",
    "  §S（ §OA5§0§S 说完又不知去向了）§0"
};
static const Dialog_t D_HOSPITAL[] = {
    {0, "A5", P_HOSPITAL, COUNT_OF(P_HOSPITAL)}
};

/* ---- 首次游乐园（12 级正式开放） ---- */
static const char *const P_AMUSE[] = {
    "  §S（原来那片工地\n   建好了一座游乐园）§0",
    "  §S（ §OA5§0§S 站在门口，\n   好像等了很久）§0",
    "  §S §OA5§0§S ：哟，开张了。\n  旋转木马、过山车、摩天轮都有。\n  贵是贵了点,\n  不过它会很开心。§0",
    "  §S（ §OA5§0§S 推了你一把，\n   示意快进去）§0"
};
static const Dialog_t D_AMUSE[] = {
    {0, "A5", P_AMUSE, COUNT_OF(P_AMUSE)}
};

/* ---- 游乐园施工提示（12 级前） ---- */
static const char *const P_AMUSE_BUILD[] = {
    "  §S这里好像是一片工地。§0",
    "  §S不知道在建什么。§0",
    "  §S之后再来看看吧。§0"
};
static const Dialog_t D_AMUSE_BUILD[] = {
    {0, "？？？", P_AMUSE_BUILD, COUNT_OF(P_AMUSE_BUILD)}
};

/* ---- 首次应聘（打工） ---- */
static const char *const P_APPLY[] = {
    "  §S（带着伴兽来到\n   附近的便利店）§0",
    "  §S（门口贴着\n   「招工」的纸条）§0",
    "   §S老板娘§0§S ：哟！\n  来应聘的？\n  正好人手不够。§0",
    "   §S老板娘§0§S ：我这什么都卖，\n  价格公道，\n  就是忙不过来。§0",
    "   §S老板娘§0§S ：行,\n  就你了。\n  一天100鲁尼，干不干？§0",
    "  §S（点了点头）§0",
    "   §S老板娘§0§S ：那就这么说定了！\n  明天就来上班~§0"
};
static const Dialog_t D_APPLY[] = {
    {0, "便利店·应聘", P_APPLY, COUNT_OF(P_APPLY)}
};

/* ---- 首次公园 ---- */
static const char *const P_PARK[] = {
    "  §S（家附近的小公园，\n   §OA5§0§S 靠在树旁）§0",
    "  §S §OA5§0§S ：这公园\n  没什么特别的,\n  但它会很喜欢。§0",
    "  §S §OA5§0§S ：偶尔来逛逛，\n  说不定能捡到点\n  零碎东西。§0",
    "  §S（ §OA5§0§S 踢了踢脚边的草垛，好像藏着什么）§0"
};
static const Dialog_t D_PARK[] = {
    {0, "A5", P_PARK, COUNT_OF(P_PARK)}
};

/* ---- 首次洗浴（按当值员工） ---- */
static const char *const P_SPA_1[] = {
    "  §S我来帮你\n  给它洗个澡吧？\n  水温我已经调好了~§0"
};
static const char *const P_SPA_2[] = {
    "  §S这水是温泉水。\n  老伙计, 你家这只毛量真厚啊。§0"
};
static const char *const P_SPA_3[] = {
    "  §S。。。。。。好了。\n  (从水汽里出来,袖子全湿了)§0"
};
static const Dialog_t D_SPA[] = {
    {"小春", "小春", P_SPA_1, COUNT_OF(P_SPA_1)},
    {"老陈", "老陈", P_SPA_2, COUNT_OF(P_SPA_2)},
    {"阿夜", "阿夜", P_SPA_3, COUNT_OF(P_SPA_3)}
};

/* ---- 首次比试（按当值员工） ---- */
static const char *const P_BATTLE_1[] = {
    "  §S我想看看，\n  司养人训练出来的伴兽有多强！§0"
};
static const char *const P_BATTLE_2[] = {
    "  §S。。。。。。来点\n  温和的比试就好。\n  别伤着它们。§0"
};
static const char *const P_BATTLE_3[] = {
    "  §S你是真的来比试，\n  还是来让他们丢脸？\n  (意味深长地笑)§0"
};
static const Dialog_t D_BATTLE[] = {
    {"小春", "小春", P_BATTLE_1, COUNT_OF(P_BATTLE_1)},
    {"老陈", "老陈", P_BATTLE_2, COUNT_OF(P_BATTLE_2)},
    {"阿夜", "阿夜", P_BATTLE_3, COUNT_OF(P_BATTLE_3)}
};

/* ---- 首次记事册（A5 递册子 + 主控独白） ---- */
static const char *const P_ACH_1[] = {
    "  §S（正要离开桥屋,\n   背后传来声音）§0",
    "  §S §OA5§0§S ：哎————等一下！§0",
    "  §S（ §OA5§0§S 不知从哪冒了出来）§0",
    "  §S*差点忘了，\n  这个给你。§0",
    "  §S（ §OA5§0§S 递过来一本\n  看起来很旧的册子）§0",
    "  §S这东西嘛……你自己琢磨吧。§0",
    "  §S*它能记下你做过的一些事。§0",
    "  §S至于怎么用————呵, 你会发现的。§0",
    "  §S（ §OA5§0§S 转身走了，\n   好像还偷笑了一下）§0"
};
static const char *const P_ACH_2[] = {
    "  §S（翻开册子，\n  里面夹着一张便签）§0",
    "  §S*字迹很潦草，\n  大概是 §OA5§0§S 写的。§0",
    "  §S便签上写着：\n  \"恭喜你有了这本 §T司养人§0§S 手册。\n  我叫它 §T记事册§0§S 。\n  别问为什么。\"§0"
};
static const Dialog_t D_ACH[] = {
    {0, "A5", P_ACH_1, COUNT_OF(P_ACH_1)},
    {0, "司养人独白", P_ACH_2, COUNT_OF(P_ACH_2)}
};

/* ============================================================
 *  结识剧情文本（首次见面 + 结识提示）
 *  每段都挂在自己 NPC 的名下，只有该 NPC 当值时才会播。
 * ============================================================ */

static const char *const P_MEET_XIAOCHUN[] = {
    "  §S（前台站着个笑眯眯的女孩）§0",
    "  §S §O小春§0§S : 你好呀！我是小春~\n  以后多来找我玩哦！§0",
    "  §S（笑容很灿烂，\n  看不出是装的还是真的）§0",
    "  §S结识了 §O小春§0§S ！§0"
};
static const Dialog_t D_MEET_XIAOCHUN[] = {
    {"小春", "小春", P_MEET_XIAOCHUN, COUNT_OF(P_MEET_XIAOCHUN)}
};

static const char *const P_MEET_LAOCHEN[] = {
    "  §S（角落里坐着一个沉默的大叔）§0",
    "  §S §O老陈§0§S : 。。。。。。新来的？\n  嗯。\n  我姓陈。有事再叫我。§0",
    "  §S（说完就继续低头干活了）§0",
    "  §S结识了 §O老陈§0§S ！§0"
};
static const Dialog_t D_MEET_LAOCHEN[] = {
    {"老陈", "老陈", P_MEET_LAOCHEN, COUNT_OF(P_MEET_LAOCHEN)}
};

static const char *const P_MEET_AYE[] = {
    "  §S（训练场边站着一个黑衣青年）§0",
    "  §S §O阿夜§0§S : 。。。。。。你是新来的 §T司养人§0§S ？\n  *有意思。\n  我叫阿夜。§0",
    "  §S（笑了一下，\n   但眼睛没在笑）§0",
    "  §S结识了 §O阿夜§0§S ！§0"
};
static const Dialog_t D_MEET_AYE[] = {
    {"阿夜", "阿夜", P_MEET_AYE, COUNT_OF(P_MEET_AYE)}
};

static const char *const P_MEET_SHOP[] = {
    "  §S（便利店门口站着个爽朗的女人）§0",
    "  §S §O老板娘§0§S ：哟！新面孔啊？\n  我这什么都卖，价格公道！§0",
    "  §S*眼神很犀利，感觉什么都瞒不住她。§0",
    "  §S结识了 §O老板娘§0§S ！§0"
};
static const Dialog_t D_MEET_SHOP[] = {
    {"老板娘", "老板娘", P_MEET_SHOP, COUNT_OF(P_MEET_SHOP)}
};

/* ============================================================
 *  其他剧情文本
 * ============================================================ */

/* ---- 叛逆期（10 天以后，和好） ---- */
static const char *const P_REBEL[] = {
    "  §S（这段时间它一直\n  不怎么理你）§0",
    "  §S（叫名字也只是\n  看你一眼就转头）§0",
    "  §S（今天它主动蹭了蹭\n  你的手）§0",
    "  §S好像。。。和好了。§0"
};
static const Dialog_t D_REBEL[] = {
    {0, "司养人独白", P_REBEL, COUNT_OF(P_REBEL)}
};

/* ---- 百日纪念 ---- */
static const char *const P_100DAY[] = {
    "  §S不知不觉100天了。§0",
    "  §S桌上多了个小蛋糕，\n  不知谁放的。§0",
    "  §S蜡烛形状像只小兽。§0",
    "  §S伴兽安静坐在旁边，\n  看着烛光。§0",
    "  §S你觉得这样的日子，\n  能多一天就多一天。§0"
};
static const Dialog_t D_100DAY[] = {
    {0, "司养人独白", P_100DAY, COUNT_OF(P_100DAY)}
};

/* ---- 旧友的信 ---- */
static const char *const P_LETTER[] = {
    "  §S信箱里有一封\n  没署名的信。§0",
    "  §S字迹歪歪扭扭的。§0",
    "  §S\"你好。我是桥屋的\n  小家伙。\"§0",
    "  §S\"听说你以前的伴兽\n  去了很远的地方。\"§0",
    "  §S\"它让我跟你说:\n  别难过, 它在那边挺好的。\"§0",
    "  §S\"现在这只也不错。\n  好好待它。\"§0"
};
static const StoryOpt_t O_LETTER[] = {
    {"收好信",             1},
    {"谁写的？",           2},
    {"......它不可能写信。", 3}
};
/* 三个选项各自的后半段 */
static const char *const P_LETTER_A[] = {
    "  §S折好放进口袋。\n  心里暖暖的。§0"
};
static const char *const P_LETTER_B[] = {
    "  §S翻遍信封,\n  没署名。§0",
    "  §S桥屋的小家伙...\n  是哪只呢。§0"
};
static const char *const P_LETTER_C[] = {
    "  §S信纸上有几道水渍。§0",
    "  §S不知道是雨水\n  还是别的。§0"
};
static const Dialog_t D_LETTER[] = {
    {0, "司养人独白", P_LETTER, COUNT_OF(P_LETTER)}
};

/* ---- 好结局（等级 100 的预告） ---- */
static const char *const P_GOOD_END[] = {
    "  §S你的 §T伴兽§0§S §P \n  终于满级了。§0",
    "  §S（它看起来\n  非常健康快乐，\n  你照顾得很好）§0",
    "  §S* §OA5§0§S 不知什么时候出现在身后。§0",
    "  §S* §OA5§0§S : 辛苦了，\n   §T司养人§0§S 。\n  *它可能……会比你想的先走一步。§0"
};
static const Dialog_t D_GOOD_END[] = {
    {0, "A5", P_GOOD_END, COUNT_OF(P_GOOD_END)}
};

/* ============================================================
 *  触发条件（on_check 拿到的是只读 Pet，只能看数值）
 * ============================================================ */

/* 叛逆期: 落在 10~18 天窗口内（原版 update_pet 里 rebellious = age 10..18） */
static int chk_rebel(const Pet *pet)
{
    return pet->age >= AGE_REBEL && pet->age <= AGE_REBEL_END;
}

/* 百日纪念: 活着满 100 天 */
static int chk_100day(const Pet *pet)
{
    return !pet->dead && pet->age >= AGE_100DAY;
}

/* 旧友的信: 曾经失去过伴兽（Pet.lost_friend，由宿主在宠物离世时置位）且已满 20 天 */
static int chk_old_letter(const Pet *pet)
{
    return pet->lost_friend && pet->age >= AGE_OLD_LETTER;
}

/* 好结局预告: 满级 */
static int chk_good_end(const Pet *pet)
{
    return pet->level >= LV_GOOD_END;
}

/* 游乐园正式开放: 满 12 级 */
static int chk_amuse_open(const Pet *pet)
{
    return pet->level >= LV_AMUSE_OPEN;
}

/* 游乐园还在施工: 没到 12 级 */
static int chk_amuse_build(const Pet *pet)
{
    return pet->level < LV_AMUSE_OPEN;
}

/* ============================================================
 *  领养流程（一个节点跑完整段流程）
 * ============================================================ */

/* Q1: RPG 式循环问答，问到"没什么想问了"才结束 */
static void adopt_q1(void)
{
    int asked[3] = {0, 0, 0};
    int last = 0;  /* 上一次问的是第几个问题 */
    int same = 0;  /* 连续问同一个问题的次数 */

    for (;;)
    {
        const char *const *prompt;
        int all_asked = asked[0] && asked[1] && asked[2];
        int r;

        /* 都问过了 / 问过一些 / 还没问过，三种提示语 */
        if (all_asked)
            prompt = P_Q1_PROMPT_ALL;
        else if (asked[0] || asked[1] || asked[2])
            prompt = P_Q1_PROMPT_SOME;
        else
            prompt = P_Q1_PROMPT_NEW;

        r = g_api->dialog("A5", prompt, 1, O_ADOPT_Q1, COUNT_OF(O_ADOPT_Q1));
        if (r == 0 || r == 4) return;  /* 取消 或 选"没什么想问了" */

        if (r >= 1 && r <= 3) asked[r - 1] = 1;

        if (r == 1)      say("A5", P_A1, COUNT_OF(P_A1));
        else if (r == 2) say("A5", P_A2, COUNT_OF(P_A2));
        else             say("A5", P_A3, COUNT_OF(P_A3));

        /* 防呆: 同一个问题连问三次就当他问完了（也避免自动化测试里死循环） */
        if (r == last) same++;
        else { last = r; same = 1; }
        if (same >= 3) return;
    }
}

/* 迷茫讲解: 主控看起来有点迷茫，问要不要讲讲这里的事 */
static void adopt_q_lost(void)
{
    int r = g_api->dialog("A5", P_LOST_Q, COUNT_OF(P_LOST_Q), O_LOST, COUNT_OF(O_LOST));

    if (r == 1) say("A5", P_LOST_YES, COUNT_OF(P_LOST_YES));
    else        say("A5", P_LOST_NO, COUNT_OF(P_LOST_NO));
}

/* 拒绝领养: 解锁隐藏成就「铁石心肠」，然后置死，让主程序结束游戏 */
static void adopt_refuse(Pet *pet)
{
    Id ach;
    int k;

    say("A5", P_REFUSE, COUNT_OF(P_REFUSE));

    ach = g_api->find(KIND_ACHIEVEMENT, "铁石心肠");
    if (ach != ID_NONE) g_api->ach_unlock(pet, ach);

    g_api->ui_title("游戏结束");
    g_api->ui_print("\n  §S（你就这样离开了\n  桥屋。）§0\n\n");
    g_api->ui_print("  Z:继续\n");
    for (;;)
    {
        k = g_api->key();
        if (k == 3 || k == 4) break;
    }

    pet->dead = 1;
    pet->death_reason = "拒绝了领养";
    (void)g_api->save(pet); /* 拒绝领养也是死亡状态，立即落盘 */
}

/* 领养流程主流程，返回 1=领养成功 0=拒绝/中断 */
static int story_adopt(Pet *pet, int choice)
{
    int r;

    (void)choice;

    /* 序章 + 向导打招呼 */
    say("序章", P_PROLOGUE, COUNT_OF(P_PROLOGUE));
    say("向导", P_GUIDE_HELLO, COUNT_OF(P_GUIDE_HELLO));

    /* 向导自我介绍: Amber → A5（之后的对话标题都变成 A5） */
    say("向导", P_GUIDE_INTRO, COUNT_OF(P_GUIDE_INTRO));

    /* Q1: 挑伴兽之前的循环问答 */
    adopt_q1();

    /* 迷茫讲解 */
    adopt_q_lost();

    /* 领养邀请: 2 选项，取消/不领养都走拒绝分支 */
    r = g_api->dialog("A5", P_INVITE, COUNT_OF(P_INVITE), O_INVITE, COUNT_OF(O_INVITE));
    if (r != 1)
    {
        adopt_refuse(pet);
        return 0;
    }

    /* Q2: 取名字 */
    r = g_api->dialog("A5", P_Q2, COUNT_OF(P_Q2), O_Q2, COUNT_OF(O_Q2));
    if (r == 2) say("A5", P_Q2_SILLY, COUNT_OF(P_Q2_SILLY));
    else        say("A5", P_Q2_OK, COUNT_OF(P_Q2_OK));

    /* 授予「司养人」称号，再让玩家反应 */
    say("A5", P_TITLE, COUNT_OF(P_TITLE));
    r = g_api->dialog("A5", P_Q3, COUNT_OF(P_Q3), O_Q3, COUNT_OF(O_Q3));

    if (r == 2)      say("A5", P_Q3_WHAT, COUNT_OF(P_Q3_WHAT));
    else if (r == 3) say("A5", P_Q3_NOD, COUNT_OF(P_Q3_NOD));
    else             say("A5", P_Q3_OK, COUNT_OF(P_Q3_OK));  /* 0=取消，按认可处理 */

    /* 收尾 */
    say("A5", P_ADOPT_END, COUNT_OF(P_ADOPT_END));
    return 1;
}

/* ============================================================
 *  其他剧情的 on_play（只做数值/物品效果，文本都在表里）
 * ============================================================ */

/* 叛逆期结束: 心情 +20 */
static int story_rebel(Pet *pet, int choice)
{
    (void)choice;
    pet->happy = stat_of(pet->happy + 20);
    return 1;
}

/* 百日纪念: 送一件稀有纪念品（原版的"亮石"） */
static int story_100day(Pet *pet, int choice)
{
    Id jewel;

    (void)choice;
    jewel = g_api->find(KIND_ITEM, "琉璃饰品");
    if (jewel != ID_NONE) g_api->inv_add(pet, jewel, 1);
    return 1;
}

/* 旧友的信: 收好 +30 / 追问 +20 / 不信 心情-10 健康+10 */
static int story_old_letter(Pet *pet, int choice)
{
    if (choice == 1)
    {
        say("司养人独白", P_LETTER_A, COUNT_OF(P_LETTER_A));
        pet->happy = stat_of(pet->happy + 30);
    }
    else if (choice == 3)
    {
        say("司养人独白", P_LETTER_C, COUNT_OF(P_LETTER_C));
        pet->happy = stat_of(pet->happy - 10);
        pet->health = stat_of(pet->health + 10);
    }
    else
    {
        say("司养人独白", P_LETTER_B, COUNT_OF(P_LETTER_B));
        pet->happy = stat_of(pet->happy + 20);
    }
    return 1;
}

/* ============================================================
 *  剧情表
 *  场景标签: adopt/work/work_apply/travel/mall/hospital/amuse/
 *            park/spa/battle/center/home/social/idle
 *  注册顺序即本模组的 Id 顺序（1~20），story_flag 第 Id 位是标志位。
 * ============================================================ */

static const Story_t STORIES[] = {
    /* ---- 领养流程: 文本都在 on_play 里，dialogs 留空 ---- */
    {"领养流程",   "adopt",      0,                 0,              0,
     0, 0, story_adopt},

    /* ---- 首次剧情 ---- */
    {"首次差事",   "work",       0,                 D_WORK,         COUNT_OF(D_WORK),
     0, 0, 0},
    {"首次出门",   "travel",     0,                 D_TRAVEL,       COUNT_OF(D_TRAVEL),
     0, 0, 0},
    {"首次商场",   "mall",       0,                 D_MALL,         COUNT_OF(D_MALL),
     0, 0, 0},
    {"首次医院",   "hospital",   0,                 D_HOSPITAL,     COUNT_OF(D_HOSPITAL),
     0, 0, 0},
    {"首次游乐园", "amuse",      chk_amuse_open,     D_AMUSE,        COUNT_OF(D_AMUSE),
     0, 0, 0},
    {"游乐园施工", "amuse",      chk_amuse_build,   D_AMUSE_BUILD,  COUNT_OF(D_AMUSE_BUILD),
     0, 0, 0},
    {"首次应聘",   "work_apply", 0,                 D_APPLY,        COUNT_OF(D_APPLY),
     0, 0, 0},
    {"首次公园",   "park",       0,                 D_PARK,         COUNT_OF(D_PARK),
     0, 0, 0},
    {"首次洗浴",   "spa",        0,                 D_SPA,          COUNT_OF(D_SPA),
     0, 0, 0},
    {"首次比试",   "battle",     0,                 D_BATTLE,       COUNT_OF(D_BATTLE),
     0, 0, 0},
    {"首次记事册", "center",     0,                 D_ACH,          COUNT_OF(D_ACH),
     0, 0, 0},

    /* ---- 结识剧情 ---- */
    {"结识小春",   "center",     0,                 D_MEET_XIAOCHUN, COUNT_OF(D_MEET_XIAOCHUN),
     0, 0, 0},
    {"结识老陈",   "center",     0,                 D_MEET_LAOCHEN,  COUNT_OF(D_MEET_LAOCHEN),
     0, 0, 0},
    {"结识阿夜",   "center",     0,                 D_MEET_AYE,      COUNT_OF(D_MEET_AYE),
     0, 0, 0},
    {"结识老板娘", "work",       0,                 D_MEET_SHOP,     COUNT_OF(D_MEET_SHOP),
     0, 0, 0},

    /* ---- 其他剧情 ---- */
    {"叛逆期",     "idle",       chk_rebel,         D_REBEL,        COUNT_OF(D_REBEL),
     0, 0, story_rebel},
    {"百日纪念",   "idle",       chk_100day,        D_100DAY,       COUNT_OF(D_100DAY),
     0, 0, story_100day},
    {"旧友的信",   "idle",       chk_old_letter,    D_LETTER,       COUNT_OF(D_LETTER),
     O_LETTER, COUNT_OF(O_LETTER), story_old_letter},
    {"好结局",     "idle",       chk_good_end,      D_GOOD_END,     COUNT_OF(D_GOOD_END),
     0, 0, 0}
};

/* ============================================================
 *  社交菜单（拜访 / 送礼）
 * ============================================================ */

static void pause_key(void)
{
    g_api->ui_print("\nZ:继续\n");
    g_api->key();
}

/* 随机一个有价物品（商店货） */
static Id random_priced_item(void)
{
    uint64_t n = g_api->count(KIND_ITEM), i;

    for (i = 0; i < 16 && n > 0; i++)
    {
        Id id = (Id)g_api->rnd64(1, n);
        const Item *it = (const Item *)g_api->get(KIND_ITEM, id);
        if (it && it->price > 0) return id;
    }
    return ID_NONE;
}

/* 随机一个纪念品（price == 0） */
static Id random_souvenir(void)
{
    uint64_t n = g_api->count(KIND_ITEM), i;

    for (i = 0; i < 16 && n > 0; i++)
    {
        Id id = (Id)g_api->rnd64(1, n);
        const Item *it = (const Item *)g_api->get(KIND_ITEM, id);
        if (it && it->price == 0) return id;
    }
    return ID_NONE;
}

/* 拜访 */
static int do_visit(Pet *pet, const char *fname)
{
    int r = (int)g_api->rnd(0, 99);
    const char *pname = pet->name ? pet->name : "它";

    pet->total_visit++;
    g_api->ui_title("拜访");
    g_api->ui_print("\n  §S带着 §O%s§0§S 去 §O%s§0§S 家做客~§0\n", pname, fname);

    if (r < 35)
    {
        uint32_t gift = g_api->rnd(20, 60);
        pet->happy = stat_of(pet->happy + 25);
        pet->coins += gift;
        g_api->ui_print("\n  §S两只宠物玩得很开心！\n  临走时对方主人送了 §O%u§0§S 鲁尼~§0\n", gift);
    }
    else if (r < 60)
    {
        pet->happy = stat_of(pet->happy + 5);
        g_api->ui_print("\n  §S两只宠物性格不合，互不理睬。。。§0\n");
    }
    else if (r < 80)
    {
        Id id = random_priced_item();
        const Item *it = (id != ID_NONE) ? (const Item *)g_api->get(KIND_ITEM, id) : NULL;
        if (it)
        {
            g_api->inv_add(pet, id, 1);
            g_api->ui_print("\n  §S对方主人送了你 §T%s§0§S x1§0\n", it->name);
        }
        else g_api->ui_print("\n  §S对方主人笑了笑。§0\n");
    }
    else
    {
        pet->happy = stat_of(pet->happy - 10);
        pet->health = stat_of(pet->health - 5);
        g_api->ui_print("\n  §S§O%s§0§S 被 §O%s§0§S 欺负了...§0\n", pname, fname);
    }

    g_api->exp_add(pet, 10);
    g_api->ach_poll(pet);
    g_api->save(pet);
    pause_key();
    return 1;
}

/* 送礼 */
static int do_gift(Pet *pet, const char *fname, int charm)
{
    Id id = g_api->inv_pick(pet, I_FOOD, "选礼物");
    const Item *it;
    int ok;

    if (id == ID_NONE) id = g_api->inv_pick(pet, I_CLEANING, "选礼物");
    if (id == ID_NONE) return 1;

    it = (const Item *)g_api->get(KIND_ITEM, id);
    if (!it) return 1;
    if (!g_api->inv_rm(pet, id, 1)) { g_api->ui_warn("数量不足"); return 1; }

    ok = (int)g_api->rnd(1, 100) < charm + pet->personality[0];
    g_api->ui_title("送礼结果");
    g_api->ui_print("\n  §S把 §T%s§0§S 送给了 §O%s§0§S ~§0\n", it->name, fname);

    if (ok)
    {
        Id souv = random_souvenir();
        const Item *si = (souv != ID_NONE) ? (const Item *)g_api->get(KIND_ITEM, souv) : NULL;
        g_api->ui_ok("   %s 非常喜欢！", fname);
        if (si) { g_api->inv_add(pet, souv, 1); g_api->ui_print("\n  §S对方回赠了 §T%s§0§S 。§0\n", si->name); }
        pet->happy = stat_of(pet->happy + 20);
        pet->total_gift_ok++;
    }
    else
    {
        g_api->ui_warn("  %s 礼貌地收下了...", fname);
        pet->happy = stat_of(pet->happy + 5);
    }

    g_api->exp_add(pet, 12);
    g_api->ach_poll(pet);
    g_api->save(pet);
    pause_key();
    return 1;
}

static int menu_social(Pet *pet)
{
    uint64_t guard = 0;

    while (1)
    {
        uint64_t total = g_api->count(KIND_NPC), i, cnt = 0;
        SelectOpt_t *opts, acts[3];
        int sel, act;
        const Npc_t *npc;
        const char *fname;
        int charm;

        if (g_api->is_headless() && ++guard > 6) return 1;

        if (total == 0)
        {
            g_api->ui_print("\n  还没有认识谁\n");
            pause_key();
            return 1;
        }

        opts = (SelectOpt_t *)calloc((size_t)total, sizeof(*opts));
        if (!opts) return 1;

        for (i = 1; i <= total; i++)
        {
            const Npc_t *n = (const Npc_t *)g_api->get(KIND_NPC, (Id)i);
            if (!n) continue;
            g_api->opt_text(&opts[cnt], "%s", n->name);
            opts[cnt].value = (int)i;
            cnt++;
        }
        if (cnt == 0)
        {
            free(opts);
            g_api->ui_print("\n  还没有认识谁\n");
            pause_key();
            return 1;
        }

        sel = g_api->ui_menu("认识的人", opts, cnt);
        free(opts); /* 文本已被 ui_menu 释放 */
        if (sel < 0) return 1;

        npc = (const Npc_t *)g_api->get(KIND_NPC, (Id)sel);
        if (!npc) return 1;

        fname = g_api->rand_name(G_FEMALE, NULL);
        if (!fname) fname = "对方";
        charm = (int)g_api->rnd(30, 95);

        g_api->ui_title(npc->name);
        g_api->ui_print("\n  %s\n", npc->desc ? npc->desc : "（没什么可说的）");

        memset(acts, 0, sizeof(acts));
        g_api->opt_text(&acts[0], "拜访"); acts[0].value = 1;
        g_api->opt_text(&acts[1], "送礼"); acts[1].value = 2;
        g_api->opt_text(&acts[2], "返回"); acts[2].value = 0;
        act = g_api->ui_menu("想做什么", acts, 3);

        if (act == 1) (void)do_visit(pet, fname);
        else if (act == 2) (void)do_gift(pet, fname, charm);
    }
}

/* ============================================================
 *  教程菜单（5 个主题，文案按当前键位）
 * ============================================================ */

static int tut_basic(Pet *pet)
{
    const char *pages[3];

    (void)pet;
    pages[0] = "  §S（ §OA5§0§S 突然冒了出来）§0";
    pages[1] = "  §S基本操作很简单，\n  记住四个键就行。\n  菜单: ↑/↓ 选择\n  Z 确认  X 返回§0";
    pages[2] = "  §S对话: ↑ 上页 ↓ 下页\n  X 跳过\n  绿色术语按 C 查看解释§0";
    g_api->dialog("教程", pages, 3, NULL, 0);
    return 1;
}

static int tut_item(Pet *pet)
{
    const char *pages[3];

    (void)pet;
    pages[0] = "  §S（ §OA5§0§S 探过头来）§0";
    pages[1] = "  §S消耗品用一次就没了。玩具可以反复使用。§0";
    pages[2] = "  §S↑/↓ 选物品\n  Z 使用  X 返回§0";
    g_api->dialog("教程", pages, 3, NULL, 0);
    return 1;
}

static int tut_center(Pet *pet)
{
    const char *pages[3];

    (void)pet;
    pages[0] = "  §S（ §OA5§0§S 从身后冒出）§0";
    pages[1] = "  §S §T桥屋§0§S 里可以训练，洗浴，寄养。\n  对战需要伴兽长大才能参加。§0";
    pages[2] = "  §S选服务→Z确认\n  X 返回桥屋菜单§0";
    g_api->dialog("教程", pages, 3, NULL, 0);
    return 1;
}

static int tut_travel(Pet *pet)
{
    const char *pages[3];

    (void)pet;
    pages[0] = "  §S（ §OA5§0§S 不知从哪冒了出来）§0";
    pages[1] = "  §S旅游需要伴兽长大才行。\n  出发前选好物品。§0";
    pages[2] = "  §S旅途中可能遇到各种事件，\n  Z 继续  X 返回§0";
    g_api->dialog("教程", pages, 3, NULL, 0);
    return 1;
}

static int tut_battle(Pet *pet)
{
    const char *pages[2];

    (void)pet;
    pages[0] = "  §S（ §OA5§0§S 不知从哪冒了出来）§0";
    pages[1] = "  §S对战就是让伴兽比试。\n  ↑/↓ 选行动 Z 确认§0";
    g_api->dialog("教程", pages, 2, NULL, 0);
    return 1;
}

/* ============ 入口 ============ */

static const Menu_t MENUS[] = {
    {"社交",          "社交",       0,      100, MENU_FLAG_NONE, menu_social},
    {"教程",          "教程",       0,      140, MENU_FLAG_NONE, 0},
    {"教程·基本操作", "基本操作",   "教程", 10,  MENU_FLAG_NONE, tut_basic},
    {"教程·背包",     "背包/物品",  "教程", 20,  MENU_FLAG_NONE, tut_item},
    {"教程·桥屋",     "桥屋",       "教程", 30,  MENU_FLAG_NONE, tut_center},
    {"教程·旅游",     "旅游",       "教程", 40,  MENU_FLAG_NONE, tut_travel},
    {"教程·对战",     "对战",       "教程", 50,  MENU_FLAG_NONE, tut_battle}
};

static const char *const REQUIRES[] = { "Core:物品/琉璃饰品", "Achievements:成就/铁石心肠" };

MOD_EXPORT const ModInfo_t *mod_info(void)
{
    static const ModInfo_t info = { "Story", "2.1.0", MOD_API_VERSION, MOD_REQUIRES(REQUIRES) };
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

    if (MOD_ADD(api, KIND_STORY, STORIES) == ID_NONE)
        return fail("KIND_STORY 注册失败：内容表已满，或 Story_t 尺寸与加载器不一致");
    if (MOD_ADD(api, KIND_NPC, NPCS) == ID_NONE)
        return fail("KIND_NPC 注册失败：内容表已满，或 Npc_t 尺寸与加载器不一致");
    if (MOD_ADD(api, KIND_TERM, TERMS) == ID_NONE)
        return fail("KIND_TERM 注册失败：内容表已满，或 TermDef_t 尺寸与加载器不一致");
    if (MOD_ADD(api, KIND_MENU, MENUS) == ID_NONE)
        return fail("KIND_MENU 注册失败：内容表已满，或 Menu_t 尺寸与加载器不一致");

    /* 自检：剧情里引用的 NPC / 剧情名必须真的注册上了 */
    if (g_api->find(KIND_NPC, "小春") == ID_NONE)
        return fail("自检失败：NPC「小春」没有注册上");
    if (g_api->find(KIND_NPC, "老板娘") == ID_NONE)
        return fail("自检失败：NPC「老板娘」没有注册上");
    if (g_api->find(KIND_STORY, "领养流程") == ID_NONE)
        return fail("自检失败：剧情「领养流程」没有注册上");
    if (g_api->find(KIND_STORY, "首次记事册") == ID_NONE)
        return fail("自检失败：剧情「首次记事册」没有注册上");
    if (g_api->find(KIND_MENU, "社交") == ID_NONE)
        return fail("自检失败：菜单「社交」没有注册上");
    if (g_api->find(KIND_MENU, "教程") == ID_NONE)
        return fail("自检失败：菜单「教程」没有注册上");

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
