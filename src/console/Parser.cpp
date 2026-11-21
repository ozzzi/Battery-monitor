#include "console/Parser.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace parser {

static std::string fmt(const char* a, long x, const char* b = "", long y = 0, const char* c = "") {
  return std::string(a) + std::to_string(x) + b + (b[0] ? std::to_string(y) : "") + c;
}

int Table::col(const char* name) const {
  for (size_t i = 0; i < header.size(); i++)
    if (!strcasecmp(header[i].c_str(), name)) return (int)i;
  return -1;
}

Tokens splitWs(std::string_view line) {
  Tokens t;
  size_t i = 0, n = line.size();
  while (i < n) {
    while (i < n && isspace((unsigned char)line[i])) i++;
    size_t b = i;
    while (i < n && !isspace((unsigned char)line[i])) i++;
    if (i > b) t.emplace_back(line.substr(b, i - b));
  }
  return t;
}

static std::string stripDots(const std::string& s) {
  std::string r;
  for (char c : s) if (c != '.') r += c;
  return r;
}

Tokens normalizeHeader(const Tokens& raw) {
  // "Base State" -> BaseSt, "Volt. State" -> VoltSt, "Base.St" -> BaseSt, "B.V.St" -> BVSt
  Tokens out;
  for (size_t i = 0; i < raw.size(); i++) {
    if (i + 1 < raw.size() && !strcasecmp(raw[i + 1].c_str(), "State")) {
      out.push_back(stripDots(raw[i]) + "St");
      i++;
    } else {
      out.push_back(stripDots(raw[i]));
    }
  }
  return out;
}

static bool isDate(const std::string& s) {   // YYYY-MM-DD
  if (s.size() != 10) return false;
  for (int i = 0; i < 10; i++) {
    if (i == 4 || i == 7) { if (s[i] != '-') return false; }
    else if (!isdigit((unsigned char)s[i])) return false;
  }
  return true;
}
static bool isTime(const std::string& s) {   // HH:MM:SS
  if (s.size() != 8) return false;
  for (int i = 0; i < 8; i++) {
    if (i == 2 || i == 5) { if (s[i] != ':') return false; }
    else if (!isdigit((unsigned char)s[i])) return false;
  }
  return true;
}

Tokens normalizeData(const Tokens& raw) {
  Tokens out;
  for (size_t i = 0; i < raw.size(); i++) {
    const std::string& t = raw[i];
    if (!strcasecmp(t.c_str(), "mAH") || !strcmp(t.c_str(), "mV") || !strcmp(t.c_str(), "mA")) continue;
    if (isDate(t) && i + 1 < raw.size() && isTime(raw[i + 1])) {
      out.push_back(t + "_" + raw[i + 1]);
      i++;
      continue;
    }
    if (t.size() > 1 && t.back() == '%') { out.push_back(t.substr(0, t.size() - 1)); continue; }
    out.push_back(t);
  }
  return out;
}

bool findTable(std::string_view text, const char* headerWord, Table& out, std::string& err) {
  out = Table{};
  bool inTable = false;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t e = text.find('\n', pos);
    if (e == std::string_view::npos) e = text.size();
    std::string_view line = text.substr(pos, e - pos);
    pos = e + 1;
    Tokens raw = splitWs(line);
    if (raw.empty()) continue;
    if (!inTable) {
      if (!strcasecmp(raw[0].c_str(), headerWord)) {
        out.header = normalizeHeader(raw);
        inTable = true;
      }
      continue;
    }
    if (isdigit((unsigned char)raw[0][0])) out.rows.push_back(normalizeData(raw));
    else if (!out.rows.empty()) break;
  }
  if (!inTable) { err = std::string("header '") + headerWord + "' not found"; return false; }
  return true;
}

bool completedOk(std::string_view text) {
  return text.find("Command completed successfully") != std::string_view::npos;
}

size_t countNoise(std::string_view text) {
  size_t n = 0;
  for (unsigned char c : text)
    if (c >= 0x7F || (c < 0x20 && c != '\r' && c != '\n' && c != '\t')) n++;
  return n;
}

static bool rejectNoise(std::string_view text, const char* what, std::string& err) {
  size_t n = countNoise(text);
  if (!n) return false;
  err = std::string(what) + ": line noise (" + std::to_string(n) + " bad bytes)";
  return true;
}

bool parseInt(const std::string& tok, int32_t& out) {
  if (tok.empty() || tok == "-") return false;
  char* end = nullptr;
  long v = strtol(tok.c_str(), &end, 10);
  if (end == tok.c_str() || *end) return false;
  out = (int32_t)v;
  return true;
}

bool parseMilli(const std::string& tok, int32_t& out) {
  if (tok.empty() || tok == "-") return false;
  if (tok.find('.') == std::string::npos) return parseInt(tok, out);
  char* end = nullptr;
  double v = strtod(tok.c_str(), &end);
  if (end == tok.c_str() || *end) return false;
  out = (int32_t)(v * 1000.0 + (v >= 0 ? 0.5 : -0.5));
  return true;
}

static const std::string& at(const Tokens& r, int i) {
  static const std::string empty;
  return (i >= 0 && (size_t)i < r.size()) ? r[i] : empty;
}

static bool hasToken(const Tokens& r, const char* s) {
  for (auto& t : r) if (!strcasecmp(t.c_str(), s)) return true;
  return false;
}

bool parsePwr(std::string_view text, PwrResult& out, std::string& err) {
  out = PwrResult{};
  if (rejectNoise(text, "pwr", err)) return false;
  Table t;
  if (!findTable(text, "Power", t, err)) return false;
  const int cN = t.col("Power"), cV = t.col("Volt"), cI = t.col("Curr"), cT = t.col("Tempr"),
            cTl = t.col("Tlow"), cTh = t.col("Thigh"), cVl = t.col("Vlow"), cVh = t.col("Vhigh"),
            cB = t.col("BaseSt"), cVs = t.col("VoltSt"), cCs = t.col("CurrSt"), cTs = t.col("TempSt"),
            cSoc = t.col("Coulomb"), cMos = t.col("MosTempr");
  if (cV < 0 || cI < 0 || cB < 0) { err = "pwr: missing Volt/Curr/BaseSt column"; return false; }
  for (auto& r : t.rows) {
    PwrRow p;
    int32_t n = 0;
    if (!parseInt(at(r, cN), n) || n < 1 || n > 16) { err = "pwr: bad module number '" + at(r, cN) + "'"; return false; }
    p.n = n;
    const bool absent = hasToken(r, "Absent");
    if (n > MAX_MODULES) {
      if (!absent) out.ignored++;
      continue;
    }
    if (absent) { p.absent = true; out.rows.push_back(p); continue; }
    if (r.size() != t.header.size()) {
      err = fmt("pwr: module ", n, ": tokens ", (long)r.size(), "") + " != header " + std::to_string(t.header.size());
      return false;
    }
    parseInt(at(r, cV), p.mv);
    parseInt(at(r, cI), p.ma);
    parseMilli(at(r, cT), p.t);
    parseMilli(at(r, cTl), p.tlow);
    parseMilli(at(r, cTh), p.thigh);
    parseInt(at(r, cVl), p.vlow);
    parseInt(at(r, cVh), p.vhigh);
    parseInt(at(r, cSoc), p.soc);
    parseMilli(at(r, cMos), p.mos);
    p.base = at(r, cB); p.vst = at(r, cVs); p.cst = at(r, cCs); p.tst = at(r, cTs);
    if (p.mv == NA || p.mv < 40000 || p.mv > 58000) { err = fmt("pwr: module ", n, " voltage out of range"); return false; }
    if (p.t != NA && (p.t < -40000 || p.t > 100000)) { err = fmt("pwr: module ", n, " temperature out of range"); return false; }
    out.rows.push_back(p);
    out.present++;
  }
  if (out.rows.empty()) { err = "pwr: no data rows"; return false; }
  return true;
}

bool parseBat(std::string_view text, BatResult& out, std::string& err) {
  out = BatResult{};
  if (rejectNoise(text, "bat", err)) return false;
  Table t;
  if (!findTable(text, "Battery", t, err)) return false;
  const int cN = t.col("Battery"), cV = t.col("Volt"), cI = t.col("Curr"), cT = t.col("Tempr"),
            cB = t.col("BaseSt"), cVs = t.col("VoltSt"), cCs = t.col("CurrSt"), cTs = t.col("TempSt"),
            cSoc = t.col("SOC"), cC = t.col("Coulomb"), cBal = t.col("BAL");
  if (cV < 0) { err = "bat: missing Volt column"; return false; }
  for (auto& r : t.rows) {
    if (r.size() != t.header.size()) {
      err = "bat: row '" + at(r, 0) + "': tokens " + std::to_string(r.size()) + " != header " + std::to_string(t.header.size());
      return false;
    }
    BatCell c;
    int32_t idx = -1;
    parseInt(at(r, cN), idx);
    c.idx = idx;
    parseInt(at(r, cV), c.mv);
    parseInt(at(r, cI), c.ma);
    parseMilli(at(r, cT), c.t);
    parseInt(at(r, cSoc), c.soc);
    parseInt(at(r, cC), c.coulomb);
    c.base = at(r, cB); c.vst = at(r, cVs); c.cst = at(r, cCs); c.tst = at(r, cTs);
    if (cBal >= 0) {
      const std::string& b = at(r, cBal);
      c.bal = (b == "Y" || b == "y" || b == "1") ? 1 : 0;
    }
    out.cells.push_back(c);
  }
  if (out.cells.size() != CELLS) { err = "bat: expected 15 cells, got " + std::to_string(out.cells.size()); return false; }
  for (int i = 0; i < CELLS; i++) {
    const BatCell& c = out.cells[i];
    if (c.idx != i) { err = "bat: cell index " + std::to_string(c.idx) + " at position " + std::to_string(i); return false; }
    if (c.mv == NA || c.mv < 2000 || c.mv > 3900) { err = "bat: cell " + std::to_string(i) + " voltage out of range"; return false; }
    if (c.t != NA && (c.t < -40000 || c.t > 100000)) { err = "bat: cell " + std::to_string(i) + " temperature out of range"; return false; }
  }
  return true;
}

static void copyStr(char* dst, size_t n, const std::string& s) {
  strncpy(dst, s.c_str(), n - 1);
  dst[n - 1] = 0;
}

void applyPwr(const PwrResult& r, Snapshot& s, uint32_t now) {
  uint8_t count = 0;
  for (auto& p : r.rows) {
    ModuleData& m = s.m[p.n - 1];
    if (p.absent) {
      m.present = false;
      m.base = BaseState::Absent;
      continue;
    }
    count++;
    m.present = true;
    m.mv = p.mv; m.ma = p.ma;
    m.temp_mc = p.t; m.tlow_mc = p.tlow; m.thigh_mc = p.thigh; m.mos_mc = p.mos;
    m.vlow_mv = p.vlow == NA ? 0 : (uint16_t)p.vlow;
    m.vhigh_mv = p.vhigh == NA ? 0 : (uint16_t)p.vhigh;
    m.soc = (p.soc == NA || p.soc < 0 || p.soc > 100) ? 0xFF : (uint8_t)p.soc;
    m.base = baseStateFrom(p.base.c_str());
    copyStr(m.base_raw, sizeof(m.base_raw), p.base);
    m.v_st = levelFrom(p.vst.c_str());
    m.c_st = levelFrom(p.cst.c_str());
    m.t_st = levelFrom(p.tst.c_str());
    m.pwr_ts = now;
  }
  s.module_count = count;
  updateModuleStability(s, count, now);
  s.has_data = true;
  s.last_ok_ts = now;
  s.seq++;
  computeDerived(s);
}

bool applyBat(int module, const BatResult& r, Snapshot& s, uint32_t now, std::string& err) {
  if (module < 1 || module > MAX_MODULES) { err = "bat: bad module"; return false; }
  if (r.cells.size() != CELLS) { err = "bat: not 15 cells"; return false; }
  ModuleData& m = s.m[module - 1];
  for (int i = 0; i < CELLS; i++) {
    const BatCell& c = r.cells[i];
    CellData& d = m.cells[i];
    d.mv = (uint16_t)c.mv;
    d.temp_mc = c.t;
    d.v_st = levelFrom(c.vst.c_str());
    d.t_st = levelFrom(c.tst.c_str());
    d.bal_known = c.bal >= 0;
    d.bal = c.bal == 1;
  }
  uint32_t mn = 0xFFFFFFFF;
  for (int i = 0; i < CELLS; i++) {
    const BatCell& c = r.cells[i];
    m.cells[i].soc = (c.soc == NA || c.soc < 0 || c.soc > 100) ? 0xFF : (uint8_t)c.soc;
    m.cells[i].mah = c.coulomb == NA || c.coulomb < 0 ? 0xFFFFFFFF : (uint32_t)c.coulomb;
    if (m.cells[i].mah < mn) mn = m.cells[i].mah;
  }
  m.remain_mah = mn;
  m.cells_ts = now;
  s.last_ok_ts = now;
  s.seq++;
  computeDerived(s);
  updateDeltaStreak(s.alarm_cfg, m);
  return true;
}

// ---------- info / stat ----------

static std::string trim(std::string_view v) {
  size_t b = v.find_first_not_of(" \t\r");
  if (b == std::string_view::npos) return "";
  size_t e = v.find_last_not_of(" \t\r");
  return std::string(v.substr(b, e - b + 1));
}

template <typename F>
static void forEachKv(std::string_view text, F fn) {
  size_t pos = 0;
  while (pos < text.size()) {
    size_t e = text.find('\n', pos);
    if (e == std::string_view::npos) e = text.size();
    std::string_view line = text.substr(pos, e - pos);
    pos = e + 1;
    size_t c = line.find(':');
    if (c == std::string_view::npos) continue;
    fn(trim(line.substr(0, c)), trim(line.substr(c + 1)));
  }
}

bool parseInfo(std::string_view text, InfoResult& out, std::string& err) {
  out = InfoResult{};
  if (rejectNoise(text, "info", err)) return false;
  forEachKv(text, [&](const std::string& k, const std::string& v) {
    if (k == "Barcode") out.barcode = v;
    else if (k == "Device name") out.model = v;
    else if (k == "Soft  version" || k == "Soft version") out.soft = v;
    else if (k == "Main Soft version") out.main_soft = v;
  });
  if (out.barcode.empty() && out.model.empty()) { err = "info: no Barcode/Device name"; return false; }
  return true;
}

bool parseStat(std::string_view text, StatResult& out, std::string& err) {
  out = StatResult{};
  if (rejectNoise(text, "stat", err)) return false;
  forEachKv(text, [&](const std::string& k, const std::string& v) {
    if (k == "CYCLE Times") parseInt(v, out.cycles);
    else if (k == "SOH") parseInt(v, out.soh);
  });
  if (out.cycles == NA && out.soh == NA) { err = "stat: no CYCLE Times/SOH"; return false; }
  if (out.soh != NA && (out.soh < 0 || out.soh > 100)) {
    out.warn = "stat: SOH " + std::to_string(out.soh) + " out of range, ignored";
    out.soh = NA;
    if (out.cycles == NA) { err = "stat: SOH out of range"; return false; }
  }
  return true;
}

void applyInfo(int module, const InfoResult& r, Snapshot& s) {
  if (module < 1 || module > MAX_MODULES) return;
  ModuleData& m = s.m[module - 1];
  copyStr(m.serial, sizeof(m.serial), r.barcode);
  copyStr(m.fw, sizeof(m.fw), r.main_soft.empty() ? r.soft : r.main_soft);
  s.seq++;
}

void applyStat(int module, const StatResult& r, Snapshot& s, uint32_t now) {
  if (module < 1 || module > MAX_MODULES) return;
  ModuleData& m = s.m[module - 1];
  m.cycles = (r.cycles == NA || r.cycles < 0 || r.cycles > 65534) ? 0xFFFF : (uint16_t)r.cycles;
  m.soh = (r.soh == NA || r.soh == 0) ? 0xFF : (uint8_t)r.soh;
  m.stat_ts = now;
  s.seq++;
}

}  // namespace parser
