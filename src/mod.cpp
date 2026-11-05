// Darknut Ruthless v1: makes every Darknut (the Temple of Time mini-boss and the Hyrule Castle ones) read
// your attacks, block or dodge them, and punish every miss. Nothing else in the game is touched, and the
// Darknut's health is never changed (it keeps its normal armor-break / damage rules).
//
// How it "reads" you:
//   * The moment you PRESS an attack (Link's move id changes), every Darknut that can see it rolls to read it
//     (82% to start, up to 96% when you keep repeating the same move). On a read it snaps to face you, raises
//     its shield from every angle for about 0.8 s, and (armor-off Darknut) may instead leap back or sideways
//     using its own dodge jumps. It uses the game's own guard / dodge states, so animations and sounds are
//     the normal ones.
//   * After 4 blocks in a row it drops its guard for about 1.2 s. That is your opening: the mod stays
//     beatable. Landing any hit resets its streak. (Change kStreakMax / kExposeTicks to taste.)
//   * Counter: when your swing ends, it strikes back within ~0.15 s, then chains up to 2 more swings
//     without waiting for its recovery. It also attacks on its own whenever you are in reach and not
//     swinging, closes distance with its leap and walks/circles faster.
//   * It keeps its own rules for the things only a skilled player can do: the 4th hit of a combo, shield
//     bashes and the helm splitter still work as in the normal game when it is exposed.
//
// Logs: the first line at start-up says whether the two game hooks installed. While a Darknut is near, a status
// line appears every 5 seconds in the Logs window. 30 ticks = 1 second. All tunables are in namespace cfg.

#include "mods/service.hpp"
#include "mods/svc/hook.hpp"
#include "mods/svc/log.hpp"

#include "SSystem/SComponent/c_cc_d.h"
#include "SSystem/SComponent/c_cc_s.h"
#include "d/d_cc_d.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_b_tn.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_iter.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_name.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

DEFINE_MOD();

IMPORT_SERVICE(LogService, svc_log);
IMPORT_OPTIONAL_SERVICE(HookService, svc_hook);  // optional: without it the mod runs without dodge jumps / combo-finisher block

namespace cfg {
// ---- Reading your moves ----
constexpr float kReadBase = 0.82f;          // chance to read a move it has no history for
constexpr float kReadHabitBonus = 0.12f;    // extra chance when you keep repeating one move
constexpr float kReadMax = 0.96f;
constexpr float kReadRange = 520.0f;        // it reads swings started within this distance of it
constexpr float kReadRangeWide = 600.0f;    // spins, jump strikes, down thrust, helm splitter
constexpr float kReadLinkFacingCos = -0.1f; // narrow moves: you must be swinging roughly toward it
constexpr int kReadCooldown = 5;
constexpr float kRecoveryReadMult = 0.75f;  // reading while it is still recovering from its own swing
constexpr int kArmTicks = 24;               // how long a read keeps the guard up (0.8 s)
constexpr int kStreakMax = 4;               // reads in a row before it drops its guard
constexpr int kStreakForget = 150;          // ticks without a read before the streak resets
constexpr int kExposeTicks = 36;            // your opening after kStreakMax reads (1.2 s)
// ---- Dodging (armor-off Darknut; needs the dodge-jump hook) ----
constexpr float kEvadeBackSlash = 0.18f, kEvadeSideSlash = 0.22f;
constexpr float kEvadeBackJump = 0.55f, kEvadeSideJump = 0.20f;
constexpr float kEvadeBackWide = 0.60f;     // spin / great spin / down thrust
constexpr float kEvadeSideHead = 0.55f;     // helm splitter
constexpr float kEvadeBackBash = 0.85f;     // shield bash (it never guards a bash: that would stun it)
// ---- Offense ----
constexpr float kRiposteChance = 0.92f;
constexpr int kRiposteDelay = 4;            // ticks after your swing ends
constexpr int kRiposteWindow = 26;          // it keeps trying for this long if it is busy
constexpr float kRiposteRange = 430.0f;
constexpr float kPressureRange = 380.0f;
constexpr float kPressureChance = 0.30f;    // per tick while ready and in reach
constexpr int kAttackGapH = 34;             // minimum ticks between its own attack starts (armored)
constexpr int kAttackGapL = 28;             // (armor off)
constexpr float kLinkReach = 260.0f;        // do not walk into a swing that is already reaching it
constexpr int kChainMax = 2;                // extra swings without waiting for its recovery
constexpr float kChainChance = 0.65f;
constexpr float kChainRange = 330.0f;
constexpr float kAttackSpeedMult = 1.2f;    // wind-up speed (1.0 = normal)
constexpr int kMaxGameCooldown = 18;        // clamps the game's own attack timer (normally 75-175 ticks)
constexpr float kLeapMin = 480.0f;          // armor-off gap closer
constexpr float kLeapMax = 900.0f;
constexpr float kLeapChance = 0.5f;
constexpr int kLeapCooldown = 75;
// ---- Footwork ----
constexpr int kTurnStep = 0x1000;           // max turn per tick toward you (the game turns ~0x160-0x800)
constexpr float kTurnRange = 700.0f;
constexpr float kEngageRange = 1400.0f;
constexpr float kWalkSpeedH = 6.5f;         // armored walk (game: 4.3), approach (2.3 -> 4.4)
constexpr float kApproachSpeedH = 4.4f;
constexpr float kWalkSpeedL = 7.0f;         // armor-off walk (game: 4.7)
constexpr float kStrafeSpeedL = 6.6f;       // armor-off circling (game: 4.8)
constexpr int kHistorySize = 12;
constexpr int kStatusTicks = 150;
}  // namespace cfg

// Game hooks (declared at file scope).
// ---- Calling the game's own dodge-jump setup, through a pass-through hook (no link dependency) ----
DEFINE_HOOK_SYMBOL("daB_TN_c::initChaseL", void(daB_TN_c*, int), TnInitChaseL);
// ---- The game's hit code: used only to cancel hits the guard cannot take on its own ----
DEFINE_HOOK_SYMBOL("cCcS::SetAtTgCommonHitInf", void(cCcS*, cCcD_Obj*, cCcD_Obj*, cXyz*), AtTgHit);

namespace {

using TN = daB_TN_c;

enum class AType { None, Slash, Jump, Spin, Down, Head, Large, Bash };
enum class St { Ready, Guard, Attack, Hurt, Other };

struct Mem {
    int armed = 0;        // ticks left in the current read
    int readCd = 0;
    int streak = 0;       // reads in a row
    int streakAge = 0;
    int expose = 0;       // ticks left of your opening
    int atkCd = 0;        // ticks until it may start another attack on its own
    int chain = 0;        // swings chained without a break
    int chainAge = 0;
    bool chainRolled = false;
    int leapCd = 0;
    int riposteIn = -1;   // ticks until the riposte window opens
    int riposteTry = 0;   // ticks left of the riposte window
    bool speeded = false;
    bool lastHurt = false;
    bool seen = false;
};

struct Stats {
    int read = 0, guard = 0, evade = 0, negated = 0, landed = 0, riposte = 0, pressure = 0, chain = 0,
        leap = 0, opening = 0;
};

std::unordered_map<const void*, Mem> g_mem;
Stats g_stats;
int g_statTicks = 0;
int g_tick = 0;
int g_logBudget = 16;
bool g_hitHook = false;
bool g_jumpHook = false;

// Link's attack bookkeeping
u16 g_prevProc = 0;
bool g_prevSwinging = false;
bool g_prevLinkAtk = false;
int g_sinceAtkStart = 1000;
int g_sinceAtkEnd = 1000;
AType g_hist[cfg::kHistorySize] = {};
int g_histCount = 0;
int g_histHead = 0;

uint32_t g_rng = 2463534242u;

constexpr float kPi = 3.14159265358979f;
constexpr float kAngleToRad = kPi / 32768.0f;  // game angles: 65536 = full circle

float rand01() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (g_rng & 0xFFFFFF) / float(0x1000000);
}

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

float dist_xz(const fopAc_ac_c* a, const fopAc_ac_c* b) {
    const float dx = a->current.pos.x - b->current.pos.x;
    const float dz = a->current.pos.z - b->current.pos.z;
    return std::sqrt(dx * dx + dz * dz);
}

// Yaw (game angle) that points from `from` toward `to`.
s16 angle_to(const fopAc_ac_c* from, const fopAc_ac_c* to) {
    const float dx = to->current.pos.x - from->current.pos.x;
    const float dz = to->current.pos.z - from->current.pos.z;
    const int a = static_cast<int>(std::lround(std::atan2(dx, dz) / kAngleToRad));
    return static_cast<s16>(static_cast<uint16_t>(a & 0xFFFF));
}

int angle_gap(const TN* t, s16 target) {
    return std::abs(static_cast<int>(static_cast<s16>(target - t->shape_angle.y)));
}

void turn_toward(TN* t, s16 target, int maxStep) {
    const int diff = static_cast<int>(static_cast<s16>(target - t->shape_angle.y));
    const int step = std::clamp(diff, -maxStep, maxStep);
    t->shape_angle.y = static_cast<s16>(t->shape_angle.y + step);
}

bool h_mode(const TN* t) {
    return t->mActionMode1 < TN::ACT_CHANGEDEMO;  // armored (the shield-and-armor phase)
}

St classify(const TN* t) {
    switch (t->mActionMode1) {
    case TN::ACT_CHASEH: return St::Ready;
    case TN::ACT_CHASEL: {
        const int m2 = t->mActionMode2;
        return (m2 <= 5 || m2 == 16) ? St::Ready : St::Other;  // not mid-leap
    }
    case TN::ACT_GUARDH:
    case TN::ACT_GUARDL: return St::Guard;
    case TN::ACT_ATTACKH:
    case TN::ACT_ATTACKSHIELDH:
    case TN::ACT_ATTACKL:
    case TN::ACT_ATTACKSHIELDL: return St::Attack;
    case TN::ACT_DAMAGEH:
    case TN::ACT_DAMAGEL:
    case TN::ACT_YOROKE: return St::Hurt;
    default: return St::Other;  // demos, waiting, ending
    }
}

// Its own swing is past the part that can hurt you and is only recovering.
bool in_recovery(const TN* t) {
    if (t->mpModelMorf2 == nullptr) {
        return false;
    }
    const float f = t->mpModelMorf2->getFrame();
    const int m2 = t->mActionMode2;
    if (t->mActionMode1 == TN::ACT_ATTACKH) {
        if (m2 == 1) {
            return f >= 41.0f;
        }
        if (m2 == 2) {
            return f >= 37.0f;
        }
    } else if (t->mActionMode1 == TN::ACT_ATTACKL) {
        if (m2 == 1 || m2 == 101) {
            return f >= 16.0f;
        }
        if (m2 == 2 || m2 == 102) {
            return f >= 40.0f;
        }
    }
    return false;
}

// Same entry the game itself uses (setActionMode): two fields and the head-lock flag.
void enter(TN* t, int mode1, int mode2) {
    t->offHeadLockFlg();
    t->mActionMode1 = mode1;
    t->mActionMode2 = mode2;
}

AType attack_type_of(u16 proc) {
    switch (proc) {
    case daAlink_c::PROC_CUT_NORMAL:
    case daAlink_c::PROC_CUT_FINISH:
    case daAlink_c::PROC_CUT_FINISH_JUMP_UP: return AType::Slash;
    case daAlink_c::PROC_CUT_JUMP: return AType::Jump;
    case daAlink_c::PROC_CUT_TURN: return AType::Spin;
    case daAlink_c::PROC_CUT_DOWN: return AType::Down;
    case daAlink_c::PROC_CUT_HEAD: return AType::Head;
    case daAlink_c::PROC_CUT_LARGE_JUMP: return AType::Large;
    case daAlink_c::PROC_GUARD_ATTACK: return AType::Bash;
    default: return AType::None;
    }
}

const char* atype_name(AType t) {
    switch (t) {
    case AType::Slash: return "slash";
    case AType::Jump: return "jump strike";
    case AType::Spin: return "spin attack";
    case AType::Down: return "down thrust";
    case AType::Head: return "helm splitter";
    case AType::Large: return "great spin";
    case AType::Bash: return "shield bash";
    default: return "attack";
    }
}

bool wide_move(AType t) {
    return t == AType::Spin || t == AType::Down || t == AType::Large || t == AType::Jump || t == AType::Head;
}

void remember_attack(AType t) {
    g_hist[g_histHead] = t;
    g_histHead = (g_histHead + 1) % cfg::kHistorySize;
    if (g_histCount < cfg::kHistorySize) {
        g_histCount++;
    }
}

float habit_share(AType t) {
    if (g_histCount < 4) {
        return 0.0f;
    }
    int n = 0;
    for (int i = 0; i < g_histCount; i++) {
        if (g_hist[i] == t) {
            n++;
        }
    }
    return float(n) / float(g_histCount);
}

void log_event(const char* what, const TN* t, AType type, float dist) {
    if (g_logBudget <= 0) {
        return;
    }
    g_logBudget--;
    mods::log::info("darknut: {} ({}) - your {} at {:.0f} units", what, h_mode(t) ? "armored" : "armor off",
                    atype_name(type), dist);
}

// Face you, shield up from every angle (the game's own "mTimer10": guard from anywhere).
void raise_guard(TN* t, const fopAc_ac_c* plr, int ticks) {
    turn_toward(t, angle_to(t, plr), 0x7FFF);
    t->field_0xa91 = true;
    if (t->mTimer10 < ticks) {
        t->mTimer10 = static_cast<u8>(std::min(ticks, 200));
    }
}

void enter_guard_state(TN* t, const fopAc_ac_c* plr) {
    raise_guard(t, plr, cfg::kArmTicks + 8);
    t->field_0xaa8 = false;
    if (h_mode(t)) {
        enter(t, TN::ACT_GUARDH, TN::ACTION2_0_e);
    } else {
        // The armor-off guard animation reads the collider of the last hit; with no hit it would be null.
        if (t->mAtInfo.mpCollider == nullptr) {
            t->mAtInfo.mpCollider = &t->mSwordSphs[0];
        }
        t->mGuardCounterKick = false;
        enter(t, TN::ACT_GUARDL, TN::ACTION2_0_e);
    }
}

// which: 6 = back-jump, 10 = jump right, 13 = jump left (the game's own dodge jumps)
bool do_dodge(TN* t, int which) {
    if (!g_jumpHook || TnInitChaseL::g_orig == nullptr || h_mode(t)) {
        return false;
    }
    enter(t, TN::ACT_CHASEL, TN::ACTION2_0_e);
    TnInitChaseL::g_orig(t, which);
    if (which != 6) {
        t->mTimer7 = 10;
    }
    return true;
}

void start_attack(TN* t, Mem& m, float dist) {
    if (h_mode(t)) {
        // like the game: the shield kick at point-blank, unless its armor is nearly gone
        enter(t, (dist < 200.0f && t->mNextBreakPart < 11) ? TN::ACT_ATTACKSHIELDH : TN::ACT_ATTACKH,
              TN::ACTION2_0_e);
    } else {
        enter(t, dist < 200.0f ? TN::ACT_ATTACKSHIELDL : TN::ACT_ATTACKL, TN::ACTION2_0_e);
    }
    m.atkCd = h_mode(t) ? cfg::kAttackGapH : cfg::kAttackGapL;
    m.chainRolled = false;
    m.speeded = false;
    m.armed = 0;
}

float read_chance(AType t, bool recovering) {
    const float habit = clampf((habit_share(t) - 0.25f) / 0.5f, 0.0f, 1.0f);
    float c = std::min(cfg::kReadMax, cfg::kReadBase + cfg::kReadHabitBonus * habit);
    if (recovering) {
        c *= cfg::kRecoveryReadMult;
    }
    return c;
}

// Returns true if the Darknut reacted (read the move).
bool try_read(TN* t, Mem& m, const fopAc_ac_c* plr, AType type, St st, float dist, float fx, float fz) {
    if (m.expose > 0 || m.readCd > 0) {
        return false;
    }
    const bool recovering = (st == St::Attack) && in_recovery(t);
    if (!(st == St::Ready || st == St::Guard || recovering)) {
        return false;
    }
    const bool wide = wide_move(type);
    if (dist > (wide ? cfg::kReadRangeWide : cfg::kReadRange) || dist < 1.0f) {
        return false;
    }
    if (!wide) {
        const float ex = (t->current.pos.x - plr->current.pos.x) / dist;  // you -> it
        const float ez = (t->current.pos.z - plr->current.pos.z) / dist;
        if (ex * fx + ez * fz < cfg::kReadLinkFacingCos) {
            return false;
        }
    }
    const bool canDodge = g_jumpHook && TnInitChaseL::g_orig != nullptr && !h_mode(t);
    if (type == AType::Bash && !canDodge) {
        return false;  // a bash is meant to break its guard: only a dodge makes sense
    }
    if (rand01() >= read_chance(type, recovering)) {
        return false;
    }

    m.readCd = cfg::kReadCooldown;
    m.streakAge = 0;
    m.riposteTry = 0;
    m.riposteIn = -1;
    g_stats.read++;
    m.streak++;
    m.armed = cfg::kArmTicks;
    if (m.streak >= cfg::kStreakMax) {
        m.streak = 0;
        m.expose = cfg::kExposeTicks;
        g_stats.opening++;
    }

    // Choose a dodge for the armor-off Darknut (the armored one has no dodge jumps and always guards).
    if (canDodge) {
        float back = 0.0f, side = 0.0f;
        switch (type) {
        case AType::Slash: back = cfg::kEvadeBackSlash; side = cfg::kEvadeSideSlash; break;
        case AType::Jump: back = cfg::kEvadeBackJump; side = cfg::kEvadeSideJump; break;
        case AType::Spin:
        case AType::Large:
        case AType::Down: back = cfg::kEvadeBackWide; break;
        case AType::Head: side = cfg::kEvadeSideHead; break;
        case AType::Bash: back = cfg::kEvadeBackBash; break;
        default: break;
        }
        const float r = rand01();
        if (r < back) {
            if (do_dodge(t, 6)) {
                g_stats.evade++;
                log_event("read -> back-jump", t, type, dist);
                return true;
            }
        } else if (r < back + side) {
            if (do_dodge(t, rand01() < 0.5f ? 10 : 13)) {
                g_stats.evade++;
                log_event("read -> side-jump", t, type, dist);
                return true;
            }
        }
    }
    if (type == AType::Bash) {
        m.armed = 0;
        return false;
    }
    enter_guard_state(t, plr);
    g_stats.guard++;
    log_event("read -> guard", t, type, dist);
    return true;
}

int collect_tn(void* actor, void* data) {
    auto* list = static_cast<std::vector<fopAc_ac_c*>*>(data);
    auto* a = static_cast<fopAc_ac_c*>(actor);
    if (fopAcM_GetProfName(a) == fpcNm_B_TN_e) {
        list->push_back(a);
    }
    return 1;
}

void reset_link_state() {
    g_prevProc = 0;
    g_prevSwinging = false;
    g_prevLinkAtk = false;
    g_sinceAtkStart = 1000;
    g_sinceAtkEnd = 1000;
}

constexpr u32 kHitMask = u32(AT_TYPE_NORMAL_SWORD) | u32(AT_TYPE_MASTER_SWORD) | u32(AT_TYPE_SHIELD_ATTACK) |
                         u32(AT_TYPE_WOLF_CUT_TURN) | u32(AT_TYPE_WOLF_ATTACK);

// Hits the Darknut's guard cannot take by itself are cancelled while it has read your move: the 4th hit of a
// combo (armor off: that is how the game staggers it) and any hit once its armor is nearly gone.
HookAction on_at_tg_hit(ModContext*, void* args, void*, void*) {
    cCcD_Obj* at = mods::arg<cCcD_Obj*>(args, 1);
    cCcD_Obj* tg = mods::arg<cCcD_Obj*>(args, 2);
    if (at == nullptr || tg == nullptr || (u32(at->GetAtType()) & kHitMask) == 0) {
        return HOOK_CONTINUE;
    }
    cCcD_Stts* as = at->GetStts();
    cCcD_Stts* ts = tg->GetStts();
    fopAc_ac_c* plr = dComIfGp_getPlayer(0);
    if (as == nullptr || ts == nullptr || plr == nullptr) {
        return HOOK_CONTINUE;
    }
    fopAc_ac_c* dst = ts->GetAc();
    if (as->GetAc() != plr || dst == nullptr || dst == plr || fopAcM_GetProfName(dst) != fpcNm_B_TN_e) {
        return HOOK_CONTINUE;
    }
    auto it = g_mem.find(dst);
    if (it == g_mem.end() || it->second.armed <= 0) {
        return HOOK_CONTINUE;
    }
    auto* t = static_cast<TN*>(dst);
    const auto* py = static_cast<daPy_py_c*>(plr);
    const bool cancel = h_mode(t) ? (t->mNextBreakPart >= 11) : (py->getCutCount() >= 4);
    if (cancel) {
        g_stats.negated++;
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

void tick() {
    fopAc_ac_c* plr = dComIfGp_getPlayer(0);
    if (plr == nullptr || dComIfGp_event_runCheck()) {
        reset_link_state();
        g_mem.clear();
        return;
    }
    auto* py = static_cast<daPy_py_c*>(plr);
    auto* link = static_cast<daAlink_c*>(plr);
    g_tick++;

    // ---- What is Link doing? ----
    const bool swinging = py->getCutAtFlg();
    const bool jumpStrike = py->checkCutJumpMode();
    const u16 proc = link->mProcID;
    const AType atype = attack_type_of(proc);
    const bool linkAtk = swinging || jumpStrike || atype != AType::None;
    g_sinceAtkStart++;
    g_sinceAtkEnd++;
    const bool procStart = atype != AType::None && proc != g_prevProc;
    const bool flagStart = swinging && !g_prevSwinging && g_sinceAtkStart > 8;
    const bool attackStart = procStart || flagStart;
    const AType startType = (atype != AType::None) ? atype : AType::Slash;
    if (attackStart) {
        g_sinceAtkStart = 0;
        remember_attack(startType);
    }
    const bool attackEnd = !linkAtk && g_prevLinkAtk;
    if (attackEnd) {
        g_sinceAtkEnd = 0;
    }
    g_prevLinkAtk = linkAtk;
    g_prevProc = proc;
    g_prevSwinging = swinging;

    const float facing = plr->shape_angle.y * kAngleToRad;
    const float fx = std::sin(facing);  // your forward direction on the ground
    const float fz = std::cos(facing);

    std::vector<fopAc_ac_c*> all;
    fopAcIt_Executor(collect_tn, &all);

    std::unordered_map<const void*, Mem> alive;
    int engaged = 0;
    for (fopAc_ac_c* a : all) {
        const float dist = dist_xz(a, plr);
        if (dist > cfg::kEngageRange) {
            continue;
        }
        auto* en = static_cast<fopEn_enemy_c*>(a);
        if (en->checkDeadFlg() || en->checkDownFlg()) {
            continue;
        }
        auto* t = static_cast<TN*>(a);
        if (t->mInactive || t->mDisappear) {
            continue;
        }
        engaged++;

        auto it = g_mem.find(a);
        Mem m = (it != g_mem.end()) ? it->second : Mem{};
        if (!m.seen) {
            m.seen = true;
            mods::log::info("darknut: found one (type {}, state {}/{}, {:.0f} units away)", int(t->mType),
                            t->mActionMode1, t->mActionMode2, dist);
        }

        // timers
        if (m.armed > 0) {
            m.armed--;
        } else if (m.expose > 0) {
            m.expose--;
        }
        if (m.readCd > 0) m.readCd--;
        if (m.atkCd > 0) m.atkCd--;
        if (m.leapCd > 0) m.leapCd--;
        if (m.streak > 0 && ++m.streakAge > cfg::kStreakForget) {
            m.streak = 0;
            m.streakAge = 0;
        }

        const St st = classify(t);
        const bool lmode = !h_mode(t);
        const s16 toLink = angle_to(t, plr);
        bool changed = false;

        // It got hit: it starts over.
        const bool hurt = (st == St::Hurt);
        if (hurt && !m.lastHurt) {
            g_stats.landed++;
            m.streak = 0;
            m.armed = 0;
            m.expose = 0;
        }
        m.lastHurt = hurt;

        if (st == St::Attack) {
            m.chainAge = 0;
        } else {
            m.speeded = false;     // the game started its next swing by itself: boost that one too
            m.chainRolled = false;
            if (++m.chainAge > 40) {
                m.chain = 0;
            }
        }

        // ---- 1. read the move you just started ----
        if (attackStart) {
            if (try_read(t, m, plr, startType, st, dist, fx, fz)) {
                changed = true;
            }
        }

        // ---- 2. keep the read going: face you, guard from every angle; or open up after the streak ----
        if (!changed && (st == St::Ready || st == St::Guard)) {
            if (m.armed > 0) {
                raise_guard(t, plr, 6);
            } else if (m.expose > 0) {
                t->field_0xa91 = false;
                t->mTimer10 = 0;
            }
        }

        // ---- 3. riposte: your swing just ended ----
        if (attackEnd && dist < cfg::kRiposteRange && rand01() < cfg::kRiposteChance) {
            m.riposteIn = cfg::kRiposteDelay;
            m.riposteTry = 0;
        }
        if (m.riposteIn > 0 && --m.riposteIn == 0) {
            m.riposteTry = cfg::kRiposteWindow;
            m.riposteIn = -1;
        }
        if (m.riposteTry > 0) {
            m.riposteTry--;
            if (m.armed > 0 || linkAtk) {
                m.riposteTry = 0;  // you are swinging again: it reads instead
            } else if (!changed && st == St::Ready && dist < cfg::kRiposteRange &&
                       angle_gap(t, toLink) < 0x4000) {
                start_attack(t, m, dist);
                g_stats.riposte++;
                m.riposteTry = 0;
                changed = true;
            }
        }

        // ---- 4. chain: swing again without waiting out its recovery ----
        if (!changed && st == St::Attack && m.armed == 0 && in_recovery(t) && !m.chainRolled) {
            m.chainRolled = true;
            if (m.chain < cfg::kChainMax && dist < cfg::kChainRange && angle_gap(t, toLink) < 0x3800 &&
                rand01() < cfg::kChainChance) {
                m.chain++;
                start_attack(t, m, dist);
                g_stats.chain++;
                changed = true;
            }
        }

        // ---- 5. pressure: you are in reach and not swinging, so it attacks ----
        if (!changed && st == St::Ready) {
            if (t->mTimer3 > cfg::kMaxGameCooldown) {
                t->mTimer3 = cfg::kMaxGameCooldown;
            }
            const bool linkReaching = linkAtk && dist < cfg::kLinkReach;
            if (m.atkCd == 0 && m.armed == 0 && !linkReaching && dist < cfg::kPressureRange &&
                angle_gap(t, toLink) < 0x3800 && rand01() < cfg::kPressureChance) {
                start_attack(t, m, dist);
                m.chain = 0;
                g_stats.pressure++;
                changed = true;
            } else if (lmode && g_jumpHook && TnInitChaseL::g_orig != nullptr && m.leapCd == 0 && m.atkCd == 0 &&
                       m.armed == 0 && dist > cfg::kLeapMin && dist < cfg::kLeapMax &&
                       angle_gap(t, toLink) < 0x2000 && rand01() < cfg::kLeapChance * 0.1f) {
                enter(t, TN::ACT_CHASEL, TN::ACTION2_0_e);
                TnInitChaseL::g_orig(t, dist < 700.0f ? 8 : 17);  // the game's own forward leap
                m.leapCd = cfg::kLeapCooldown;
                g_stats.leap++;
                changed = true;
            }
        }

        // ---- 6. footwork: turn to you quickly, walk and circle faster (absolute speeds, not stacking) ----
        if (!changed && (st == St::Ready || st == St::Guard) && dist < cfg::kTurnRange && m.expose == 0) {
            turn_toward(t, toLink, cfg::kTurnStep);
        }
        if (!changed && st == St::Ready) {
            const int m2 = t->mActionMode2;
            if (!lmode) {
                if (m2 == TN::ACTION2_9_e && dist > 450.0f) {
                    t->speedF = cfg::kWalkSpeedH;
                } else if (m2 == TN::ACTION2_2_e && dist > 320.0f) {
                    t->speedF = cfg::kApproachSpeedH;
                }
            } else {
                if (m2 == TN::ACTION2_3_e && dist > 450.0f) {
                    t->speedF = cfg::kWalkSpeedL;
                } else if (m2 == TN::ACTION2_16_e) {
                    t->speedF = cfg::kWalkSpeedL;
                } else if (m2 == TN::ACTION2_4_e || m2 == TN::ACTION2_5_e) {
                    t->speedF = cfg::kStrafeSpeedL;
                }
            }
        }

        // ---- 7. faster wind-up, once per swing ----
        if (st == St::Attack && !m.speeded && !changed && t->mpModelMorf2 != nullptr &&
            (t->mActionMode2 == 1 || t->mActionMode2 == 2)) {
            m.speeded = true;
            t->mpModelMorf2->setPlaySpeed(t->mpModelMorf2->getPlaySpeed() * cfg::kAttackSpeedMult);
        }

        alive[a] = m;
    }
    g_mem.swap(alive);

    if (engaged > 0 && ++g_statTicks >= cfg::kStatusTicks) {
        g_statTicks = 0;
        mods::log::info(
            "darknut: n={} reads={} guard={} dodge={} cancelled={} opened={} | its attacks: riposte={} pressure={} "
            "chain={} leap={} | your hits landed={} | hooks: hit={} dodge={}",
            engaged, g_stats.read, g_stats.guard, g_stats.evade, g_stats.negated, g_stats.opening,
            g_stats.riposte, g_stats.pressure, g_stats.chain, g_stats.leap, g_stats.landed,
            g_hitHook ? "ON" : "OFF", g_jumpHook ? "ON" : "OFF");
        g_stats = Stats{};
    } else if (engaged == 0) {
        g_statTicks = 0;
    }
}

}  // namespace

extern "C" {
MOD_EXPORT ModResult mod_initialize(ModError*) {
    const ModResult hr = mods::hook::add_pre<AtTgHit>(on_at_tg_hit);
    g_hitHook = (hr == MOD_OK);
    const ModResult jr = mods::hook::install<TnInitChaseL>();
    g_jumpHook = (jr == MOD_OK) && TnInitChaseL::g_orig != nullptr;
    mods::log::info(
        "darknut_ruthless v1 initialized | combo-finisher hook {} | dodge-jump hook {} | status line every 5 s "
        "near a Darknut{}",
        g_hitHook ? "ON" : "UNAVAILABLE", g_jumpHook ? "ON" : "UNAVAILABLE",
        (g_hitHook && g_jumpHook)
            ? ""
            : " | a hook says UNAVAILABLE: your game build has no symbol manifest. Guarding, counters and chaining "
              "still work; armor-off dodge jumps / combo-finisher cancel need it.");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    tick();
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    g_mem.clear();
    g_stats = Stats{};
    g_statTicks = 0;
    g_tick = 0;
    g_histCount = 0;
    g_histHead = 0;
    reset_link_state();
    return MOD_OK;
}
}
