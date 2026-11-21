#include <unity.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <sstream>
#include <string>
#include "console/Parser.h"

using namespace parser;

static std::string load(const char* rel) {
  std::ifstream f(std::string(FIXTURES_DIR) + "/" + rel, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  std::string s = ss.str();
  if (s.rfind("#PYLONMON", 0) == 0) s.erase(0, s.find('\n') + 1);
  return s;
}

void setUp() {}
void tearDown() {}

void test_header_normalize() {
  Tokens h = normalizeHeader(splitWs("Battery Volt Base State Volt. State Base.St B.V.St MosTempr"));
  TEST_ASSERT_EQUAL(7, h.size());
  TEST_ASSERT_EQUAL_STRING("BaseSt", h[2].c_str());
  TEST_ASSERT_EQUAL_STRING("VoltSt", h[3].c_str());
  TEST_ASSERT_EQUAL_STRING("BaseSt", h[4].c_str());
  TEST_ASSERT_EQUAL_STRING("BVSt", h[5].c_str());
}

void test_data_normalize() {
  Tokens d = normalizeData(splitWs("1 52310 87% 2026-11-09 14:02:15 43500 mAH N"));
  TEST_ASSERT_EQUAL(6, d.size());
  TEST_ASSERT_EQUAL_STRING("87", d[2].c_str());
  TEST_ASSERT_EQUAL_STRING("2026-11-09_14:02:15", d[3].c_str());
  TEST_ASSERT_EQUAL_STRING("43500", d[4].c_str());
}

void test_parse_milli() {
  int32_t v;
  TEST_ASSERT_TRUE(parseMilli("22000", v)); TEST_ASSERT_EQUAL(22000, v);
  TEST_ASSERT_TRUE(parseMilli("22.5", v));  TEST_ASSERT_EQUAL(22500, v);
  TEST_ASSERT_TRUE(parseMilli("-1.2", v));  TEST_ASSERT_EQUAL(-1200, v);
  TEST_ASSERT_FALSE(parseMilli("-", v));
}

void test_pwr() {
  std::string txt = load("synthetic/pwr_us2000c.txt");
  PwrResult r; std::string err;
  TEST_ASSERT_TRUE_MESSAGE(parsePwr(txt, r, err), err.c_str());
  TEST_ASSERT_TRUE(completedOk(txt));
  TEST_ASSERT_EQUAL(2, r.present);
  TEST_ASSERT_EQUAL(4, r.rows.size());
  TEST_ASSERT_EQUAL(52310, r.rows[0].mv);
  TEST_ASSERT_EQUAL(-6170, r.rows[0].ma);
  TEST_ASSERT_EQUAL(20500, r.rows[0].t);
  TEST_ASSERT_EQUAL(87, r.rows[0].soc);
  TEST_ASSERT_EQUAL(22100, r.rows[0].mos);
  TEST_ASSERT_EQUAL_STRING("Dischg", r.rows[0].base.c_str());
  TEST_ASSERT_TRUE(r.rows[2].absent);
}

void test_bat_and_derived() {
  Snapshot s;
  PwrResult p; BatResult b; std::string err;
  TEST_ASSERT_TRUE(parsePwr(load("synthetic/pwr_us2000c.txt"), p, err));
  applyPwr(p, s, 1000);
  TEST_ASSERT_TRUE_MESSAGE(parseBat(load("synthetic/bat_us2000c.txt"), b, err), err.c_str());
  TEST_ASSERT_TRUE(applyBat(1, b, s, 2000, err));
  const ModuleData& m = s.m[0];
  TEST_ASSERT_EQUAL(2, s.module_count);
  TEST_ASSERT_EQUAL(3485, m.cell_min_mv);
  TEST_ASSERT_EQUAL(7, m.cell_min_idx);
  TEST_ASSERT_EQUAL(3499, m.cell_max_mv);
  TEST_ASSERT_EQUAL(1, m.cell_max_idx);
  TEST_ASSERT_EQUAL(14, m.cell_delta_mv);
  TEST_ASSERT_EQUAL(2, m.bal_count);
  TEST_ASSERT_EQUAL_HEX16((1 << 1) | (1 << 8), m.bal_mask);
  TEST_ASSERT_EQUAL(43500, m.remain_mah);
  TEST_ASSERT_EQUAL(2, s.sys_bal_count);
  TEST_ASSERT_EQUAL(-12320, s.sys_ma);
  TEST_ASSERT_EQUAL(87, s.sys_soc);
  TEST_ASSERT_EQUAL(2, s.seq);
}

void test_bat_without_soc_bal() {
  BatResult b; std::string err;
  TEST_ASSERT_TRUE_MESSAGE(parseBat(load("synthetic/bat_nosoc_nobal.txt"), b, err), err.c_str());
  TEST_ASSERT_EQUAL(-1, b.cells[0].bal);
  TEST_ASSERT_EQUAL(3310, b.cells[0].mv);
  TEST_ASSERT_EQUAL(3324, b.cells[14].mv);
}

void test_bat_truncated_rejected() {
  BatResult b; std::string err;
  TEST_ASSERT_FALSE(parseBat(load("synthetic/bat_truncated.txt"), b, err));
  TEST_ASSERT_TRUE(err.find("15 cells") != std::string::npos);
}

void test_garbage_rejected() {
  PwrResult p; BatResult b; std::string err;
  TEST_ASSERT_FALSE(parsePwr("pwr\r\nUnknown command\r\npylon>", p, err));
  TEST_ASSERT_FALSE(parseBat("", b, err));
}


void test_real_pwr_16_rows() {
  PwrResult r; std::string err;
  TEST_ASSERT_TRUE_MESSAGE(parsePwr(load("real/pwr_idle_2mod.txt"), r, err), err.c_str());
  TEST_ASSERT_EQUAL(2, r.present);
  TEST_ASSERT_EQUAL(51788, r.rows[0].mv);
  TEST_ASSERT_EQUAL(0, r.rows[0].ma);
  TEST_ASSERT_EQUAL(14300, r.rows[0].t);
  TEST_ASSERT_EQUAL(3434, r.rows[0].vlow);
  TEST_ASSERT_EQUAL(3456, r.rows[0].vhigh);
  TEST_ASSERT_EQUAL(94, r.rows[0].soc);
  TEST_ASSERT_EQUAL(12200, r.rows[0].mos);
  TEST_ASSERT_EQUAL_STRING("Idle", r.rows[0].base.c_str());
  TEST_ASSERT_EQUAL(95, r.rows[1].soc);
  TEST_ASSERT_EQUAL(13300, r.rows[1].mos);
  TEST_ASSERT_EQUAL(MAX_MODULES, r.rows.size());
  TEST_ASSERT_TRUE(r.rows[7].absent);
}

void test_real_bat_two_modules() {
  Snapshot s;
  PwrResult p; BatResult b1, b2; std::string err;
  TEST_ASSERT_TRUE(parsePwr(load("real/pwr_idle_2mod.txt"), p, err));
  applyPwr(p, s, 1000);
  TEST_ASSERT_TRUE_MESSAGE(parseBat(load("real/bat1_idle.txt"), b1, err), err.c_str());
  TEST_ASSERT_TRUE_MESSAGE(parseBat(load("real/bat2_idle.txt"), b2, err), err.c_str());
  TEST_ASSERT_TRUE(applyBat(1, b1, s, 2000, err));
  TEST_ASSERT_TRUE(applyBat(2, b2, s, 3000, err));
  const ModuleData& m1 = s.m[0];
  TEST_ASSERT_EQUAL(12200, m1.cells[0].temp_mc);
  TEST_ASSERT_EQUAL(12600, m1.cells[5].temp_mc);
  TEST_ASSERT_EQUAL(12500, m1.cells[10].temp_mc);
  TEST_ASSERT_EQUAL(3434, m1.cell_min_mv);
  TEST_ASSERT_EQUAL(12, m1.cell_min_idx);
  TEST_ASSERT_EQUAL(3454, m1.cell_max_mv);
  TEST_ASSERT_EQUAL(5, m1.cell_max_idx);
  TEST_ASSERT_EQUAL(20, m1.cell_delta_mv);
  TEST_ASSERT_EQUAL(94, m1.cells[12].soc);
  TEST_ASSERT_EQUAL(100, m1.cells[0].soc);
  TEST_ASSERT_EQUAL(45855, m1.remain_mah);
  TEST_ASSERT_TRUE(m1.cells[0].bal_known);
  TEST_ASSERT_EQUAL(0, m1.bal_count);
  const ModuleData& m2 = s.m[1];
  TEST_ASSERT_EQUAL(3444, m2.cell_min_mv);
  TEST_ASSERT_EQUAL(14, m2.cell_min_idx);
  TEST_ASSERT_EQUAL(45595, m2.remain_mah);
  TEST_ASSERT_EQUAL(3434, s.m[0].cell_min_mv);
  TEST_ASSERT_EQUAL(3455 - 3434, s.sys_delta_mv);
  TEST_ASSERT_EQUAL(95, s.sys_soc);
}

void test_real_bat_noise_rejected() {
  BatResult b; std::string err;
  TEST_ASSERT_FALSE(parseBat(load("real/bat1_noise.txt"), b, err));
  TEST_ASSERT_TRUE_MESSAGE(err.find("line noise") != std::string::npos, err.c_str());
}

void test_real_info_stat() {
  InfoResult i; StatResult st; std::string err;
  TEST_ASSERT_TRUE_MESSAGE(parseInfo(load("real/info1.txt"), i, err), err.c_str());
  TEST_ASSERT_EQUAL_STRING("K230102C10342136", i.barcode.c_str());
  TEST_ASSERT_EQUAL_STRING("US2000C", i.model.c_str());
  TEST_ASSERT_EQUAL_STRING("B68.5.0.0", i.main_soft.c_str());
  TEST_ASSERT_EQUAL_STRING("V2.8", i.soft.c_str());
  TEST_ASSERT_TRUE_MESSAGE(parseStat(load("real/stat1.txt"), st, err), err.c_str());
  TEST_ASSERT_EQUAL(246, st.cycles);
  TEST_ASSERT_EQUAL(98, st.soh);
  Snapshot s;
  applyInfo(1, i, s);
  applyStat(1, st, s, 5000);
  TEST_ASSERT_EQUAL(246, s.m[0].cycles);
  TEST_ASSERT_EQUAL(98, s.m[0].soh);
  TEST_ASSERT_EQUAL_STRING("K230102C10342136", s.m[0].serial);
  TEST_ASSERT_EQUAL_STRING("B68.5.0.0", s.m[0].fw);
}

void test_real_module2_soh0_is_null() {
  StatResult st; InfoResult i; std::string err;
  TEST_ASSERT_TRUE(parseStat(load("real/stat2_soh0.txt"), st, err));
  TEST_ASSERT_EQUAL(407, st.cycles);
  TEST_ASSERT_TRUE(parseInfo(load("real/info2.txt"), i, err));
  TEST_ASSERT_EQUAL_STRING("B69.13.2.0", i.main_soft.c_str());
  Snapshot s;
  applyStat(2, st, s, 1);
  TEST_ASSERT_EQUAL(407, s.m[1].cycles);
  TEST_ASSERT_EQUAL(0xFF, s.m[1].soh);
}

void test_real_pwr_noise_rejected() {
  PwrResult p; std::string err;
  TEST_ASSERT_FALSE(parsePwr(load("real/pwr_noise.txt"), p, err));
  TEST_ASSERT_TRUE(err.find("line noise") != std::string::npos);
}

void test_real_help_has_no_tables() {
  PwrResult p; std::string err;
  TEST_ASSERT_FALSE(parsePwr(load("real/help.txt"), p, err));
}

void test_real_balancing() {
  Snapshot s; BatResult b; std::string err;
  TEST_ASSERT_TRUE_MESSAGE(parseBat(load("real/bat2_balancing.txt"), b, err), err.c_str());
  int y = 0;
  for (auto& c : b.cells) y += c.bal == 1;
  TEST_ASSERT_EQUAL(12, y);
  TEST_ASSERT_TRUE(applyBat(2, b, s, 1, err));
  TEST_ASSERT_EQUAL(12, s.m[1].bal_count);
  TEST_ASSERT_EQUAL(30, s.m[1].cell_delta_mv);
  TEST_ASSERT_EQUAL(3555, s.m[1].cell_max_mv);
  uint16_t mask = s.m[1].bal_mask;
  for (int i = 0; i < CELLS; i++) TEST_ASSERT_EQUAL_MESSAGE(b.cells[i].bal == 1, (mask >> i) & 1, "bal_mask bit");
}

void test_real_pwr_discharge() {
  PwrResult r; std::string err;
  TEST_ASSERT_TRUE_MESSAGE(parsePwr(load("real/pwr_dischg_24a.txt"), r, err), err.c_str());
  TEST_ASSERT_EQUAL(2, r.present);
  TEST_ASSERT_EQUAL(49418, r.rows[0].mv);
  TEST_ASSERT_EQUAL(-11006, r.rows[0].ma);
  TEST_ASSERT_EQUAL(-12917, r.rows[1].ma);
  TEST_ASSERT_EQUAL(3286, r.rows[0].vlow);
  TEST_ASSERT_EQUAL(3304, r.rows[0].vhigh);
  TEST_ASSERT_EQUAL(99, r.rows[0].soc);
  TEST_ASSERT_EQUAL(11200, r.rows[0].mos);
  TEST_ASSERT_EQUAL_STRING("Dischg", r.rows[0].base.c_str());
  TEST_ASSERT_EQUAL_STRING("Dischg", r.rows[1].base.c_str());
  TEST_ASSERT_EQUAL_STRING("Normal", r.rows[1].cst.c_str());
}

void test_real_bat_discharge_snapshot() {
  Snapshot s;
  PwrResult p; BatResult b1, b2; std::string err;
  TEST_ASSERT_TRUE(parsePwr(load("real/pwr_dischg_24a.txt"), p, err));
  applyPwr(p, s, 1000);
  TEST_ASSERT_TRUE_MESSAGE(parseBat(load("real/bat1_dischg.txt"), b1, err), err.c_str());
  TEST_ASSERT_TRUE_MESSAGE(parseBat(load("real/bat2_dischg.txt"), b2, err), err.c_str());
  TEST_ASSERT_EQUAL(-11006, b1.cells[0].ma);
  TEST_ASSERT_EQUAL(-13032, b2.cells[14].ma);
  TEST_ASSERT_EQUAL_STRING("Dischg", b1.cells[7].base.c_str());
  TEST_ASSERT_TRUE(applyBat(1, b1, s, 2000, err));
  TEST_ASSERT_TRUE(applyBat(2, b2, s, 3000, err));
  const ModuleData& m1 = s.m[0];
  const ModuleData& m2 = s.m[1];
  TEST_ASSERT_EQUAL(BaseState::Dischg, m1.base);
  TEST_ASSERT_EQUAL(BaseState::Dischg, m2.base);
  TEST_ASSERT_EQUAL(-12917, m2.ma);
  TEST_ASSERT_EQUAL(3280, m1.cell_min_mv);
  TEST_ASSERT_EQUAL(10, m1.cell_min_idx);
  TEST_ASSERT_EQUAL(3296, m1.cell_max_mv);
  TEST_ASSERT_EQUAL(8, m1.cell_max_idx);
  TEST_ASSERT_EQUAL(16, m1.cell_delta_mv);
  TEST_ASSERT_EQUAL(48323, m1.remain_mah);
  TEST_ASSERT_EQUAL(0, m1.bal_count);
  TEST_ASSERT_EQUAL(3282, m2.cell_min_mv);
  TEST_ASSERT_EQUAL(3288, m2.cell_max_mv);
  TEST_ASSERT_EQUAL(3, m2.cell_max_idx);
  TEST_ASSERT_EQUAL(6, m2.cell_delta_mv);
  TEST_ASSERT_EQUAL(47406, m2.remain_mah);
  TEST_ASSERT_EQUAL(-11006 - 12917, s.sys_ma);
  TEST_ASSERT_EQUAL((49418 + 49435) / 2, s.sys_mv);
  TEST_ASSERT_EQUAL(99, s.sys_soc);
  TEST_ASSERT_EQUAL(3296 - 3280, s.sys_delta_mv);
  TEST_ASSERT_EQUAL(0, s.sys_bal_count);
  TEST_ASSERT_EQUAL(13000, s.sys_tmin_mc);
  TEST_ASSERT_EQUAL(14400, s.sys_tmax_mc);
  TEST_ASSERT_EQUAL_HEX16(0, moduleAlarm(s, m1, 3000, true));
  TEST_ASSERT_EQUAL_HEX16(0, moduleAlarm(s, m2, 3000, true));
  TEST_ASSERT_EQUAL_HEX16(0, systemAlarm(s, 3000, true));
}


static void loadTwoModules(Snapshot& s, uint32_t t) {
  PwrResult p; BatResult b1, b2; std::string err;
  TEST_ASSERT_TRUE(parsePwr(load("real/pwr_idle_2mod.txt"), p, err));
  applyPwr(p, s, t);
  TEST_ASSERT_TRUE(parseBat(load("real/bat1_idle.txt"), b1, err));
  TEST_ASSERT_TRUE(parseBat(load("real/bat2_idle.txt"), b2, err));
  TEST_ASSERT_TRUE(applyBat(1, b1, s, t, err));
  TEST_ASSERT_TRUE(applyBat(2, b2, s, t, err));
}

void test_alarm_normal_and_offline() {
  Snapshot s;
  loadTwoModules(s, 1000);
  TEST_ASSERT_EQUAL_HEX16(0, systemAlarm(s, 2000, true));
  TEST_ASSERT_EQUAL_HEX16(ALARM_OFFLINE, systemAlarm(s, 2000, false));
  TEST_ASSERT_EQUAL_HEX16(ALARM_STALE_CELLS, systemAlarm(s, 1000 + 91000, true));
}

void test_alarm_delta_needs_3_polls() {
  Snapshot s;
  s.alarm_cfg.delta_warn_mv = 15;
  s.alarm_cfg.delta_crit_mv = 100;
  BatResult b; std::string err;
  TEST_ASSERT_TRUE(parseBat(load("real/bat1_idle.txt"), b, err));
  PwrResult p;
  TEST_ASSERT_TRUE(parsePwr(load("real/pwr_idle_2mod.txt"), p, err));
  applyPwr(p, s, 1);
  for (int i = 1; i <= 2; i++) {
    applyBat(1, b, s, 10 * i, err);
    TEST_ASSERT_FALSE(moduleAlarm(s, s.m[0], 10 * i, true) & ALARM_DELTA_WARN);
  }
  applyBat(1, b, s, 30, err);
  TEST_ASSERT_TRUE(moduleAlarm(s, s.m[0], 30, true) & ALARM_DELTA_WARN);
  TEST_ASSERT_FALSE(moduleAlarm(s, s.m[0], 30, true) & ALARM_DELTA_CRIT);
  s.alarm_cfg.delta_warn_mv = 25;
  applyBat(1, b, s, 40, err);
  TEST_ASSERT_FALSE(moduleAlarm(s, s.m[0], 40, true) & ALARM_DELTA_WARN);
}

void test_alarm_module_missing_after_stable() {
  Snapshot s;
  PwrResult p2, p1; std::string err;
  TEST_ASSERT_TRUE(parsePwr(load("real/pwr_idle_2mod.txt"), p2, err));
  p1 = p2;
  p1.rows[1].absent = true;
  p1.present = 1;
  applyPwr(p2, s, 1);
  applyPwr(p1, s, 1000);
  TEST_ASSERT_FALSE(systemAlarm(s, 1000, true) & ALARM_MODULE_MISSING);
  applyPwr(p2, s, 2000);
  applyPwr(p2, s, 2000 + 600000);
  TEST_ASSERT_EQUAL(2, s.stable_modules);
  applyPwr(p1, s, 700000);
  TEST_ASSERT_TRUE(systemAlarm(s, 700000, true) & ALARM_MODULE_MISSING);
}

void test_alarm_temp_and_cell_state() {
  Snapshot s;
  loadTwoModules(s, 1000);
  s.m[0].cells[3].v_st = Level::High;
  s.m[1].cells[0].temp_mc = 46000;
  computeDerived(s);
  TEST_ASSERT_EQUAL_HEX16(ALARM_CELL_STATE, moduleAlarm(s, s.m[0], 1000, true));
  TEST_ASSERT_EQUAL_HEX16(ALARM_TEMP_WARN, moduleAlarm(s, s.m[1], 1000, true));
}

void test_pwr_extra_module_ignored() {
  std::string txt = load("real/pwr_idle_2mod.txt");
  size_t m1 = txt.find("\n1     51788");
  size_t e1 = txt.find('\n', m1 + 1);
  std::string row9 = txt.substr(m1 + 1, e1 - m1);
  row9[0] = '9';
  size_t p9 = txt.find("\n9     -");
  size_t e9 = txt.find('\n', p9 + 1);
  txt.replace(p9 + 1, e9 - p9, row9);
  PwrResult r; std::string err;
  TEST_ASSERT_TRUE_MESSAGE(parsePwr(txt, r, err), err.c_str());
  TEST_ASSERT_EQUAL(2, r.present);
  TEST_ASSERT_EQUAL(1, r.ignored);
  TEST_ASSERT_EQUAL(MAX_MODULES, r.rows.size());
  Snapshot s;
  applyPwr(r, s, 1);
  TEST_ASSERT_EQUAL(2, s.module_count);
  TEST_ASSERT_EQUAL(51788, s.m[0].mv);
}

void test_stat_bad_soh_keeps_cycles() {
  std::string txt = load("real/stat1.txt");
  size_t p = txt.find("SOH             :       98");
  TEST_ASSERT_TRUE(p != std::string::npos);
  txt.replace(p, 26, "SOH             :      180");
  StatResult st; std::string err;
  TEST_ASSERT_TRUE_MESSAGE(parseStat(txt, st, err), err.c_str());
  TEST_ASSERT_EQUAL(246, st.cycles);
  TEST_ASSERT_EQUAL(NA, st.soh);
  TEST_ASSERT_FALSE(st.warn.empty());
  Snapshot s;
  applyStat(1, st, s, 1);
  TEST_ASSERT_EQUAL(246, s.m[0].cycles);
  TEST_ASSERT_EQUAL(0xFF, s.m[0].soh);
  StatResult st2;
  TEST_ASSERT_FALSE(parseStat("SOH             :      180\nCommand completed successfully\n", st2, err));
}

void test_time_state_key() {
  Snapshot s;
  loadTwoModules(s, 1000);
  TEST_ASSERT_EQUAL_HEX16(0x8000, timeStateKey(s, 2000, true));
  TEST_ASSERT_EQUAL_HEX16(0, timeStateKey(s, 2000, false));
  TEST_ASSERT_EQUAL_HEX16(0x0003, timeStateKey(s, 1000 + 91000, false));
  s.m[1].cells_ts = 50000;
  TEST_ASSERT_EQUAL_HEX16(0x8001, timeStateKey(s, 1000 + 91000, true));
  s.m[1].present = false;
  TEST_ASSERT_EQUAL_HEX16(0x8001, timeStateKey(s, 200000, true));
}

void test_all_raw_captures() {
  namespace fs = std::filesystem;
  static const char* garbled[] = {"000052_bat1.txt"};
  int ok = 0, noise = 0, incomplete = 0, bad = 0, garb = 0;
  std::string report;
  for (auto& e : fs::directory_iterator(std::string(FIXTURES_DIR) + "/raw")) {
    std::string name = e.path().filename().string();
    bool isPwr = name.find("_pwr") != std::string::npos, isBat = name.find("_bat") != std::string::npos;
    if (!isPwr && !isBat) continue;
    std::string txt = load(("raw/" + name).c_str());
    std::string err;
    bool r;
    if (isPwr) { PwrResult p; r = parsePwr(txt, p, err); if (r && p.present != 2) { r = false; err = "present != 2"; } }
    else { BatResult b; r = parseBat(txt, b, err); }
    bool isGarbled = false;
    for (const char* g : garbled) if (name == g) isGarbled = true;
    if (isGarbled) { if (r) { bad++; report += name + ": garbled but accepted\n"; } else garb++; continue; }
    if (r) ok++;
    else if (err.find("line noise") != std::string::npos) noise++;
    else if (!completedOk(txt)) incomplete++;
    else { bad++; report += name + ": " + err + "\n"; }
  }
  printf("raw captures: ok=%d noise=%d garbled=%d incomplete=%d bad=%d\n", ok, noise, garb, incomplete, bad);
  TEST_ASSERT_TRUE(ok > 0);
  TEST_ASSERT_EQUAL_MESSAGE(0, bad, report.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_header_normalize);
  RUN_TEST(test_data_normalize);
  RUN_TEST(test_parse_milli);
  RUN_TEST(test_pwr);
  RUN_TEST(test_bat_and_derived);
  RUN_TEST(test_bat_without_soc_bal);
  RUN_TEST(test_bat_truncated_rejected);
  RUN_TEST(test_garbage_rejected);
  RUN_TEST(test_real_pwr_16_rows);
  RUN_TEST(test_real_bat_two_modules);
  RUN_TEST(test_real_bat_noise_rejected);
  RUN_TEST(test_real_info_stat);
  RUN_TEST(test_real_help_has_no_tables);
  RUN_TEST(test_real_module2_soh0_is_null);
  RUN_TEST(test_real_pwr_noise_rejected);
  RUN_TEST(test_all_raw_captures);
  RUN_TEST(test_real_balancing);
  RUN_TEST(test_real_pwr_discharge);
  RUN_TEST(test_real_bat_discharge_snapshot);
  RUN_TEST(test_alarm_normal_and_offline);
  RUN_TEST(test_alarm_delta_needs_3_polls);
  RUN_TEST(test_alarm_module_missing_after_stable);
  RUN_TEST(test_alarm_temp_and_cell_state);
  RUN_TEST(test_pwr_extra_module_ignored);
  RUN_TEST(test_stat_bad_soh_keeps_cycles);
  RUN_TEST(test_time_state_key);
  return UNITY_END();
}
