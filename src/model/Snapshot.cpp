#include "model/Snapshot.h"
#include <cstring>
#include <strings.h>

BaseState baseStateFrom(const char* s) {
  if (!s || !*s || !strcmp(s, "-")) return BaseState::Unknown;
  if (!strcasecmp(s, "Idle")) return BaseState::Idle;
  if (!strcasecmp(s, "Charge")) return BaseState::Charge;
  if (!strcasecmp(s, "Dischg")) return BaseState::Dischg;
  if (!strcasecmp(s, "Absent")) return BaseState::Absent;
  if (!strncasecmp(s, "Prot", 4)) return BaseState::Protect;
  return BaseState::Other;
}

Level levelFrom(const char* s) {
  if (!s || !*s || !strcmp(s, "-")) return Level::Unknown;
  if (!strcasecmp(s, "Normal")) return Level::Normal;
  if (!strcasecmp(s, "Low") || !strcasecmp(s, "LowerLow") || !strcasecmp(s, "UnderVolt")) return Level::Low;
  if (!strcasecmp(s, "High") || !strcasecmp(s, "HigherHigh") || !strcasecmp(s, "OverVolt")) return Level::High;
  return Level::Other;
}

const char* baseStateName(BaseState b) {
  switch (b) {
    case BaseState::Idle: return "Idle";
    case BaseState::Charge: return "Charge";
    case BaseState::Dischg: return "Dischg";
    case BaseState::Absent: return "Absent";
    case BaseState::Protect: return "Protect";
    case BaseState::Other: return "Other";
    default: return "Unknown";
  }
}

const char* levelName(Level l) {
  switch (l) {
    case Level::Normal: return "Normal";
    case Level::Low: return "Low";
    case Level::High: return "High";
    case Level::Other: return "Other";
    default: return "Unknown";
  }
}

static void moduleDerived(ModuleData& m) {
  m.cell_min_mv = m.cell_max_mv = m.cell_delta_mv = 0;
  m.cell_min_idx = m.cell_max_idx = m.bal_count = 0;
  m.cell_tmin_mc = m.cell_tmax_mc = NA;
  m.bal_mask = m.alarm_cell_mask = 0;
  if (!m.cells_ts) return;
  bool first = true;
  for (uint8_t i = 0; i < CELLS; i++) {
    const CellData& c = m.cells[i];
    if (c.mv) {
      if (first || c.mv < m.cell_min_mv) { m.cell_min_mv = c.mv; m.cell_min_idx = i; }
      if (first || c.mv > m.cell_max_mv) { m.cell_max_mv = c.mv; m.cell_max_idx = i; }
      first = false;
    }
    if (c.temp_mc != NA) {
      if (m.cell_tmin_mc == NA || c.temp_mc < m.cell_tmin_mc) m.cell_tmin_mc = c.temp_mc;
      if (m.cell_tmax_mc == NA || c.temp_mc > m.cell_tmax_mc) m.cell_tmax_mc = c.temp_mc;
    }
    if (c.bal) { m.bal_count++; m.bal_mask |= (1u << i); }
    bool vbad = c.v_st != Level::Normal && c.v_st != Level::Unknown;
    bool tbad = c.t_st != Level::Normal && c.t_st != Level::Unknown;
    if (vbad || tbad) m.alarm_cell_mask |= (1u << i);
  }
  if (!first) m.cell_delta_mv = m.cell_max_mv - m.cell_min_mv;
}

void computeDerived(Snapshot& s) {
  int64_t sum_mv = 0, sum_ma = 0;
  int n_mv = 0, n_ma = 0, n_soc = 0, sum_soc = 0;
  uint16_t gmin = 0, gmax = 0;
  bool gfirst = true;
  s.sys_bal_count = 0;
  s.sys_tmin_mc = s.sys_tmax_mc = NA;
  for (uint8_t i = 0; i < MAX_MODULES; i++) {
    ModuleData& m = s.m[i];
    moduleDerived(m);
    if (!m.present) continue;
    if (m.mv != NA) { sum_mv += m.mv; n_mv++; }
    if (m.ma != NA) { sum_ma += m.ma; n_ma++; }
    if (m.soc != 0xFF) { sum_soc += m.soc; n_soc++; }
    s.sys_bal_count += m.bal_count;
    if (m.cells_ts && m.cell_max_mv) {
      if (gfirst || m.cell_min_mv < gmin) gmin = m.cell_min_mv;
      if (gfirst || m.cell_max_mv > gmax) gmax = m.cell_max_mv;
      gfirst = false;
    }
    if (m.cell_tmin_mc != NA && (s.sys_tmin_mc == NA || m.cell_tmin_mc < s.sys_tmin_mc)) s.sys_tmin_mc = m.cell_tmin_mc;
    if (m.cell_tmax_mc != NA && (s.sys_tmax_mc == NA || m.cell_tmax_mc > s.sys_tmax_mc)) s.sys_tmax_mc = m.cell_tmax_mc;
  }
  s.sys_mv = n_mv ? (int32_t)(sum_mv / n_mv) : NA;
  s.sys_ma = n_ma ? (int32_t)sum_ma : NA;
  s.sys_soc = n_soc ? (uint8_t)((sum_soc + n_soc / 2) / n_soc) : 0xFF;
  s.sys_delta_mv = gfirst ? 0 : (uint16_t)(gmax - gmin);
}

void updateDeltaStreak(const AlarmCfg& c, ModuleData& m) {
  auto bump = [](uint8_t& v, bool cond) { v = cond ? (uint8_t)(v < 255 ? v + 1 : 255) : 0; };
  bump(m.delta_warn_streak, m.cell_delta_mv >= c.delta_warn_mv);
  bump(m.delta_crit_streak, m.cell_delta_mv >= c.delta_crit_mv);
}

void updateModuleStability(Snapshot& s, uint8_t count, uint32_t now) {
  if (count != s.candidate_modules || !s.candidate_since) {
    s.candidate_modules = count;
    s.candidate_since = now;
  }
  if (now - s.candidate_since >= s.alarm_cfg.stable_modules_ms) s.stable_modules = count;
}

static bool cellsStale(const AlarmCfg& c, const ModuleData& m, uint32_t now) {
  return !m.cells_ts || now - m.cells_ts > c.stale_cells_ms;
}

static bool notNormal(Level l) { return l != Level::Normal && l != Level::Unknown; }

uint16_t moduleAlarm(const Snapshot& s, const ModuleData& m, uint32_t now, bool online) {
  const AlarmCfg& c = s.alarm_cfg;
  uint16_t a = 0;
  if (!online) a |= ALARM_OFFLINE;
  if (cellsStale(c, m, now)) a |= ALARM_STALE_CELLS;
  if (m.delta_warn_streak >= c.delta_streak) a |= ALARM_DELTA_WARN;
  if (m.delta_crit_streak >= c.delta_streak) a |= ALARM_DELTA_CRIT;
  if (m.alarm_cell_mask) a |= ALARM_CELL_STATE;
  if (notNormal(m.v_st) || notNormal(m.c_st) || notNormal(m.t_st) || m.base == BaseState::Protect ||
      m.base == BaseState::Absent)
    a |= ALARM_MODULE_STATE;
  if ((m.cell_tmax_mc != NA && m.cell_tmax_mc >= c.temp_high_mc) || (m.cell_tmin_mc != NA && m.cell_tmin_mc <= c.temp_low_mc))
    a |= ALARM_TEMP_WARN;
  return a;
}

uint16_t systemAlarm(const Snapshot& s, uint32_t now, bool online) {
  uint16_t a = online ? 0 : ALARM_OFFLINE;
  for (uint8_t i = 0; i < MAX_MODULES; i++)
    if (s.m[i].present) a |= moduleAlarm(s, s.m[i], now, online);
  if (s.stable_modules && s.module_count < s.stable_modules) a |= ALARM_MODULE_MISSING;
  return a;
}

uint16_t timeStateKey(const Snapshot& s, uint32_t now, bool online) {
  uint16_t k = online ? 0x8000 : 0;
  for (uint8_t i = 0; i < MAX_MODULES; i++)
    if (s.m[i].present && cellsStale(s.alarm_cfg, s.m[i], now)) k |= (1u << i);
  return k;
}
