# 写一个 EPets 模组

模组就是一个 DLL，导出三个入口（建议再加一个可选的 `mod_error`），通过加载器给的函数表注册内容。
内容有十一种：特质、物品、宠物类型、成就、剧情、随机事件、小游戏、NPC、术语、**菜单、配置**。
下面从最小可用例子讲到十一种内容、依赖声明和错误诊断。

接口版本：`LOADER_API_VERSION = 11`。用旧版 `Loader.h` 编译的 DLL 会在启动时被拒绝，并打印版本不符。

---

## 1. 六十秒上手

新建 `mods/src/ToyPack.c`：

```c
#define _CRT_SECURE_NO_WARNINGS 1
#include "Defineds.h"
#include "Loader.h"

#include <stdarg.h>
#include <stdio.h>

static const LoaderApi *g_api;
static char g_err[192];

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof(g_err), fmt, ap);
    va_end(ap);
    return 0;
}

/* 物品效果：用一次心情 +15 */
static int bite_used(Pet *pet)
{
    int v = (int)pet->happy + 15;
    pet->happy = (uint8_t)(v > MAX_STAT ? MAX_STAT : v);
    return 1;
}

static const Item MY_ITEMS[] = {
    {bite_used, I_TOY, 120, "咬咬胶"},
};

/* 依赖：成就奖励/宠物类型都按名字查 Core，所以先声明依赖 */
static const char *const REQUIRES[] = { "Core" };

MOD_EXPORT const ModInfo_t *mod_info(void)
{
    static const ModInfo_t info = { "ToyPack", "1.0.0", MOD_API_VERSION, MOD_REQUIRES(REQUIRES) };
    return &info;
}

MOD_EXPORT int mod_init(const LoaderApi *api)
{
    if (!api) return fail("mod_init 收到空接口表");
    if (api->version != LOADER_API_VERSION)
        return fail("接口版本不符：模组按 v%u 编译，加载器是 v%u",
                    (unsigned)MOD_API_VERSION, (unsigned)api->version);
    if (api->size < (uint32_t)sizeof(LoaderApi))
        return fail("LoaderApi 大小不足：Defineds.h/Loader.h 与加载器不一致");

    g_api = api;
    if (MOD_ADD(api, KIND_ITEM, MY_ITEMS) == ID_NONE)
        return fail("KIND_ITEM 注册失败：内容表已满，或 Item 尺寸与加载器不一致");

    if (g_api->find(KIND_ITEM, "咬咬胶") == ID_NONE)
        return fail("自检失败：注册后按名字查不到「咬咬胶」");
    return 1;
}

/* 可选：mod_init 返回 0 时加载器会调用它，把原因放进启动报错里 */
MOD_EXPORT const char *mod_error(void)
{
    return g_err[0] ? g_err : "mod_init 失败（未提供原因）";
}

MOD_EXPORT void mod_exit(void)
{
    g_api = 0;
}
```

编译（在项目根目录）：

```bat
cl /nologo /utf-8 /W4 /LD /Imods\incl mods\src\ToyPack.c /Fe:mods\ToyPack.dll
```

或者把 `ToyPack` 加进 `build.bat` 里那行：

```bat
for %%M in (Core Ach Story Minigame Events Activity Care World Debug ToyPack) do (
```

然后 `build.bat`、`epet.exe`，进游戏 → 出门 → 商场，就能买到「咬咬胶」。

---

## 2. 模组是什么

一个 DLL，必须导出：

| 符号 | 作用 |
|---|---|
| `const ModInfo_t *mod_info(void)` | 名字、版本、接口版本、依赖列表 |
| `int mod_init(const LoaderApi *api)` | 注册内容；返回 1 成功，0 = 放弃加载 |
| `void mod_exit(void)` | 卸载，一般只把 `g_api` 置空 |

可选导出：

| 符号 | 作用 |
|---|---|
| `const char *mod_error(void)` | `mod_init` 返回 0 后的详细原因（推荐实现） |

```c
typedef struct {
    const char *name;            /* 模组名：依赖检查用的键 */
    const char *version;         /* 版本串 */
    uint32_t api_version;        /* 要求的接口版本 */
    const char *const *requires; /* 依赖的模组名数组，可 NULL */
    uint64_t require_count;
} ModInfo_t;

#define MOD_REQUIRES(list) (list), (uint64_t)(sizeof(list) / sizeof((list)[0]))
```

加载器在 `modloader_init()` 里扫描 `mods\*.dll`：

1. **阶段一**：`LoadLibrary` + 读 `mod_info`，校验三个导出、接口版本、依赖表、重名；
2. **阶段二**：按 `requires` 做多轮扫描，依赖就绪才调 `mod_init`
   （候选先按 DLL 文件名排序，所以顺序稳定；Id 顺序只保证依赖在前）；
3. 失败原因写进错误表，启动时打印摘要。

一个模组**可以只注册任意几种内容**：只加 3 个成就也行，只加几个菜单也行。

---

## 3. 十一种内容类型

所有内容都是**只读描述符数组**，字符串是字面量（进 DLL 只读段），不拷贝、不释放。
注册用 `MOD_ADD(api, kind, 数组)`，一次注册一整张表，返回第一个 Id。

| kind | 结构体 | 一句话 |
|---|---|---|
| `KIND_TRAIT` | `Trait_t` | 特质：一个回调 + 正负 + 名字 + 互斥项 |
| `KIND_ITEM` | `Item` | 物品：一个回调 + 分类 + 价格 + 名字 |
| `KIND_PET_TYPE` | `PetType` | 宠物种类：内嵌被动 + 名字 + 简介 |
| `KIND_ACHIEVEMENT` | `Achievement_t` | 成就：达成条件回调 + 纯数据奖励 |
| `KIND_STORY` | `Story_t` | 剧情：场景 + 对话页 + 可选选项 + 播完的效果 |
| `KIND_EVENT` | `Event_t` | 随机事件：权重 + 旁白 + 效果回调 |
| `KIND_MINIGAME` | `Minigame_t` | 小游戏：绑定宠物类型 + 完整流程回调 |
| `KIND_NPC` | `Npc_t` | NPC：简介 + 招呼/介绍/道别 |
| `KIND_TERM` | `TermDef_t` | 术语：名字 + 解释（对话里按 C 查） |
| `KIND_MENU` | `Menu_t` | 菜单：父菜单 + 排序 + 回调（界面也是内容） |
| `KIND_CONFIG` | `Config_t` | 配置：持久化键 + 类型 + 默认值/范围（配置页可改） |

### 3.1 特质 `Trait_t`

```c
typedef struct {
    OnTraitUsed on_used;   /* int (*)(Pet*)，1=已生效 */
    TraitType_t type;      /* T_POS / T_NEG / T_MID */
    const char *name;      /* 持久化键 */
    const char *conflict;  /* 与之互斥的特质名，NULL=无 */
} Trait_t;
```

```c
static int trait_lively_used(Pet *pet)
{
    int v = (int)pet->happy + 10;
    pet->happy = (uint8_t)(v > MAX_STAT ? MAX_STAT : v);
    return 1;
}

static const Trait_t MY_TRAITS[] = {
    {trait_lively_used, T_POS, "活泼", "懒惰"},
};
```

- 引擎在**每日结算**时对每只宠物的特质调用 `on_used`；
- 互斥只要写一边（加载器的 `trait_conflict(a,b)` 会双向比对）；
- 摇特质时引擎保证 1 正 1 负且不互斥（`gen_traits`）；用 `trait_shop` 追加额外特质时
  引擎同样会排除“已拥有”和“与已有互斥”的候选，特质数量不设上限。

### 3.2 物品 `Item`

```c
typedef struct {
    OnItemUsed on_used;  /* int (*)(Pet*)，1=使用成功 */
    ItemType_t type;     /* I_FOOD / I_TOY / I_DECORATION / I_MEDICINE / I_CLEANING / I_TRAVEL */
    uint32_t price;      /* 0 = 非卖品（纪念品） */
    const char *name;
} Item;
```

```c
static int item_bento_used(Pet *pet)
{
    pet->hunger = (uint8_t)(pet->hunger + 10 > MAX_STAT ? MAX_STAT : pet->hunger + 10);
    return 1;
}

static const Item MY_ITEMS[] = {
    {item_bento_used, I_TRAVEL, 70, "干粮"},
};
```

- 效果**只写在回调里**，不要再想着 `effect[4]` 那种数据表；
- 是否消耗由宿主/菜单决定（喂食/清洁会 `inv_rm`），回调只管改属性；
- 价格 0 的物品不会出现在商店里。

### 3.3 宠物类型 `PetType`（+ 内嵌被动）

```c
typedef struct { OnPassiveTrigger on_trigger; uint8_t chance; const char *name; } Passive_t;
typedef struct { Passive_t passive; const char *name; const char *describtion; } PetType;
```

```c
static int passive_ice(Pet *pet)
{
    pet->health = (uint8_t)(pet->health + 10 > MAX_STAT ? MAX_STAT : pet->health + 10);
    return PASSIVE_OK;
}

static const PetType MY_PETS[] = {
    {{passive_ice, 25, "寒冰护体"}, "冰霜兔", "浑身覆盖冰晶的小生灵"},
};
```

被动返回值有语义：

| 值 | 含义 |
|---|---|
| `PASSIVE_FAIL` (0) | 未生效 |
| `PASSIVE_OK` (1) | 已生效（纯增益） |
| `PASSIVE_GUARD` (2) | 抵消负面效果（闪避/减伤），`damage()` 据此免伤 |

概率 `chance`（%）由引擎掷骰，不满足就不调回调；没有被动就填 `on_trigger = 0`。

### 3.4 成就 `Achievement_t`

```c
typedef struct { const char *item; uint16_t count; } AchReward_t;

typedef struct {
    const char *name;
    const char *desc;
    uint8_t hidden;                    /* 1 = 达成前显示 ??? */
    int (*on_check)(const Pet *pet);   /* 1 = 达成；NULL = 只能由引擎强制解锁 */
    const AchReward_t *rewards;        /* 按物品名引用，解锁时自动发 */
    uint32_t reward_count;
} Achievement_t;
```

```c
static int ach_rich(const Pet *pet) { return pet->coins >= 5000; }

static const AchReward_t R_RICH[] = { {"冠羽", 1} };

static const Achievement_t MY_ACH[] = {
    {"小富翁", "持有鲁尼 >= 5000", 0, ach_rich, R_RICH, 1},
    {"铁石心肠", "拒绝了A5的领养邀请", 1, 0, 0, 0},   /* 剧情性成就 */
};
```

- 引擎每次轮询都会对未解锁的成就调 `on_check`，命中就置位 + 发奖；
- 奖励物品按**名字**解析，物品模组不在也无所谓（找不到就不发）；
- 领取状态（`ach_claimed`）由记事册菜单控制，奖励在解锁时已经入包。

### 3.5 剧情 `Story_t`

```c
typedef struct { const char *text; int value; } StoryOpt_t;

typedef struct {
    const char *npc;              /* NULL=通用；否则只在该 NPC 当值时播 */
    const char *speaker;
    const char *const *pages;     /* 顺序播放；支持 $ 格式符和 %NAME%/%TYPE% */
    uint32_t page_count;
} Dialog_t;

typedef struct {
    const char *name;                 /* 持久化键 + 标志位 */
    const char *scene;                /* 触发场景，NULL=任意 */
    int (*on_check)(const Pet *pet);  /* 1=该播了；NULL=无条件 */
    const Dialog_t *dialogs;          /* 按顺序播，npc 不匹配的跳过 */
    uint32_t dialog_count;
    const StoryOpt_t *opts;           /* 可空：播完弹选项 */
    uint32_t opt_count;
    int (*on_play)(Pet *pet, int choice); /* 播完的效果；choice=0 表示无选项 */
} Story_t;
```

```c
static const char *const P_MALL[] = {
    "  (走到一家商场门口,\n   A5正好在旁边)",
    "  A5: 这商场东西挺全。",
};

static const Dialog_t D_MALL[] = { {0, "A5", P_MALL, 2} };

static const Story_t MY_STORY[] = {
    {"首次逛商场", "mall", 0, D_MALL, 1, 0, 0, 0},
};
```

场景标签（宿主/菜单会在对应时机调用 `story_play`）：

`adopt` `work` `work_apply` `travel` `mall` `hospital` `amuse` `park` `spa` `battle` `center` `idle` `death`

带 NPC 分支的写法（同一段剧情，当值员工不同）：

```c
static const Dialog_t D_SPA[] = {
    {"小春", "小春", P_SPA_1, 1},
    {"老陈", "老陈", P_SPA_2, 1},
    {"阿夜", "阿夜", P_SPA_3, 1},
};
```

播放规则：同场景里**第一段“未播过且条件满足”**的剧情会被播，播完置位标志位；
所有对白都不匹配当前 NPC 时不算播过。

需要流程控制（循环问答、多步菜单）时，把 `dialogs` 留空，在 `on_play` 里用
`g_api->dialog(speaker, pages, n, opts, n_opts)` 自己跑完整流程。

### 3.6 随机事件 `Event_t`

```c
typedef struct {
    const char *name;
    const char *text;          /* 旁白，可用 %NAME% 和 $ 格式符 */
    uint16_t weight;           /* 相对权重，0=停用 */
    int (*on_fire)(Pet *pet);  /* 效果 */
} Event_t;
```

```c
static int ev_bug(Pet *pet) { pet->happy = (uint8_t)(pet->happy + 15 > MAX_STAT ? MAX_STAT : pet->happy + 15); return 1; }

static const Event_t MY_EVENTS[] = {
    {"找到小虫", "%NAME% 找到了一只小虫子！", 1, ev_bug},
};
```

### 3.7 小游戏 `Minigame_t`

```c
typedef struct {
    const char *name;
    const char *desc;
    const char *pet_type;   /* 绑定宠物类型名；NULL=通用 */
    int (*run)(Pet *pet);   /* 完整流程，1=成功 */
} Minigame_t;
```

```c
static int game_guess(Pet *pet)
{
    int answer = (int)g_api->rnd(1, 4);
    g_api->ui_title("猜方向");
    g_api->ui_print("\n  它会从哪个方向出来？\n  ↑左 ↓右 Z上 X下\n");
    if (g_api->key() == KEY_OK)   /* Z 键 */
    {
        pet->happy = (uint8_t)(pet->happy + 20 > MAX_STAT ? MAX_STAT : pet->happy + 20);
        g_api->ui_ok("  猜中了！心情+20");
        return 1;
    }
    g_api->ui_warn("  猜错了...");
    (void)answer;
    return 0;
}

static const Minigame_t MY_GAMES[] = {
    {"猜方向", "猜它从哪边出来", "暗影猫", game_guess},  /* 绑定种类 */
    {"丢沙包", "简单的接投游戏", 0, game_guess},          /* 通用 */
};
```

小游戏里可以自由用 `ui_title/ui_print/ui_ok/ui_err/ui_warn/key/delay/rnd` 编排流程，
直接改 `pet` 字段作为奖励。

**实时玩法**用 `key_poll()`（非阻塞）+ `now_ms()`（毫秒）：

```c
/* 5 秒内连按 Z；轮询间隔 100ms，和原版节奏一致 */
static int mash_5s(void)
{
    uint32_t start = g_api->now_ms();
    int count = 0;

    while (g_api->now_ms() - start < 5000)
    {
        int k = g_api->key_poll();
        if (k == KEY_OK) count++;
        else if (k == KEY_CANCEL) break;
        g_api->delay(100);
    }
    return count;
}
```

- 无头模式（`--demo`/`--auto`）下 `key_poll()` 恒返回 `KEY_OK`、`delay()` 不等待、
  `now_ms()` 是**虚拟时间**（每查一次 +16ms），所以限时循环会瞬间跑完，不会拖慢自检；
- 写“必须等到某个键”的循环时，用 `g_api->is_headless()` 加个退出条件，别把自检挂死。

### 3.8 NPC `Npc_t`

```c
typedef struct {
    const char *name;     /* 剧情分支引用的键 */
    const char *desc;     /* 社群菜单里的简介 */
    const char *greet;    /* 打招呼，可 NULL */
    const char *intro;    /* 介绍宠物，可 NULL */
    const char *farewell; /* 道别，可 NULL */
} Npc_t;
```

### 3.9 术语 `TermDef_t` 与文本格式符

```c
typedef struct { const char *name; const char *desc; } TermDef_t;
```

剧情里用 `$K桥屋$k` 把**关键词**包起来：对话页里显示成绿色，并按 **C** 键从术语表查解释。
术语表里没有的词也照样高亮，只是查解释时提示“模组没有提供解释”。

#### 文本按“块”处理

引擎先把文本切成块，块是计数单位（`$A` / `$W` / `$F` 靠它算“项目数”）：

| 写法 | 块数 | 说明 |
|---|---|---|
| `AAA` | 3 | 每个字符一块 |
| `$Xas` | 3 | `$X` 命令一块、`a`、`s` |
| `$[aaa$]` | 1 | 一个内容块（里面还有 3 块） |
| `${AAAA$}` | 1 | 一个逻辑块（里面 4 个内容块） |

`$[ … $]`（内容块）和 `${ … $}`（逻辑块）可以互相嵌套；没闭合的块在文本结尾自动补全。
`$L` 也是一条命令，所以它同样算一块；文本里的 `\n` 不是命令，只是同一条逻辑行里的普通换行。

#### 格式符

| 格式符 | 作用 |
|---|---|
| `$X` / `$x` | 流式输出时 **X 键跳过** 开（默认）/ 关 |
| `$S` / `$s` | 流式输出 开（对话默认开）/ 关 |
| `$K` / `$k` | 关键词效果（绿，C 键查术语）开 / 关（默认） |
| `$N` / `$n` | 名字效果（黄）开 / 关（默认） |
| `$E` / `$e` | 吐槽 / 评论效果（灰色斜体）开 / 关（默认） |
| `$L` | 逻辑换行：另起一条逻辑行（`$c` 的清行边界） |
| `$C` / `$c` | 清屏 / 清掉当前整条逻辑行（含这行里的 `\n`） |
| `$P` / `$p!T` | 等 Z 键 / 等 T 毫秒（T 是内容块里的数字） |
| `$A!L!L` | 分支块：L1 是选项，选中第 i 项就播放 L2 的第 i 项（两块项目数应相等） |
| `$a!T!L!L` | 分支块（带问题）：先渲染 `!T`，再同 `$A` |
| `$W!L!L` | 循环块：L1 是选项（比 L2 多一项，最后一项=结束），选第 i 项播放 L2 第 i 项，然后重新出选项 |
| `$w!T!L!L` | 循环块（带问题）：先渲染 `!T`，再同 `$W` |
| `$F!T!L` | 函数调用：T=函数 id，返回值当 L 的项目下标（超出项目数就取模循环） |
| `$f!T` | 函数调用：T=函数 id，返回值当文本插回 |
| `$$` | 字面 `$` |
| `%NAME%` / `%TYPE%` | 宠物名 / 宠物种类 |

`!T` / `!L` 只是“这里要跟一个内容块 / 逻辑块”的书写说明，不写进文本。
命令**后面直接跟参数块**：

```text
$w$[有什么想问的吗？$]${$[伴兽是什么？$]$[能赚钱吗？$]$[没什么想问了$]}
  ${$[就是你们说的宠物。$P$]$[它接差事赚的比你多。$P$]}
```

- `$W` / `$w` 要求 **L1 项目数 = L2 项目数 + 1**，多出来那项当“结束循环”；
- `$A` / `$a` 要求两块项目数相等，不等时按短的那个走；
- 选项标签取内容块的可见文字（格式符会被去掉）；
- 无头自检（`--demo` / `--auto`）里选项自动选第 1 项；`$W` 会把每个选项各问一遍再结束，不会死循环。

#### 节奏：没有翻页

对话**不再有 ←/→ 翻页**：`pages` 按顺序播放，节奏用 `$C` / `$P` / `$p` 自己排。
一页播完如果文本自己没等过键（`$P` / `$p` / 选项菜单），引擎会在页尾补一次 Z 等待，
所以只写文本、不写 `$P` 也会停下来等玩家。

```c
static const char *const P_MALL[] = {
    "  $C（走到一家商场门口，$L   $NA5$n 正好在旁边）$P",
    "  $C$NA5$n ：这商场东西挺全，$L  就是价格嘛……死贵。$P",
};
```

`$C` 清屏后会把当前说话人标题补回来。流式输出每字多少毫秒由配置项 `text.stream_ms`
决定（配置菜单可改，默认 30，0=不流式）；流式过程中按 **X** 立刻跳过这一屏剩下的逐字动画。

换行有两种：`$L` 是**逻辑换行**（另起一条逻辑行），`\n` 是同一条逻辑行里的普通换行。
`$c` 清的是“当前逻辑行”，所以 `$L` 之前的内容不会被清掉，而同一逻辑行里用 `\n` 折出来的
几行会被一起清掉、光标回到这条逻辑行的开头，接着写就是覆盖刷新：

```text
$C加载中……$c完成            ← $c 把「加载中……」清掉，从行首写「完成」
$C第一行\n第二行$c替换      ← 两行属于同一条逻辑行，$c 两行一起清
$C第一行$L第二行$c替换      ← $L 之后是新逻辑行，$c 只清「第二行」
```

没有 VT 的控制台退不上去，`$c` 只能清光标所在的那一行。

#### 文本函数 `$f` / `$F`

模组用 `api->text_funcs(list, count)` 注册一组函数，id 从 0 开始按注册顺序编号：

```c
/* 返回 0   → 结果数字写在 *(uint64_t *)ret；
 * 返回 >0  → 结果是长度为该返回值的 UTF-8 串，指针写在 *(const char **)ret。 */
static uint64_t fn_level(Pet *pet, void *ret)
{
    *(uint64_t *)ret = pet->level;
    return 0;
}

static uint64_t fn_name(Pet *pet, void *ret)
{
    const char *n = pet->name ? pet->name : "它";
    *(const char **)ret = n;
    return (uint64_t)strlen(n);
}

/* id 0 = 等级，id 1 = 名字 */
static const TextFunc_t FUNCS[] = { fn_level, fn_name };
```

- `$f$[0$]` 插入等级，`$f$[1$]` 插入名字；
- `$F$[0$]${$[零$]$[一$]$[二$]}` 用返回值取第 `返回值 % 3` 项。

### 3.10 菜单 `Menu_t`

```c
typedef int (*OnMenuRun)(Pet *pet);

typedef struct {
    const char *name;   /* 菜单唯一名，也是子菜单引用的键 */
    const char *label;  /* 菜单里显示的文字，NULL=用 name */
    const char *parent; /* 父菜单名，NULL=主菜单 */
    uint32_t order;     /* 排序权重，小的在前 */
    uint32_t flags;     /* MENU_FLAG_EXIT 等 */
    OnMenuRun run;      /* 执行回调；NULL=纯容器 */
} Menu_t;

#define MENU_FLAG_EXIT 1u  /* 选中后结束游戏（“退出”） */
#define MENU_FLAG_STAY 2u  /* 页面型：回调返回 1 留在本页重画，返回 0 才回上一级 */
```

回调返回值：

| 返回值 | 含义 |
|---|---|
| `1` | 已处理，继续（父菜单重新渲染） |
| `0` | 返回上一级 |
| `-1` | 请求退出游戏（会一路传回宿主主循环） |

页面型菜单（`flags` 含 `MENU_FLAG_STAY`）的返回值含义稍有不同：`1` = 留在本页（加载器会再调一次回调重画），
`0` = 离开本页回到上一级（父菜单重画），`-1` 仍然是退出游戏。商场 / 医院就是这样实现的：买完 / 治完还留在
购买页，按 X 或选「返回」才回「出门」；无头自检只执行一次，不循环。

父菜单用名字表达。`run = NULL` 表示容器：选中后加载器会进入它的子菜单，
并反复渲染，直到玩家选「返回」（加载器会自动在子菜单末尾加「返回」）。

```c
/* 容器本身可以不写回调，子菜单用 parent 挂到它下面 */
static int menu_bite(Pet *pet)
{
    g_api->ui_title("玩具箱");
    g_api->ui_print("\n  咬咬胶被%s咬得咯吱响~\n", pet->name ? pet->name : "它");
    pet->happy = (uint8_t)(pet->happy + 10 > MAX_STAT ? MAX_STAT : pet->happy + 10);
    g_api->ui_print("\nZ:继续\n");
    g_api->key();
    return 1;
}

static const Menu_t MY_MENUS[] = {
    {"玩具箱", "玩具箱", 0,       200, MENU_FLAG_NONE, 0},
    {"咬咬胶", "咬咬胶", "玩具箱", 10,  MENU_FLAG_NONE, menu_bite},
};
```

注册：`MOD_ADD(api, KIND_MENU, MY_MENUS);`

规则与坑：

- 菜单 `name` 在**全局唯一**（同名会进“内容重名”警告，`find` 只返回先注册的）；
- 子菜单的 `parent` 必须和父菜单的 `name` 完全一致；
- 同一个父菜单下 `order` 小的排前面；`order` 相同按注册顺序；
- 子菜单数量动态（uint64_t 计数，受内存限制）；**每页几项由滑动条配置 `ui.page_size` 决定**（默认 4，可调 1~16）；
- 选中项不用箭头，改为背景色 + 反色高亮；自定义列表用 `g_api->ui_option(text, selected)` 保持同样风格；
- 多页菜单用 **←/→ 翻页**，↑/↓ 只负责选择，Z 确认、X 返回（←/→ 不再等同于 X/Z）；
- 每层菜单会**按步骤记录**光标位置（键是父菜单名，主菜单用 `@root`）：选完选项自动回到父菜单、
  或按 X 一级级返回时，每一层都停在原来那一项，嵌套层数不限；`ui_menu` 的自定义列表按标题记，
  动态列表变短时夹到最后一项。记录只在内存里，重启进程后从第一项开始；
- **购买页**用 `MENU_FLAG_STAY`：回调返回 1 表示“还在这一页”，加载器会重新调用回调重画；
  返回 0 才回上一级。`World.c` 的商场 / 医院就是例子（买完留在列表里，按 X 或选「返回」才回出门）；
- 选项文本用 `g_api->opt_text(&opts[i], ...)` 写入（短串 15 字节/长串动态），`ui_menu` 消费后自动释放；
- 菜单回调里做完整流程（列表、对话、小游戏都行），宿主不管内容；
- 动作跑完建议调用 `g_api->save(pet)`（如果需要立刻落盘），并用 `ui_print("\nZ:继续\n") + key()`
  暂停一下；
- 主菜单里的「存档 / 读档 / 配置 / 退出」是宿主自带的 4 个系统菜单，不用你注册。

现成例子：

- `Care.c`：状态/背包/喂食/清洁/每日任务/睡觉，用 `inv_pick` 选物品；
- `World.c`：`出门` 容器 + 5 个子菜单 + `游乐园` 容器 + 5 个项目（3 个可玩 + 2 个未开）；
- `Activity.c`：`桥屋` 容器 + 训练/美容/寄养/附近对战 + `祭坛` 容器（改名 / 特质定制，用 `name_pick`、`trait_shop`）；
- `Story.c`：`社交`（动态 NPC 列表）和 `教程`（静态子菜单）；
- `Ach.c` / `Minigame.c`：单个动作型菜单；`Debug.c`：命令式控制台（`_getch` 行输入 + 命令解析）。

### 3.11 配置 `Config_t`

```c
typedef enum { CFG_BOOL = 0, CFG_INT = 1, CFG_SLIDER = 2 } ConfigType_t;

typedef struct {
    const char *name;  /* 持久化键，建议 "模块.项" */
    const char *label; /* 配置页显示名 */
    const char *desc;  /* 说明，可空 */
    ConfigType_t type; /* CFG_BOOL / CFG_INT / CFG_SLIDER */
    int32_t def;       /* 默认值 */
    int32_t min;       /* 最小值 */
    int32_t max;       /* 最大值 */
} Config_t;
```

```c
static const Config_t MY_CONFIGS[] = {
    {"toypack.bite_bonus", "咬咬胶加成", "开启后咬咬胶多 +5 心情", CFG_BOOL, 1, 0, 1},
    {"toypack.cooldown",   "冷却秒数",   "两次咬咬胶之间的等待",   CFG_INT,  3, 1, 30},
};

MOD_ADD(api, KIND_CONFIG, MY_CONFIGS);
```

- 配置页是宿主自带的系统菜单「配置」，会自动列出**所有模组**注册的配置项；
- `CFG_BOOL` 选中直接切换；`CFG_INT` 弹出值列表；`CFG_SLIDER` 显示 `< value >`，←/→ 调整、Z 确认；
- 改动立刻写入 `config.dat`；
- 读取：`int32_t v = g_api->config_get("toypack.cooldown");`
- 写入：`g_api->config_set("toypack.cooldown", 5);`（会按描述符夹到 min~max 并落盘）
- `name` 是持久化键，改了名字等于丢配置；建议 `模块.项`；
- 同名配置先注册的生效，后来的只记“内容重名”警告；
- 宿主自带 `ui.page_size`（滑动条，菜单每页项数 1~16，默认 4）和 `text.stream_ms`（滑动条，流式输出每字毫秒 0~120，默认 30），Care 自带
  `care.feed_trait_special`（喂食贪吃/挑食特判，默认开）。

---

## 4. 加载器服务（55 项）

### 注册与查表

```c
Id          add(ContentKind_t kind, const void *list, uint64_t count, uint32_t elem_size);
const void *get(ContentKind_t kind, Id id);          /* 无效 Id 返回 NULL */
Id          find(ContentKind_t kind, const char *name);
uint64_t    count(ContentKind_t kind);
```

> `MOD_ADD(api, kind, 数组)` 会自动算 `count` 和 `elem_size`，优先用它。

### 名字库

```c
uint64_t    add_names(Gender_t gender, const char *const *list, uint64_t count);
const char *name_get(Gender_t gender, uint64_t index);
uint64_t    name_count(Gender_t gender);
```

### 数值 / 背包

```c
uint32_t    rnd(uint32_t lo, uint32_t hi);           /* 闭区间（32 位） */
uint64_t    rnd64(uint64_t lo, uint64_t hi);         /* 闭区间（64 位，动态表下标用） */
const char *rand_name(Gender_t gender, const char *avoid);
int         gen_traits(Pet *pet);                    /* 1 正 1 负，返回数量 */
int         inv_add(Pet *pet, Id item, uint64_t count);
uint64_t    inv_count(const Pet *pet, Id item);
int         inv_rm(Pet *pet, Id item, uint64_t count);
int         inv_set(Pet *pet, Id item, uint64_t count);
int         passive_try(Pet *pet);                   /* 掷概率 + 跑被动，返回 PASSIVE_* */
int         damage(Pet *pet, uint32_t amount);       /* 返回 1=造成伤害, 0=被抵消 */
Id          inv_pick(Pet *pet, ItemType_t type, const char *title); /* 让玩家挑背包物品 */
```

### 界面 / 输入

```c
void ui_title(const char *title);
void ui_print(const char *fmt, ...);   /* 也走文字引擎：支持 $ 格式符（%NAME% 要写 %%NAME%%） */
void ui_ok(const char *fmt, ...);
void ui_err(const char *fmt, ...);
void ui_warn(const char *fmt, ...);
int  ui_menu(const char *header, SelectOpt_t *opts, uint64_t count); /* 返回 value，X=返回 -1；按 header 记住上次停留的项 */
void opt_text(SelectOpt_t *opt, const char *fmt, ...);  /* 写选项文本；ui_menu 消费后释放 */
int  key(void);       /* 阻塞读键，返回 KEY_*：↑=1 ↓=2 Z=3 X=4 C=5 ←=6 →=7 */
void delay(uint32_t ms);
int  dialog(const char *speaker, const char *const *pages, uint32_t n_pages,
            const StoryOpt_t *opts, uint32_t n_opts);  /* pages 顺序播放，无翻页 */
void ui_option(const char *text, int selected);  /* 自定义列表的一行（同款背景+反色高亮） */
uint64_t text_funcs(const TextFunc_t *list, uint64_t count); /* 注册文本函数（$f / $F 用，id 从 0 开始） */
```

### 宠物定制

```c
const char *name_pick(Gender_t gender, const char *title); /* 名字库手选，取消返回 NULL */
int trait_shop(Pet *pet, const char *title, int allow_delete,
               uint32_t cost_random, uint32_t cost_pick, uint32_t cost_delete);
```

`trait_shop` 跑完整的一套“特质商店”菜单：

- **随机加**：从“没拥有、不与已有互斥”的候选里随机挑一个，追加到 `Pet.traits`；
- **手选加**：把同一批候选列出来让玩家挑（带 正面 / 负面 标注）；
- **删除**（`allow_delete != 0` 才有）：列出当前拥有的特质，删掉一个。

代价由调用方按内容定价传入，加载器只负责从 `pet->coins` 扣钱、维护动态数组、
查重和查互斥；钱不够 / 没得加只提示、不扣钱。重生向导和桥屋的祭坛共用这一套，
所以两边规则不会漂移。无头自检模式直接返回，不消费。

### 成长 / 时间 / 规则

```c
int      ach_unlock(Pet *pet, Id ach);          /* 解锁 + 发奖，1=新解锁 */
int      ach_unlocked(const Pet *pet, Id ach);
int      ach_claimed(const Pet *pet, Id ach);
int      ach_claim(Pet *pet, Id ach);           /* 标记已领 */
void     ach_merge_global(Pet *pet);            /* 并入跨存档成就 */
int      story_seen(const Pet *pet, Id story);
int      story_mark(Pet *pet, Id story);
void     exp_add(Pet *pet, int amount);         /* 加经验并处理升级 */
uint32_t rtc_day(void);                         /* 10 秒一天 */
int      rtc_night(void);                       /* 20:00~06:00 或 tick 为奇数 */
int      trait_conflict(Id a, Id b);
```

### 菜单调度

```c
int menu_run(Pet *pet, const char *parent);  /* parent=NULL 为主菜单 */
```

返回：`1`=已处理（继续），`0`=返回上一级，`-1`=请求退出。
容器菜单（`run == NULL`）由加载器自动进入子菜单并循环，直到返回；标记 `MENU_FLAG_STAY`
的页面型菜单由加载器反复调用回调，直到回调返回 0（见 3.10）。
每页项数读滑动条配置 `ui.page_size`（默认 4，范围 1~16）；选中项由加载器画成背景色 + 反色高亮。
`menu_run` / `ui_menu` 都会按步骤记录光标：再次进入同一菜单时恢复上次停留的项
（`menu_run` 用父菜单名当键、主菜单用 `@root`；`ui_menu` 用标题当键），
所以从子菜单返回后父菜单仍停在原处，嵌套层数不限。

### 配置

```c
int32_t config_get(const char *name);            /* 未注册/未存过返回 0 */
int config_set(const char *name, int32_t value); /* 按描述符夹范围并写 config.dat */
```

### 内容驱动

```c
int      story_play(Pet *pet, const char *scene, const char *npc); /* 播匹配场景的下一段剧情 */
int      ach_poll(Pet *pet);                                      /* 轮询成就，返回新解锁数 */
uint64_t minigame_list(Id pet_type, Id *out, uint64_t max);       /* 列出可玩游戏 */
int      minigame_run(Pet *pet, Id game);                         /* 跑一个小游戏 */
```

### 存档

```c
int save(Pet *pet);   /* 1=成功 */
int load(Pet *pet);   /* 1=成功（失败不动 pet） */
```

### 实时输入 / 模式

```c
int      key_poll(void);   /* 非阻塞读键：有键 KEY_*，无键 0 */
uint32_t now_ms(void);     /* 单调毫秒时钟（第一次调用为起点） */
int      is_headless(void);/* 1=无头自检模式 */
```

---

## 5. 依赖与诊断

### 5.1 声明依赖

```c
static const char *const REQUIRES[] = {
    "Core:物品/粗粮",              /* Core 必须注册物品「粗粮」 */
    "Story",                       /* 只要求 Story 能加载 */
    "Achievements:成就/铁石心肠",  /* 成就必须由 Achievements 本人注册 */
};

MOD_EXPORT const ModInfo_t *mod_info(void)
{
    static const ModInfo_t info = { "MyWorld", "1.1.0", MOD_API_VERSION, MOD_REQUIRES(REQUIRES) };
    return &info;
}
```

语法只有两种：

| 写法 | 含义 |
|---|---|
| `模组名` | 只要求该模组加载成功 |
| `模组名:种类/名字` | 还要求该模组**本人**注册了指定内容 |

`种类` 可用中文或英文：特质/物品/宠物类型/成就/剧情/事件/小游戏/术语/NPC/菜单/配置
（对应 trait/item/pet/achievement/story/event/minigame/term/npc/menu/config）。

- 模组名匹配不区分大小写；**内容名区分大小写**，必须是注册时的原字符串；
- 内容检查发生在依赖模组 `mod_init` 完成之后，所以它注册的内容一定已经就位；
- 依赖内容缺失、或同名内容其实是别的模组注册的，本模组不会加载，并给出具体是哪个模组/内容；
- 只把**硬依赖**写进 `requires`：对方不在就活不下去的模块。只是“可选增强”的引用
  （按名字查不到就跳过）不要写，否则会把可选模组变成必装；
- 不写版本约束：`requires` 只有“模组 + 内容”。接口版本由 `ModInfo_t.api_version` 统一校验。

### 5.2 详细报错

`mod_init` 失败时用 `fail()` 留下原因，并导出 `mod_error()`：

```c
if (MOD_ADD(api, KIND_ITEM, MY_ITEMS) == ID_NONE)
    return fail("KIND_ITEM 注册失败：内容表已满，或 Item 尺寸与加载器不一致");
```

启动时会看到：

```
[模组] 成功 9 个，发现 2 条问题：
  [错误] Activities (Activity.dll)：缺少依赖模组 Story
        没有找到名为 "Story" 的模组；检查 mods\ 目录、DLL 文件名，或对方的 mod_info.name
  [错误] BadPack (BadPack.dll)：mod_init 返回 0
        故意测试失败：注册了 1 个菜单后返回 0
```

加载器会检查的失败原因：

| 原因 | 说明 |
|---|---|
| 无法加载 DLL | 文件损坏/位数不对/被占用，含 Windows 错误文本 |
| 缺少导出函数 | `mod_info` / `mod_init` / `mod_exit` 少一个 |
| mod_info 返回空指针 / 没有名字 | `ModInfo_t` 必须是静态结构，`name` 不能为空 |
| 接口版本不符 | DLL 用旧 `Loader.h` 编译，重新编译 |
| 依赖表损坏 / 过长 | `requires` 为 NULL 但 `require_count > 0`，或超过 32 个 |
| 模组重名 | 两个 DLL 的 `mod_info.name` 相同，先加载的生效 |
| 缺少依赖模组 X | 目录里没有 X，或 X 的名字拼错 |
| 依赖的模组 X 加载失败 | X 自己失败了，顺着 X 的错误条目查 |
| 依赖循环 | `requires` 互相等待，相关模组全部拒绝加载 |
| 依赖格式错误 | 写成了 `Core:物品粗粮` 之类，缺 `/` 或冒号两边为空 |
| 依赖种类未知 | 冒号后的种类不是 特质/物品/宠物类型/…/配置 |
| 依赖内容缺失 | 模组在，但它没注册你写的内容名（最常见是拼错/改过名） |
| 依赖内容不属于该模组 | 同名内容存在，但注册者是另一个模组（内容名区分大小写） |
| 配置扩容失败 | 内存不足，配置项没有注册（警告） |
| `mod_init` 返回 0 | 模组自己的原因（读 `mod_error()`） |

另外，**内容重名**只记警告：同名内容仍会拿到 Id，但 `find()` 只返回先注册的那个。

宿主侧 API（`ModLoader.h`）：

```c
typedef struct {
    const char *mod;    /* 模组名 */
    const char *file;   /* DLL 文件名 */
    const char *reason; /* 一行原因 */
    const char *detail; /* 详细说明 */
    int warning;        /* 1=警告，0=没加载 */
} ModError_t;

uint64_t          modloader_error_count(void);
const ModError_t *modloader_error_at(uint64_t index);
void              modloader_print_errors(const char *title);
```

失败模块已经注册的内容与配置会被**回滚**，不会留下半个模组。

### 5.3 配置持久化

- 配置值存 `config.dat`（和存档一样整块异或 + 校验字节），按 `name` 读写，与注册顺序无关；
- `def` 只在“没存过这个 name”时使用；模组卸载/改名后，值仍留在文件里；
- `config_set` 会按描述符把值夹到 `min~max`（`CFG_BOOL` 固定 0/1）并立刻落盘；
- 配置页只列**当前已注册**的配置项；宿主自带 `ui.page_size` / `text.stream_ms` 也在这一页里；
- 配置表是动态数组，条数上限由 uint64/内存决定，没有 128 项硬限制；
- 无头模式（`--auto`）配置页只读，避免自检改掉真实配置。

---

## 6. 约定与坑

1. **名字就是存档键**。改名字 = 变成另一个内容，老存档里的东西会当成“不存在”。
   宁可在 `desc` 里改字，也不要改 `name`。
2. **`elem_size` 必须等于结构体大小**，所以一定用 `MOD_ADD`，别手写 `sizeof`。
3. **接口版本**：`api->version != LOADER_API_VERSION` 就 `return fail(...)`，否则加载器升级后
   结构体布局可能变了，会读出垃圾。`api->size` 也要查。
4. **字符串不能释放**：所有 `name/desc/pages` 都是 DLL 只读段里的字面量。
   `rd_*` 那种堆内存只发生在加载器内部（存档读取）。
5. **回调返回值别乱填**：特质/物品返回 1 表示生效；被动要用 `PASSIVE_*`；
   菜单回调是 1/0/-1（继续/返回/退出）。
6. **注册顺序 = Id 顺序**，但初始化顺序由依赖决定（候选先按 DLL 文件名排序，
   依赖就绪即初始化，所以 Id 顺序只保证依赖在前）。名字全局查找，同名先到先得。
7. **跨模组引用用名字**（奖励物品名、宠物类型名、NPC 名、父菜单名），不要传 Id。
8. **无头模式**（`--demo`/`--auto`）下 `key()` 恒返回 `KEY_OK`、`key_poll()` 恒返回 `KEY_OK`、
   `delay()` 不等待、`now_ms()` 虚拟快进——别写“必须按特定键才退出”的死循环，
   用 `is_headless()` 兜底。
9. **实时玩法**：`key_poll()` + `now_ms()`，轮询间隔用 `delay(100)`（和原版一致）；
   不要只写 `while (key_poll() != KEY_OK);` 空转。
10. **文本格式符**：`%NAME%` / `%TYPE%` 替换成宠物名 / 种类，`$K…$k` 关键词（绿，C 键查解释）、
    `$N…$n` 名字（黄）、`$E…$e` 吐槽（灰斜体）、`$C` 清屏、`$P` 等 Z、`$p` 停顿；
    `$A`/`$a` 分支、`$W`/`$w` 循环、`$F`/`$f` 函数，详见 3.9。`§` 老写法已经不支持。
11. **按键**：↑ / ↓ 菜单选择，←/→ 菜单翻页（滑动条里调整数值），Z 确认、X 返回，
    对话里 C 查术语、X 跳过流式输出；内部编码 `KEY_UP=1 KEY_DOWN=2 KEY_OK=3 KEY_CANCEL=4 KEY_TERM=5 KEY_LEFT=6 KEY_RIGHT=7`。
    对话本身不再用 ←/→ 翻页。
12. **菜单 name 全局唯一**：子菜单用 `parent` 引用父菜单，不是指针。
13. **失败要留话**：`mod_init` 的每个 `return 0` 都换成 `return fail("哪一步+为什么")`，
    并导出 `mod_error()`；否则启动报告里只能看到“未提供原因”。
14. **配置 name 是持久化键**，建议 `模块.项`；类型/默认值/范围要写对，`config_set` 依赖描述符
    夹范围。读取用 `config_get`，没存过时返回 0，所以默认值最好大于 0 或在代码里兜底。
15. **菜单每页项数由 `ui.page_size` 决定**，不要在模组里写死 4 行；自定义列表用
    `ui_option(text, selected)`，别再自己画 `>` 箭头。
16. **依赖的内容名必须与注册名完全一致**（区分大小写）；只把“缺了就不工作”的内容写进
    `requires`，可选用 `find` 判断后跳过。
17. **喂食特质特判**：Care 通过特质名字（`贪吃`/`挑食`）检查，不依赖运行时 Id；改特质名要同步改
    Care（或自己的特判代码），否则特判会静默失效。
18. **所有数量都是 uint64_t**：内容/名字/模组/配置/背包/特质的计数与容量都用 `uint64_t`，
    不要写 `uint32_t` 截断表下标；随机取下标用 `rnd64`。
19. **菜单文本必须用 `opt_text` 写**：短串 15 字节/长串动态，直接 `_snprintf(opts[i].text, ...)`
    编译不过；`ui_menu` 会释放选项文本，调用方只释放自己的 `SelectOpt_t` 数组。
20. **滑动条配置**用 `CFG_SLIDER`：配置页显示 `< value >`，←/→ 调整，范围写对（宿主
    `ui.page_size` 是 1~16，`text.stream_ms` 是 0~120）。

---

## 7. 构建、放置、加载

1. 源码放 `mods\src\你的模组.c`；
2. 把名字加进 `build.bat` 的模组列表（`for %%M in (... 你的模组)`）；
3. `build.bat` → 产物在 `mods\你的模组.dll`；
4. `epet.exe`（项目根目录）启动时自动扫描 `mods\`，先做依赖检查再初始化。

临时单个编译：

```bat
cl /nologo /utf-8 /W4 /LD /Imods\incl mods\src\你的模组.c /Fe:mods\你的模组.dll
```

---

## 8. 诊断与自检

```bat
epet.exe --demo     :: 无头自检：加载数、依赖、内容数、剧情/事件/小游戏/存档/死亡
epet.exe --auto     :: 无头把每层菜单点一遍，验证菜单与模组内容能被调度到
```

启动报告里的 `[模组]` 段落就是依赖检查/加载诊断的结果；有问题时 `--demo` 会再报一次条数。
需要完整错误表时，在代码里调用 `modloader_print_errors("模组诊断")`。

常见现象对照：

| 现象 | 原因 |
|---|---|
| 模组没出现在启动列表 | 缺依赖 / 接口版本不符 / `mod_init` 返回 0 —— 看启动 `[模组]` 段落 |
| `MOD_ADD` 返回 `ID_NONE` | 结构体大小与加载器不一致（没 include 同一份 `Defineds.h`） |
| 启动报“接口版本不符” | DLL 是用旧 `Loader.h` 编译的，重新 `build.bat` |
| 启动报“缺少依赖模组 X” | `mods\` 里没有 X.dll，或 `mod_info.name` 拼错 |
| 启动报“依赖循环” | 两个模组的 `requires` 互相等待 |
| 启动报“依赖的模组 X 加载失败” | 先解决 X 自己的错误 |
| 启动报“依赖格式错误/种类未知” | 按 `模组名:种类/名字` 写，种类用中文或英文全称 |
| 启动报“依赖内容缺失” | 内容名拼错，或对方真的没注册；区分大小写 |
| 启动报“依赖内容不属于该模组” | 同名内容由别的模组注册；换名字或改依赖的模组名 |
| 启动报“内容重名（物品/配置）” | 两个模组注册了同名内容；改名或删掉一个（`find` 只认先注册的） |
| 内容注册了但游戏里看不到 | 没加进 `build.bat`，或内容类别不参与该菜单（例如 price=0 不进商店） |
| 菜单不出现 | 忘了 `MOD_ADD(KIND_MENU, ...)` / `parent` 拼错 / `name` 与其他菜单重复 |
| 菜单点进去没反应 | `run` 为 NULL 且没有子菜单（容器是空的） |
| 菜单每页项数不对 | 检查滑动条配置 `ui.page_size`（1~16）；默认 4 |
| 配置改了重启又变回去 | 配置 `name` 改了 / `config.dat` 被删或损坏 / 写盘失败 |
| 选中项没有高亮 | 输出被重定向到文件时没有颜色；真机控制台看背景色 + 反色 |
| 长菜单文本乱码/截断 | 用 `opt_text` 写文本；短串上限 14 字节，长串自动动态分配 |
| 旧存档读不出来 | 本版 `PET_VER=3`，旧存档不兼容；`dead.dat`/`ach.dat`/`config.dat` 也升了版本 |
| 表很大时崩溃/分配失败 | 检查 `uint64_t` 下标是否被截成 32 位；扩容失败会报“内存不足” |
| 实时小游戏判定不对 | 无头模式时间虚拟快进是正常的；真机检查轮询间隔与判定窗口 |
| 成就永远不解锁 | `on_check` 恒 0，或达成条件是 `const Pet*` 里读不到的字段 |
| 剧情不播 | 场景标签拼错 / 该 NPC 分支不存在 / 标志位已置 |
| 存档读回内容消失 | 改了内容 `name`（旧名字查不到就是没有） |

---

## 9. 提交前检查清单

- [ ] `mod_info` / `mod_init` / `mod_exit` 三个导出齐全
- [ ] 实现了 `mod_error()`，所有失败路径都 `return fail("原因")`
- [ ] `mod_init` 校验了 `api->version` 与 `api->size`
- [ ] `requires` 写的是真正的硬依赖；`模组名:种类/名字` 的内容名与注册名完全一致
- [ ] 所有注册都用 `MOD_ADD`
- [ ] 字符串全是字面量，没有 `malloc`/`free`
- [ ] 回调返回值符合约定（菜单是 1/0/-1）
- [ ] 菜单 `name` 全局唯一，`parent` 拼写正确，`order` 合理
- [ ] 自定义列表用 `ui_option`，没有自己画 `>` 箭头
- [ ] 配置 `name` 稳定，`type/def/min/max` 正确，读写用 `config_get/config_set`
- [ ] 表计数/下标用 `uint64_t`，随机下标用 `rnd64`
- [ ] 菜单文本用 `opt_text`，调用 `ui_menu` 后只释放自己的 `SelectOpt_t` 数组
- [ ] 滑动条配置用 `CFG_SLIDER`，范围与默认值一致
- [ ] 名字稳定（当作持久化键）
- [ ] 实时循环用了 `key_poll`/`now_ms`，并且无头模式不会死等
- [ ] `cl /utf-8 /W4` 无 warning
- [ ] `--demo` / `--auto` 跑一遍没有崩溃
- [ ] 已经加进 `build.bat` 的模组列表
