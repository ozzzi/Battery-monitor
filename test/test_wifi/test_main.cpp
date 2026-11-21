#include <unity.h>
#include "net/WifiFsm.h"

using namespace wififsm;

void setUp() {}
void tearDown() {}

void test_backoff() {
  TEST_ASSERT_EQUAL_UINT32(5000, backoffMs(0));
  TEST_ASSERT_EQUAL_UINT32(10000, backoffMs(1));
  TEST_ASSERT_EQUAL_UINT32(30000, backoffMs(2));
  TEST_ASSERT_EQUAL_UINT32(60000, backoffMs(3));
  TEST_ASSERT_EQUAL_UINT32(60000, backoffMs(200));
}

void test_no_creds_ap_setup() {
  Fsm f;
  Actions a = f.start(false, 0);
  TEST_ASSERT_TRUE(a.ap_on);
  TEST_ASSERT_FALSE(a.sta_begin);
  TEST_ASSERT_EQUAL(Mode::ApSetup, f.mode());
  for (uint32_t t = 0; t < 3600000; t += 1000) {
    a = f.step(false, false, t);
    TEST_ASSERT_FALSE(a.sta_begin);
    TEST_ASSERT_FALSE(a.ap_on);
    TEST_ASSERT_FALSE(a.ap_off);
  }
  TEST_ASSERT_EQUAL(Mode::ApSetup, f.mode());
}

void test_connecting_backoff_schedule() {
  Fsm f;
  TEST_ASSERT_TRUE(f.start(true, 0).sta_begin);
  TEST_ASSERT_EQUAL(Mode::StaConnecting, f.mode());
  uint32_t begins[8];
  int n = 0;
  for (uint32_t t = 100; t <= 170000 && n < 8; t += 100) {
    if (f.step(false, false, t).sta_begin) begins[n++] = t;
  }
  TEST_ASSERT_EQUAL(5, n);
  TEST_ASSERT_EQUAL_UINT32(5000, begins[0]);
  TEST_ASSERT_EQUAL_UINT32(15000, begins[1]);
  TEST_ASSERT_EQUAL_UINT32(45000, begins[2]);
  TEST_ASSERT_EQUAL_UINT32(105000, begins[3]);
  TEST_ASSERT_EQUAL_UINT32(165000, begins[4]);
}

void test_rescue_after_10_min_and_ap_off_2_min_after_online() {
  Fsm f;
  f.start(true, 0);
  bool apOn = false;
  uint32_t onAt = 0;
  for (uint32_t t = 1000; t <= 700000; t += 1000) {
    Actions a = f.step(false, false, t);
    if (a.ap_on) { apOn = true; onAt = t; break; }
  }
  TEST_ASSERT_TRUE(apOn);
  TEST_ASSERT_EQUAL_UINT32(600000, onAt);
  TEST_ASSERT_EQUAL(Mode::ApRescue, f.mode());
  int begins = 0;
  for (uint32_t t = onAt + 1000; t <= onAt + 180000; t += 1000)
    if (f.step(false, false, t).sta_begin) begins++;
  TEST_ASSERT_TRUE(begins >= 3);
  uint32_t up = onAt + 200000;
  TEST_ASSERT_FALSE(f.step(true, false, up).ap_off);
  TEST_ASSERT_EQUAL(Mode::StaOnline, f.mode());
  TEST_ASSERT_FALSE(f.step(true, false, up + 119000).ap_off);
  TEST_ASSERT_TRUE(f.step(true, false, up + 120000).ap_off);
  TEST_ASSERT_FALSE(f.apOn());
}

void test_online_loss_retries_immediately() {
  Fsm f;
  f.start(true, 0);
  f.step(true, false, 2000);
  TEST_ASSERT_EQUAL(Mode::StaOnline, f.mode());
  TEST_ASSERT_TRUE(f.step(false, false, 50000).sta_begin);
  TEST_ASSERT_EQUAL(Mode::StaConnecting, f.mode());
  TEST_ASSERT_FALSE(f.step(false, false, 54000).sta_begin);
  TEST_ASSERT_TRUE(f.step(false, false, 55000).sta_begin);
  TEST_ASSERT_FALSE(f.step(false, false, 50000 + 599000).ap_on);
  TEST_ASSERT_TRUE(f.step(false, false, 50000 + 600000).ap_on);
}

void test_trial_suppresses_begin_and_creds_saved_10s() {
  Fsm f;
  f.start(false, 0);
  for (uint32_t t = 0; t < 20000; t += 500) TEST_ASSERT_FALSE(f.step(false, true, t).sta_begin);
  f.credsSaved(20000, 10000);
  TEST_ASSERT_FALSE(f.step(true, false, 20100).ap_off);
  TEST_ASSERT_EQUAL(Mode::StaOnline, f.mode());
  TEST_ASSERT_FALSE(f.step(true, false, 29900).ap_off);
  TEST_ASSERT_TRUE(f.step(true, false, 30000).ap_off);
}

void test_trial_in_rescue_overrides_2min() {
  Fsm f;
  f.start(true, 0);
  for (uint32_t t = 1000; t <= 600000; t += 1000) f.step(false, false, t);
  TEST_ASSERT_EQUAL(Mode::ApRescue, f.mode());
  TEST_ASSERT_FALSE(f.step(false, true, 700000).sta_begin);
  f.step(true, false, 701000);
  f.credsSaved(701000, 10000);
  TEST_ASSERT_TRUE(f.step(true, false, 711000).ap_off);
}

void test_wraparound() {
  Fsm f;
  const uint32_t t0 = 0xFFFFF000u;
  f.start(true, t0);
  TEST_ASSERT_FALSE(f.step(false, false, t0 + 1000).sta_begin);
  TEST_ASSERT_TRUE(f.step(false, false, t0 + 5000).sta_begin);
  TEST_ASSERT_FALSE(f.step(false, false, t0 + 599000).ap_on);
  TEST_ASSERT_TRUE(f.step(false, false, t0 + 600000).ap_on);
}

void test_classify_and_hostname() {
  TEST_ASSERT_EQUAL(Trial::NoSsid, classifyDisconnect(201));
  TEST_ASSERT_EQUAL(Trial::NoSsid, classifyDisconnect(210));
  TEST_ASSERT_EQUAL(Trial::BadPass, classifyDisconnect(15));
  TEST_ASSERT_EQUAL(Trial::BadPass, classifyDisconnect(202));
  TEST_ASSERT_EQUAL(Trial::None, classifyDisconnect(8));
  TEST_ASSERT_EQUAL(Trial::Failed, classifyDisconnect(200));
  TEST_ASSERT_EQUAL_STRING("AP_RESCUE", modeName(Mode::ApRescue));
  TEST_ASSERT_EQUAL_STRING("bad_pass", trialName(Trial::BadPass));
  TEST_ASSERT_TRUE(validHostname("battery"));
  TEST_ASSERT_TRUE(validHostname("pylon-garage2"));
  TEST_ASSERT_FALSE(validHostname(""));
  TEST_ASSERT_FALSE(validHostname("-bat"));
  TEST_ASSERT_FALSE(validHostname("bat-"));
  TEST_ASSERT_FALSE(validHostname("bat.local"));
  TEST_ASSERT_FALSE(validHostname("батарея"));
  TEST_ASSERT_TRUE(validHostname("a234567890123456789012345678901"));    // 31
  TEST_ASSERT_FALSE(validHostname("a2345678901234567890123456789012"));  // 32
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_backoff);
  RUN_TEST(test_no_creds_ap_setup);
  RUN_TEST(test_connecting_backoff_schedule);
  RUN_TEST(test_rescue_after_10_min_and_ap_off_2_min_after_online);
  RUN_TEST(test_online_loss_retries_immediately);
  RUN_TEST(test_trial_suppresses_begin_and_creds_saved_10s);
  RUN_TEST(test_trial_in_rescue_overrides_2min);
  RUN_TEST(test_wraparound);
  RUN_TEST(test_classify_and_hostname);
  return UNITY_END();
}
