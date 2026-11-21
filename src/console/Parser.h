#pragma once
//

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "model/Snapshot.h"

namespace parser {

using Tokens = std::vector<std::string>;

struct Table {
  Tokens header;
  std::vector<Tokens> rows;
  int col(const char* name) const;
};

Tokens splitWs(std::string_view line);
Tokens normalizeHeader(const Tokens& raw);
Tokens normalizeData(const Tokens& raw);

bool findTable(std::string_view text, const char* headerWord, Table& out, std::string& err);

bool completedOk(std::string_view text);   // "Command completed successfully"
size_t countNoise(std::string_view text);

bool parseMilli(const std::string& tok, int32_t& out);
bool parseInt(const std::string& tok, int32_t& out);

struct PwrRow {
  int      n = 0;
  bool     absent = false;
  int32_t  mv = NA, ma = NA, t = NA, tlow = NA, thigh = NA, vlow = NA, vhigh = NA, soc = NA, mos = NA;
  std::string base, vst, cst, tst;
};
struct PwrResult {
  std::vector<PwrRow> rows;
  int present = 0;
  int ignored = 0;
};
bool parsePwr(std::string_view text, PwrResult& out, std::string& err);

struct BatCell {
  int      idx = -1;
  int32_t  mv = NA, ma = NA, t = NA, soc = NA, coulomb = NA;
  std::string base, vst, cst, tst;
  int      bal = -1;
};
struct BatResult {
  std::vector<BatCell> cells;
};
bool parseBat(std::string_view text, BatResult& out, std::string& err);

struct InfoResult {
  std::string barcode, model, soft, main_soft;
};
bool parseInfo(std::string_view text, InfoResult& out, std::string& err);
struct StatResult {
  int32_t cycles = NA, soh = NA;
  std::string warn;
};
bool parseStat(std::string_view text, StatResult& out, std::string& err);

void applyPwr(const PwrResult& r, Snapshot& s, uint32_t now);
bool applyBat(int module, const BatResult& r, Snapshot& s, uint32_t now, std::string& err);
void applyInfo(int module, const InfoResult& r, Snapshot& s);
void applyStat(int module, const StatResult& r, Snapshot& s, uint32_t now);

}  // namespace parser
