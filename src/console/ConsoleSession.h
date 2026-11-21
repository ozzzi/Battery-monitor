#pragma once
#include <Arduino.h>
#include <string>
#include "config.h"
#include "console/ConsoleClient.h"

enum class SessState : uint8_t { Boot, Probe, Wake, Login, Ready, Offline };
const char* sessStateName(SessState s);

enum class JobType : uint8_t { Cmd, Sniff, Wake, Login, Uart, Reprobe, Config };

struct Job {
  JobType     type = JobType::Cmd;
  std::string cmd;              // Cmd
  uint32_t    ms = 3000;
  bool        save = false;
  uint8_t     tx = 0, rx = 0;   // Uart
  uint32_t    baud = 0;         // Uart
  CmdReply    reply;
  std::string out;
  bool        ok = false;
};

struct SessionInfo {
  SessState state;
  bool      paused;
  uint32_t  last_ok_ms;
  uint8_t   fails_in_row;
  uint16_t  offline_rounds;
  uint8_t   module_count;
  uint8_t   tx, rx;
  uint32_t  baud;
  char      last_cmd[16];
  char      last_result[12];
  char      last_parse_err[96];
};

namespace ConsoleSession {
void begin();
bool run(Job& job, uint32_t wait_ms = CONSOLE_RUN_WAIT_MS);
void setPaused(bool p);
SessionInfo info();
bool online();
}
