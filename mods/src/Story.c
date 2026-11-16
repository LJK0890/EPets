/* ============================================================
 * EPets · 剧情模组（全部剧情）
 *   20 段剧情 = 领养流程 1 + 首次剧情 10 + 结识剧情 4 + 其他剧情 5
 *   5 个 NPC + 7 条术语
 *   注册 KIND_STORY / KIND_NPC / KIND_TERM，外加社交/教程菜单（KIND_MENU）
 *
 * 编译: cl /LD /utf-8 /I. Story.c /Fe:Story.dll
 *
 * 剧情 = 场景标签 + 文本 + 可选条件；引擎负责播放、置位 story_flag。
 * 文本用引擎的格式符：
 *   $C 清屏   $P 等 Z   $p 停顿   $K/$k 关键词   $N/$n 名字   $E/$e  吐槽
 *   $A/$a 分支   $W/$w 循环   $F/$f 函数   %NAME% 宠物名   %TYPE% 宠物种类
 * 需要 C 侧效果的选择（领养/拒绝、旧友的信）仍用 StoryOpt_t；
 * 纯文本分支（Q1 问答、取名、对称号的反应）直接写在文本里。
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

/* 播一段对话（没有选项；文本里的 $P / $C 控制节奏） */
static void say(const char *speaker, const char *const *pages, uint32_t n)
{
    g_api->dialog(speaker, pages, n, 0, 0);
}

/* ============ 文本函数（$f / $F 用，id 从 0 开始） ============ */

/* id 0：宠物等级 —— 返回 0 表示结果是数字 */
static uint64_t fn_level(Pet *pet, void *ret)
{
    *(uint64_t *)ret = pet ? (uint64_t)pet->level : 0;
    return 0;
}

/* id 1：宠物名字 —— 返回串长表示结果是字符串 */
static uint64_t fn_name(Pet *pet, void *ret)
{
    const char *n = (pet && pet->name) ? pet->name : "它";

    *(const char **)ret = n;
    return (uint64_t)strlen(n);
}

static const TextFunc_t TEXT_FUNCS[] = { fn_level, fn_name };

/* ============ NPC ============ */

static const Npc_t NPCS[] = {
    {"向导", "$K桥屋$k 的向导。$L性格很有特点，$L总是神出鬼没。", 0, 0, 0},
    {"小春", "$K桥屋$k 前台。$L永远在笑，$L不知道是真开心还是职业笑容。",
     "欢迎来到宠物园区！$L我是 $N小春$n ，今天由我带你挑选小伙伴~",
     "这只小家伙怎么样？$L让我给你介绍介绍~",
     "太好了！要好好照顾它哦~$L有问题随时来找小春姐姐！"},
    {"老陈", "$K桥屋$k 后勤。$L话不多，脸挺臭。但活干得利索。$L$E大概是好人？$e ",
     "来了。我是 $N老陈$n 。$L跟我走吧，带你看看园区里的宠物。",
     "这只。看看合不合眼缘。$L我来说说它的特点。",
     "嗯，好好养。有什么事来园区找我。"},
    {"阿夜", "$K桥屋$k 训练员。$L说话总带谜语，笑起来怪怪的。$L$E有点怕他。$e ",
     "哦？新来的……$L我是 $N阿夜$n 。这里的小家伙们都认识我。",
     "嗯……这只在叫你呢。$L它的故事，我知道一些。",
     "它会选你，不简单……$L走好，后会有期。"},
    {"老板娘", "便利店老板娘。$L说话很爽快，总是笑呵呵的。$L$E感觉什么都瞒不住她。$e ", 0, 0, 0}
};

/* ============ 术语 ============ */

static const TermDef_t TERMS[] = {
    {"伴兽",     "这个世界对宠物的称呼。$L能干活能比试，$L跟普通宠物不一样。"},
    {"桥屋",     "养 $K伴兽$k 的机构。$L也接 $K差事$k ，做训练，$L算是伴兽管理中心。"},
    {"司养人",   "$NA5$n 给你起的称号。$L$E她自己编的，$e $L专门管饲养的人。"},
    {"鲁尼",     "这里的货币。$L买东西都要用它。"},
    {"差事",     "$K伴兽$k 可以接的任务。$L跑腿，搬运之类。$L能赚 $K鲁尼$k 。"},
    {"记事册",   "$NA5$n 给你的册子。$L记录你做过的事。$L$E她叫它记事册，$e $L$E别问为什么。$e "},
    {"伴兽中心", "就是 $K桥屋$k 。$L$E$NA5$n 有时候混着叫。$e "}
};

/* ============================================================
 *  领养流程文本
 *  序章 → 向导欢迎/自我介绍 → Q1 循环问答（$w）→ 迷茫讲解（$a）
 *  → 领养邀请（C 侧选择）→ Q2 取名（$a）→ 授予「司养人」（$a）→ 收尾
 * ============================================================ */

/* 序章 */
static const char *const P_PROLOGUE[] = {
    "$C你不知道为什么$L来到了这座城镇。$P",
    "$C路过一家叫 $K桥屋$k 的机构，$L门口的向导向你招手……$P"
};

/* 向导：欢迎 + 自我介绍（Amber → A5） */
static const char *const P_GUIDE[] = {
    "$C欢迎来到 $K桥屋$k 。$L我是这里的向导。$P"
    "$C新来的吧？$L去挑一只 $K伴兽$k 做伴吧~$P"
    "$C对了，还没自我介绍。$P"
    "$C我叫 $NAmber$n 。$L也可以叫我 $NA5$n 。$P"
    "$C$E别问为什么是5不是其他数字。$e $P"
};

/*
 * Q1：挑伴兽之前的循环问答（$w）。
 *   $w 后面依次是：问题内容块、选项逻辑块（4 项）、后续内容逻辑块（3 项）。
 *   选项数 = 内容数 + 1，最后一项「没什么想问了」结束循环。
 *   每轮先渲染问题（$C 清屏），选完播对应回答（$P 等 Z），然后重新出选项。
 */
static const char *const P_Q1[] = {
    "$w$[$C挑 $K伴兽$k 之前，有什么想问的吗？$]"
    "${"
    "$[伴兽是什么？$]"
    "$[怎么养伴兽？$]"
    "$[能赚钱吗？$]"
    "$[没什么想问了$]"
    "$}"
    "${"
    "$[$C就是你们说的宠物，$L不过这边都叫 $K伴兽$k 。$P$]"
    "$[$C跟养小孩差不多。$L饿了喂，脏了洗，$L病了别硬扛。$P$]"
    "$[$C它接 $K差事$k 赚的可能比你多。$P$C$E你是来养伴兽的还是找工作的？$e $P$]"
    "$}"
};

/* 迷茫讲解：$a —— 先渲染问题，再按选项播放不同内容 */
static const char *const P_LOST[] = {
    "$C$E（向导歪了歪头）$e $L你看起来有点迷茫。$P"
    "$C$a$[需要我给你讲讲这里的事吗？$]"
    "${"
    "$[需要$]"
    "$[不用了$]"
    "$}"
    "${"
    "$[$C$K桥屋$k 是养 $K伴兽$k 的地方，$L也接 $K差事$k 。$P"
    "$C$K伴兽$k 不是普通宠物，$L它们能干活能比试。$P"
    "$C你负责养好它就行，$L其他的慢慢来。$P$]"
    "$[$C也行。$L有不懂的随时问。$P$]"
    "$}"
};

/* 领养邀请（要领选择结果，仍走 StoryOpt_t） */
static const char *const P_INVITE[] = {
    "$C怎么样，想不想养养看？$P",
    "$C$E我看咱俩有缘，$L就送你一只比较特别的小家伙。$e $P"
    "$C现在领养还有新手福利哦~$P"
};
static const StoryOpt_t O_INVITE[] = {
    {"领养",   1},
    {"不领养", 2}
};

/* 拒绝领养：官方吐槽（之后解锁隐藏成就「铁石心肠」并结束游戏） */
static const char *const P_REFUSE[] = {
    "$C……$P",
    "$C（ $NA5$n 歪了歪头，$L似乎在认真思考你的选择）$P"
    "$C不领养的话，$L你玩“这个游戏”的目的是什么？$P"
    "$C$E不过嘛，我也不能强求你就是了。$e $P"
};

/* Q2：取名字（名字由主程序摇好，这里只走问答与反应） */
static const char *const P_Q2[] = {
    "$C看样子它跟你$L挺投缘的。$P"
    "$C$a$[要取个名字吗？$]"
    "${"
    "$[好$]"
    "$[它自己取？$]"
    "$}"
    "${"
    "$[$C那就这么定了。$L以后就叫它 %NAME% 吧。$P$]"
    "$[$C……它要是会取名，$L就不需要你了。$P"
    "$C（它打了个哈欠，$L 好像没什么意见）$P"
    "$C行吧，那就叫 $N%NAME%$n 好了。$P$]"
    "$}"
};

/* 授予「司养人」称号 */
static const char *const P_TITLE[] = {
    "$C你对 $K伴兽$k 挺有天赋的。$P"
    "$C以后我们就叫你 $K司养人$k 吧。$P"
};

/* Q3：对称号的反应 */
static const char *const P_Q3[] = {
    "$C$a$[怎么样？以后就这么叫了。$]"
    "${"
    "$[认可$]"
    "$[那是什么？$]"
    "$[（点头）$]"
    "$}"
    "${"
    "$[$C是吧？$L挺有派头的。$P$]"
    "$[$C就是我们编的。$P"
    "$C专门管饲养的人$L就叫 $K司养人$k 。$P"
    "$C挺好听的吧？$L$E反正已经这么叫了。$e $P$]"
    "$[$C$E好高冷啊。$e $c话不多啊，行$P"
    "$C（对伴兽说）$L你的饲主挺酷的，$L好好相处。$P$]"
    "$}"
};

/* 领养收尾 */
static const char *const P_ADOPT_END[] = {
    "$C好了，去吧。$L有空来 $K桥屋$k 坐坐。$P"
};

/* ============================================================
 *  首次剧情文本
 * ============================================================ */

/* ---- 首次差事（按当值员工分 3 个分支 + 差事地点便利店） ---- */
static const char *const P_WORK_1[] = {
    "$C来接差事？$L正好有张跑腿单，$L赚的不多但够一天饭钱。$P"
};
static const char *const P_WORK_2[] = {
    "$C差事啊……$L都是别人挑剩的。$L要不给你张加急单？$P"
};
static const char *const P_WORK_3[] = {
    "$C你说差事？$L有意思。就让这只小家伙试试吧。$P"
    "$C（从桌下抽出个木箱）$P"
};
static const char *const P_WORK_SHOP[] = {
    "$C（差事地点是附近一家便利店）$P"
    "$C（门口站着个爽朗的女人）$P"
    "$C$N老板娘$n ：哟！$L新面孔啊？$L我这什么都卖，价格公道！$P"
};
static const Dialog_t D_WORK[] = {
    {"小春", "小春", P_WORK_1, COUNT_OF(P_WORK_1)},
    {"老陈", "老陈", P_WORK_2, COUNT_OF(P_WORK_2)},
    {"阿夜", "阿夜", P_WORK_3, COUNT_OF(P_WORK_3)},
    {0, "老板娘", P_WORK_SHOP, COUNT_OF(P_WORK_SHOP)}
};

/* ---- 首次出门：$F 按等级从 3 段描写里挑一段 ---- */
static const char *const P_TRAVEL[] = {
    "$C（第一次带它去远一点的地方）$P",
    "$C$F$[0$]${"
    "$[%NAME% 东张西望，$L什么都好奇。$]"
    "$[%NAME% 走得比你还快，$L好像知道要去哪。$]"
    "$[%NAME% 一路都在打哈欠，$L但尾巴没停过。$]"
    "$}$P"
};
static const Dialog_t D_TRAVEL[] = {
    {0, "司养人独白", P_TRAVEL, COUNT_OF(P_TRAVEL)}
};

/* ---- 首次商场 ---- */
static const char *const P_MALL[] = {
    "$C（走到一家商场门口，$L $NA5$n 正好在旁边）$P"
    "$C$NA5$n ：这商场东西挺全，$L食品玩具药品都有。$L就是价格嘛……死贵。$P"
    "$C（ $NA5$n 指了指招牌，$L好像想说什么又咽了回去）$P"
};
static const Dialog_t D_MALL[] = {
    {0, "A5", P_MALL, COUNT_OF(P_MALL)}
};

/* ---- 首次医院 ---- */
static const char *const P_HOSPITAL[] = {
    "$C（路过一家医院，$L $NA5$n 从拐角冒出）$P"
    "$C$NA5$n ：生病了就来这。$L治一次200 $K鲁尼$k ，$L不便宜————所以平时照顾好它。$P"
    "$C（ $NA5$n 说完又不知去向了）$P"
};
static const Dialog_t D_HOSPITAL[] = {
    {0, "A5", P_HOSPITAL, COUNT_OF(P_HOSPITAL)}
};

/* ---- 首次游乐园（12 级正式开放） ---- */
static const char *const P_AMUSE[] = {
    "$C（原来那片工地$L 建好了一座游乐园）$P"
    "$C（ $NA5$n 站在门口，$L 好像等了很久）$P"
    "$C$NA5$n ：哟，开张了。$L旋转木马、过山车、摩天轮都有。$L贵是贵了点，$L不过它会很开心。$P"
    "$C（ $NA5$n 推了你一把，$L 示意快进去）$P"
};
static const Dialog_t D_AMUSE[] = {
    {0, "A5", P_AMUSE, COUNT_OF(P_AMUSE)}
};

/* ---- 游乐园施工提示（12 级前） ---- */
static const char *const P_AMUSE_BUILD[] = {
    "$C这里好像是一片工地。$P"
    "$C不知道在建什么。$P"
    "$C之后再来看看吧。$P"
};
static const Dialog_t D_AMUSE_BUILD[] = {
    {0, "？？？", P_AMUSE_BUILD, COUNT_OF(P_AMUSE_BUILD)}
};

/* ---- 首次应聘（打工） ---- */
static const char *const P_APPLY[] = {
    "$C（带着伴兽来到$L 附近的便利店）$P"
    "$C（门口贴着$L 「招工」的纸条）$P"
    "$C$N老板娘$n ：哟！$L来应聘的？$L正好人手不够。$P"
    "$C$N老板娘$n ：我这什么都卖，$L价格公道，$L就是忙不过来。$P"
    "$C$N老板娘$n ：行，$L就你了。$L一天100 $K鲁尼$k ，干不干？$P"
    "$C（点了点头）$P"
    "$C$N老板娘$n ：那就这么说定了！$L明天就来上班~$P"
};
static const Dialog_t D_APPLY[] = {
    {0, "便利店·应聘", P_APPLY, COUNT_OF(P_APPLY)}
};

/* ---- 首次公园 ---- */
static const char *const P_PARK[] = {
    "$C（家附近的小公园，$L $NA5$n 靠在树旁）$P"
    "$C$NA5$n ：这公园$L没什么特别的，$L但它会很喜欢。$P"
    "$C$NA5$n ：偶尔来逛逛，$L说不定能捡到点$L零碎东西。$P"
    "$C（ $NA5$n 踢了踢脚边的草垛，$L 好像藏着什么）$P"
};
static const Dialog_t D_PARK[] = {
    {0, "A5", P_PARK, COUNT_OF(P_PARK)}
};

/* ---- 首次洗浴（按当值员工） ---- */
static const char *const P_SPA_1[] = {
    "$C我来帮你$L给它洗个澡吧？$L水温我已经调好了~$P"
};
static const char *const P_SPA_2[] = {
    "$C这水是温泉水。$L老伙计，你家这只毛量真厚啊。$P"
};
static const char *const P_SPA_3[] = {
    "$C……好了。$L（从水汽里出来，袖子全湿了）$P"
};
static const Dialog_t D_SPA[] = {
    {"小春", "小春", P_SPA_1, COUNT_OF(P_SPA_1)},
    {"老陈", "老陈", P_SPA_2, COUNT_OF(P_SPA_2)},
    {"阿夜", "阿夜", P_SPA_3, COUNT_OF(P_SPA_3)}
};

/* ---- 首次比试（按当值员工） ---- */
static const char *const P_BATTLE_1[] = {
    "$C我想看看，$L$K司养人$k 训练出来的伴兽有多强！$P"
};
static const char *const P_BATTLE_2[] = {
    "$C……来点$L温和的比试就好。$L别伤着它们。$P"
};
static const char *const P_BATTLE_3[] = {
    "$C你是真的来比试，$L还是来让他们丢脸？$L$E（意味深长地笑）$e $P"
};
static const Dialog_t D_BATTLE[] = {
    {"小春", "小春", P_BATTLE_1, COUNT_OF(P_BATTLE_1)},
    {"老陈", "老陈", P_BATTLE_2, COUNT_OF(P_BATTLE_2)},
    {"阿夜", "阿夜", P_BATTLE_3, COUNT_OF(P_BATTLE_3)}
};

/* ---- 首次记事册（A5 递册子 + 主控独白） ---- */
static const char *const P_ACH_1[] = {
    "$C（正要离开桥屋，$L 背后传来声音）$P"
    "$C$NA5$n ：哎————等一下！$P"
    "$C（ $NA5$n 不知从哪冒了出来）$P"
    "$C$E差点忘了，$L这个给你。$e $P"
    "$C（ $NA5$n 递过来一本$L看起来很旧的册子）$P"
    "$C这东西嘛……你自己琢磨吧。$P"
    "$C$E它能记下你做过的一些事。$e $P"
    "$C至于怎么用————呵，你会发现的。$P"
    "$C（ $NA5$n 转身走了，$L 好像还偷笑了一下）$P"
};
static const char *const P_ACH_2[] = {
    "$C（翻开册子，$L里面夹着一张便签）$P"
    "$C$E字迹很潦草，$L大概是 $NA5$n 写的。$e $P"
    "$C便签上写着：$L“恭喜你有了这本 $K司养人$k 手册。$L我叫它 $K记事册$k 。$L别问为什么。”$P"
};
static const Dialog_t D_ACH[] = {
    {0, "A5", P_ACH_1, COUNT_OF(P_ACH_1)},
    {0, "司养人独白", P_ACH_2, COUNT_OF(P_ACH_2)}
};

/* ============================================================
 *  结识剧情文本（首次见面）
 *  每段都挂在自己 NPC 的名下，只有该 NPC 当值时才会播。
 * ============================================================ */

static const char *const P_MEET_XIAOCHUN[] = {
    "$C（前台站着个笑眯眯的女孩）$P"
    "$C$N小春$n ：你好呀！我是小春~$L以后多来找我玩哦！$P"
    "$C（笑容很灿烂，$L看不出是装的还是真的）$P"
    "$C结识了 $N小春$n ！$P"
};
static const Dialog_t D_MEET_XIAOCHUN[] = {
    {"小春", "小春", P_MEET_XIAOCHUN, COUNT_OF(P_MEET_XIAOCHUN)}
};

static const char *const P_MEET_LAOCHEN[] = {
    "$C（角落里坐着一个沉默的大叔）$P"
    "$C$N老陈$n ：……新来的？$L嗯。$L我姓陈。有事再叫我。$P"
    "$C（说完就继续低头干活了）$P"
    "$C结识了 $N老陈$n ！$P"
};
static const Dialog_t D_MEET_LAOCHEN[] = {
    {"老陈", "老陈", P_MEET_LAOCHEN, COUNT_OF(P_MEET_LAOCHEN)}
};

static const char *const P_MEET_AYE[] = {
    "$C（训练场边站着一个黑衣青年）$P"
    "$C$N阿夜$n ：……你是新来的 $K司养人$k ？$L$E有意思。$e $L我叫阿夜。$P"
    "$C（笑了一下，$L 但眼睛没在笑）$P"
    "$C结识了 $N阿夜$n ！$P"
};
static const Dialog_t D_MEET_AYE[] = {
    {"阿夜", "阿夜", P_MEET_AYE, COUNT_OF(P_MEET_AYE)}
};

static const char *const P_MEET_SHOP[] = {
    "$C（便利店门口站着个爽朗的女人）$P"
    "$C$N老板娘$n ：哟！新面孔啊？$L我这什么都卖，价格公道！$P"
    "$C$E眼神很犀利，$L感觉什么都瞒不住她。$e $P"
    "$C结识了 $N老板娘$n ！$P"
};
static const Dialog_t D_MEET_SHOP[] = {
    {"老板娘", "老板娘", P_MEET_SHOP, COUNT_OF(P_MEET_SHOP)}
};

/* ============================================================
 *  其他剧情文本
 * ============================================================ */

/* ---- 叛逆期（10 天以后，和好） ---- */
static const char *const P_REBEL[] = {
    "$C（这段时间它一直$L不怎么理你）$P"
    "$C（叫 $f$[1$] 也只是$L看你一眼就转头）$P"
    "$C（今天它主动蹭了蹭$L你的手）$P"
    "$C好像……和好了。$P"
};
static const Dialog_t D_REBEL[] = {
    {0, "司养人独白", P_REBEL, COUNT_OF(P_REBEL)}
};

/* ---- 百日纪念 ---- */
static const char *const P_100DAY[] = {
    "$C不知不觉100天了。$P"
    "$C桌上多了个小蛋糕，$L不知谁放的。$P"
    "$C蜡烛形状像只小兽。$P"
    "$C%NAME% 安静坐在旁边，$L看着烛光。$P"
    "$C你觉得这样的日子，$L能多一天就多一天。$P"
};
static const Dialog_t D_100DAY[] = {
    {0, "司养人独白", P_100DAY, COUNT_OF(P_100DAY)}
};

/* ---- 旧友的信 ---- */
static const char *const P_LETTER[] = {
    "$C信箱里有一封$L没署名的信。$P"
    "$C字迹歪歪扭扭的。$P"
    "$C“你好。我是 $K桥屋$k 的$L小家伙。”$P"
    "$C“听说你以前的伴兽$L去了很远的地方。”$P"
    "$C“它让我跟你说：$L别难过，它在那边挺好的。”$P"
    "$C“现在这只也不错。$L好好待它。”$P"
};
static const StoryOpt_t O_LETTER[] = {
    {"收好信",             1},
    {"谁写的？",           2},
    {"......它不可能写信。", 3}
};
/* 三个选项各自的后半段 */
static const char *const P_LETTER_A[] = {
    "$C折好放进口袋。$L心里暖暖的。$P"
};
static const char *const P_LETTER_B[] = {
    "$C翻遍信封，$L没署名。$P"
    "$C桥屋的小家伙……$L是哪只呢。$P"
};
static const char *const P_LETTER_C[] = {
    "$C信纸上有几道水渍。$P"
    "$C不知道是雨水$L还是别的。$P"
};
static const Dialog_t D_LETTER[] = {
    {0, "司养人独白", P_LETTER, COUNT_OF(P_LETTER)}
};

/* ---- 好结局（等级 100 的预告；$f 插入等级） ---- */
static const char *const P_GOOD_END[] = {
    "$C你的 $K伴兽$k %NAME%$L已经 Lv.$f$[0$] 了。$P"
    "$C（它看起来$L非常健康快乐，$L你照顾得很好）$P"
    "$C$E$NA5$n 不知什么时候出现在身后。$e $P"
    "$C$NA5$n ：辛苦了，$L$K司养人$k 。$L$E它可能……会比你想的先走一步。$e $P"
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

/* 拒绝领养: 解锁隐藏成就「铁石心肠」，然后置死，让主程序结束游戏 */
static void adopt_refuse(Pet *pet)
{
    Id ach;
    int k;

    say("A5", P_REFUSE, COUNT_OF(P_REFUSE));

    ach = g_api->find(KIND_ACHIEVEMENT, "铁石心肠");
    if (ach != ID_NONE) g_api->ach_unlock(pet, ach);

    g_api->ui_title("游戏结束");
    g_api->ui_print("\n  （你就这样离开了\n  $K桥屋$k 。）\n\n");
    g_api->ui_print("Z:继续\n");
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

    /* 序章 + 向导打招呼 + 自我介绍 */
    say("序章", P_PROLOGUE, COUNT_OF(P_PROLOGUE));
    say("向导", P_GUIDE, COUNT_OF(P_GUIDE));

    /* Q1: 挑伴兽之前的循环问答（文本里的 $w 自己跑循环） */
    say("A5", P_Q1, COUNT_OF(P_Q1));

    /* 迷茫讲解（文本里的 $a） */
    say("A5", P_LOST, COUNT_OF(P_LOST));

    /* 领养邀请: 2 选项，取消/不领养都走拒绝分支 */
    r = g_api->dialog("A5", P_INVITE, COUNT_OF(P_INVITE), O_INVITE, COUNT_OF(O_INVITE));
    if (r != 1)
    {
        adopt_refuse(pet);
        return 0;
    }

    /* Q2 取名 + 授予称号 + Q3 对称号的反应（都是纯文本分支） */
    say("A5", P_Q2, COUNT_OF(P_Q2));
    say("A5", P_TITLE, COUNT_OF(P_TITLE));
    say("A5", P_Q3, COUNT_OF(P_Q3));

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
    g_api->ui_print("\n  带着 $N%s$n 去 $N%s$n 家做客~\n", pname, fname);

    if (r < 35)
    {
        uint32_t gift = g_api->rnd(20, 60);
        pet->happy = stat_of(pet->happy + 25);
        pet->coins += gift;
        g_api->ui_print("\n  两只宠物玩得很开心！\n  临走时对方主人送了 $N%u$n $K鲁尼$k ~\n", gift);
    }
    else if (r < 60)
    {
        pet->happy = stat_of(pet->happy + 5);
        g_api->ui_print("\n  $E两只宠物性格不合，\n  互不理睬……$e \n");
    }
    else if (r < 80)
    {
        Id id = random_priced_item();
        const Item *it = (id != ID_NONE) ? (const Item *)g_api->get(KIND_ITEM, id) : NULL;
        if (it)
        {
            g_api->inv_add(pet, id, 1);
            g_api->ui_print("\n  对方主人送了你 $K%s$k x1\n", it->name);
        }
        else g_api->ui_print("\n  对方主人笑了笑。\n");
    }
    else
    {
        pet->happy = stat_of(pet->happy - 10);
        pet->health = stat_of(pet->health - 5);
        g_api->ui_print("\n  $N%s$n 被 $N%s$n 欺负了……\n", pname, fname);
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
    g_api->ui_print("\n  把 $K%s$k 送给了 $N%s$n ~\n", it->name, fname);

    if (ok)
    {
        Id souv = random_souvenir();
        const Item *si = (souv != ID_NONE) ? (const Item *)g_api->get(KIND_ITEM, souv) : NULL;
        g_api->ui_ok(" %s 非常喜欢！", fname);
        if (si) { g_api->inv_add(pet, souv, 1); g_api->ui_print("\n  对方回赠了 $K%s$k 。\n", si->name); }
        pet->happy = stat_of(pet->happy + 20);
        pet->total_gift_ok++;
    }
    else
    {
        g_api->ui_warn("%s 礼貌地收下了...", fname);
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
    static const char *const PAGES[] = {
        "$C$E（ $NA5$n 突然冒了出来）$e $P"
        "$C基本操作很简单，$L记住几个键就行。$L菜单：↑/↓ 选择$LZ 确认  X 返回$P"
        "$C对话：Z 继续  X 跳过$L$K绿色关键词$k 按 C 查看解释$P"
    };

    (void)pet;
    g_api->dialog("教程", PAGES, COUNT_OF(PAGES), NULL, 0);
    return 1;
}

static int tut_item(Pet *pet)
{
    static const char *const PAGES[] = {
        "$C$E（ $NA5$n 探过头来）$e $P"
        "$C消耗品用一次就没了。$L玩具可以反复使用。$P"
        "$C↑/↓ 选物品$LZ 使用  X 返回$P"
    };

    (void)pet;
    g_api->dialog("教程", PAGES, COUNT_OF(PAGES), NULL, 0);
    return 1;
}

static int tut_center(Pet *pet)
{
    static const char *const PAGES[] = {
        "$C$E（ $NA5$n 从身后冒出）$e $P"
        "$C$K桥屋$k 里可以训练，洗浴，寄养。$L对战需要伴兽长大才能参加。$P"
        "$C选服务→Z确认$LX 返回桥屋菜单$P"
    };

    (void)pet;
    g_api->dialog("教程", PAGES, COUNT_OF(PAGES), NULL, 0);
    return 1;
}

static int tut_travel(Pet *pet)
{
    static const char *const PAGES[] = {
        "$C$E（ $NA5$n 不知从哪冒了出来）$e $P"
        "$C旅游需要伴兽长大才行。$L出发前选好物品。$P"
        "$C旅途中可能遇到各种事件，$LZ 继续  X 返回$P"
    };

    (void)pet;
    g_api->dialog("教程", PAGES, COUNT_OF(PAGES), NULL, 0);
    return 1;
}

static int tut_battle(Pet *pet)
{
    static const char *const PAGES[] = {
        "$C$E（ $NA5$n 不知从哪冒了出来）$e $P"
        "$C对战就是让伴兽比试。$L↑/↓ 选行动  Z 确认$P"
    };

    (void)pet;
    g_api->dialog("教程", PAGES, COUNT_OF(PAGES), NULL, 0);
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
    static const ModInfo_t info = { "Story", "2.2.0", MOD_API_VERSION, MOD_REQUIRES(REQUIRES) };
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

    /* 文本函数：$f / $F 用，id 从 0 开始 */
    if (api->text_funcs(TEXT_FUNCS, COUNT_OF(TEXT_FUNCS)) == UINT64_MAX)
        return fail("文本函数注册失败");

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
