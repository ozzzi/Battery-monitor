#pragma once

#include <cstdint>
#include <climits>

constexpr uint8_t MAX_MODULES = 8;
constexpr uint8_t CELLS = 15;
constexpr int32_t NA = INT32_MIN;

enum class BaseState : uint8_t { Unknown, Idle, Charge, Dischg, Absent, Protect, Other };
enum class Level : uint8_t { Unknown, Normal, Low, High, Other };

BaseState baseStateFrom(const char* s);
Level levelFrom(const char* s);
const char* baseStateName(BaseState b);
const char* levelName(Level l);

struct CellData {
  uint16_t mv = 0;
  int32_t  temp_mc = NA;
  Level    v_st = Level::Unknown, t_st = Level::Unknown;
  bool     bal = false;
  bool     bal_known = false;
  uint8_t  soc = 0xFF;
  uint32_t mah = 0xFFFFFFFF;
};

struct ModuleData {
  bool      present = false;
  int32_t   mv = NA, ma = NA;
  int32_t   temp_mc = NA, tlow_mc = NA, thigh_mc = NA, mos_mc = NA;
  uint16_t  vlow_mv = 0, vhigh_mv = 0;
  uint8_t   soc = 0xFF;
  uint32_t  remain_mah = 0xFFFFFFFF;
  BaseState base = BaseState::Unknown;
  Level     v_st = Level::Unknown, c_st = Level::Unknown, t_st = Level::Unknown;
  char      base_raw[9] = {0};
  CellData  cells[CELLS];
  uint32_t  pwr_ts = 0, cells_ts = 0, stat_ts = 0;
  uint16_t  cycles = 0xFFFF;
  uint8_t   soh = 0xFF;
  char      serial[24] = {0}, fw[24] = {0};
  uint16_t  cell_min_mv = 0, cell_max_mv = 0, cell_delta_mv = 0;
  uint8_t   cell_min_idx = 0, cell_max_idx = 0, bal_count = 0;
  int32_t   cell_tmin_mc = NA, cell_tmax_mc = NA;
  uint16_t  bal_mask = 0, alarm_cell_mask = 0;
  uint8_t   delta_warn_streak = 0, delta_crit_streak = 0;
};

enum AlarmBit : uint16_t {
  ALARM_OFFLINE = 1 << 0,
  ALARM_STALE_CELLS = 1 << 1,
  ALARM_DELTA_WARN = 1 << 2,
  ALARM_DELTA_CRIT = 1 << 3,
  ALARM_CELL_STATE = 1 << 4,
  ALARM_MODULE_STATE = 1 << 5,
  ALARM_TEMP_WARN = 1 << 6,
  ALARM_MODULE_MISSING = 1 << 7,
};

struct AlarmCfg {
  uint16_t delta_warn_mv = 30, delta_crit_mv = 50;
  uint8_t  delta_streak = 3;
  uint32_t stale_cells_ms = 90000;
  int32_t  temp_high_mc = 45000, temp_low_mc = 5000;
  uint32_t stable_modules_ms = 600000;
};

struct Snapshot {
  uint32_t   seq = 0;
  uint8_t    module_count = 0;
  ModuleData m[MAX_MODULES];
  int32_t    sys_mv = NA, sys_ma = NA;
  uint8_t    sys_soc = 0xFF;
  uint16_t   sys_delta_mv = 0;
  uint8_t    sys_bal_count = 0;
  int32_t    sys_tmin_mc = NA, sys_tmax_mc = NA;
  uint32_t   last_ok_ts = 0;
  bool       has_data = false;
  AlarmCfg   alarm_cfg;
  uint8_t    stable_modules = 0;
  uint8_t    candidate_modules = 0;
  uint32_t   candidate_since = 0;
};

void computeDerived(Snapshot& s);
void updateDeltaStreak(const AlarmCfg& c, ModuleData& m);
void updateModuleStability(Snapshot& s, uint8_t count, uint32_t now);
uint16_t moduleAlarm(const Snapshot& s, const ModuleData& m, uint32_t now, bool online);
uint16_t systemAlarm(const Snapshot& s, uint32_t now, bool online);
uint16_t timeStateKey(const Snapshot& s, uint32_t now, bool online);
