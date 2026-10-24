// shiny_pad: executes a pre-uploaded timeline of pad states at exact
// microsecond offsets. Wire format: "Serial protocol" in the repo README.

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "timed_pad.h"
#include "wifi_link.h"

const char kFirmwareVersion[] = "0.2.0";
const unsigned kProtocolVersion = 2;
const uint32_t kBaud = 115200;
const size_t kMaxEvents = 64;
// F3, 2026-10-24, Switch 2: it accepts one report per ~8 ms; faster streams back up and drift.
const uint32_t kMinSpacingUs = 10000;
// Slower than 8 ms so idle keep-alives never back up the console's queue.
const uint32_t kKeepAliveMs = 100;
// Keep-alives, network I/O and DONE output only run when the next event is further off than this.
const uint32_t kSlackGuardUs = 20000;
const size_t kLineMax = 128;
const size_t kMaxTokens = 10;
const uint8_t kStickCentre = 0x80;
const uint16_t kButtonMask = 0x3FFF;  // 14 buttons in the descriptor
const uint8_t kDpadCentred = 15;

struct Event {
  uint32_t at_us;
  uint16_t seq, buttons;
  uint8_t dpad, lx, ly, rx, ry;
};

struct Result {
  uint32_t before_us, send_us;
  int32_t err_us;
  bool ok;
};

struct LineReader {
  char buf[kLineMax + 1];
  size_t len;
  bool overflow;
};

enum class State : uint8_t { Idle, Loading, Armed, Running };

static TimedPad pad;
static Event events[kMaxEvents];
static Result results[kMaxEvents];
static size_t eventCount = 0;
static size_t nextEvent = 0;
static size_t resultsReported = 0;
static State state = State::Idle;

static uint32_t armedAtMs = 0;
static uint32_t armTimeoutMs = 0;
static uint32_t anchorUs = 0;
static uint32_t maxAbsErrUs = 0;
static size_t failedSends = 0;
static bool anchorPending = false;
static uint32_t lastKeepAliveMs = 0;

static Stream *cmdLink = &Serial;  // where the current command's reply goes
static Stream *runLink = &Serial;  // the link that armed the timeline; gets DONE/END
static LineReader serialReader;
static LineReader tcpReader;

const char *stateName() {
  switch (state) {
    case State::Idle: return "IDLE";
    case State::Loading: return "LOADING";
    case State::Armed: return "ARMED";
    case State::Running: return "RUNNING";
  }
  return "?";
}

size_t freeSlots() {
  return kMaxEvents - eventCount;
}

void replyErr(const char *code, const char *detail) {
  cmdLink->printf("ERR %s %s\n", code, detail);
}

void replyErrSeq(const char *code, uint32_t seq) {
  cmdLink->printf("ERR %s %lu\n", code, (unsigned long)seq);
}

// Strict unsigned parse: whole token, no sign, no overflow, <= max.
bool parseU32(const char *s, int base, uint32_t max, uint32_t *out) {
  if (*s == '\0' || *s == '-' || *s == '+') return false;
  errno = 0;
  char *end;
  unsigned long v = strtoul(s, &end, base);
  if (*end != '\0' || errno == ERANGE || v > max) return false;
  *out = v;
  return true;
}

// True when nothing is due within kSlackGuardUs, so a blocking call can't delay an event.
bool hasSlack() {
  if (state != State::Running) return true;
  uint32_t elapsed = micros() - anchorUs;
  uint32_t due = events[nextEvent].at_us;
  return due > elapsed && due - elapsed > kSlackGuardUs;
}

// ANCHOR and DONE lines are held until there's slack: a TCP write can block for a millisecond.
void reportResults(bool all) {
  if (anchorPending && (all || hasSlack())) {
    runLink->printf("ANCHOR %lu\n", (unsigned long)anchorUs);
    anchorPending = false;
  }
  while (resultsReported < nextEvent && (all || hasSlack())) {
    const Event &ev = events[resultsReported];
    const Result &r = results[resultsReported];
    runLink->printf("DONE %u %lu %ld %lu\n", (unsigned)ev.seq, (unsigned long)r.before_us,
                    (long)r.err_us, (unsigned long)r.send_us);
    if (!r.ok) runLink->printf("ERR SEND %u\n", (unsigned)ev.seq);
    resultsReported++;
  }
}

void resetTimeline() {
  eventCount = 0;
  nextEvent = 0;
  resultsReported = 0;
  state = State::Idle;
}

void abortTimeline() {
  if (state == State::Running) reportResults(true);
  resetTimeline();
  pad.sendNeutral();
  cmdLink->print("ABORTED\n");
}

void cmdPing() {
  cmdLink->printf("PONG %s %u %u\n", kFirmwareVersion, kProtocolVersion, (unsigned)freeSlots());
}

void cmdEv(char **tok, size_t n) {
  if (state == State::Armed || state == State::Running) {
    replyErr("BUSY", stateName());
    return;
  }
  if (n != 5 && n != 9) {
    replyErr("BAD_ARGS", "EV");
    return;
  }

  uint32_t seq, at, buttons, dpad;
  uint32_t sticks[4] = {kStickCentre, kStickCentre, kStickCentre, kStickCentre};
  static const char *const kStickNames[4] = {"lx", "ly", "rx", "ry"};

  if (!parseU32(tok[1], 10, 0xFFFF, &seq)) { replyErr("BAD_VALUE", "seq"); return; }
  if (!parseU32(tok[2], 10, UINT32_MAX, &at)) { replyErr("BAD_VALUE", "at_us"); return; }
  if (!parseU32(tok[3], 16, kButtonMask, &buttons)) { replyErr("BAD_VALUE", "buttons"); return; }
  if (!parseU32(tok[4], 10, kDpadCentred, &dpad) || (dpad > 7 && dpad != kDpadCentred)) {
    replyErr("BAD_VALUE", "dpad");
    return;
  }
  if (n == 9) {
    for (size_t i = 0; i < 4; i++) {
      if (!parseU32(tok[5 + i], 10, 255, &sticks[i])) { replyErr("BAD_VALUE", kStickNames[i]); return; }
    }
  }

  if (eventCount == kMaxEvents) { replyErrSeq("QUEUE_FULL", seq); return; }
  if (eventCount > 0) {
    const Event &prev = events[eventCount - 1];
    if (seq <= prev.seq) { replyErrSeq("SEQ_ORDER", seq); return; }
    if (at <= prev.at_us) { replyErrSeq("TIME_ORDER", seq); return; }
    if (at - prev.at_us < kMinSpacingUs) { replyErrSeq("SPACING", seq); return; }
  }

  events[eventCount++] = {at, (uint16_t)seq, (uint16_t)buttons, (uint8_t)dpad,
                          (uint8_t)sticks[0], (uint8_t)sticks[1],
                          (uint8_t)sticks[2], (uint8_t)sticks[3]};
  state = State::Loading;
  cmdLink->printf("OK %lu %u\n", (unsigned long)seq, (unsigned)freeSlots());
}

void cmdArm(char **tok, size_t n) {
  if (state == State::Idle) { replyErr("EMPTY", "-"); return; }
  if (state != State::Loading) { replyErr("BUSY", stateName()); return; }
  if (n != 2) { replyErr("BAD_ARGS", "ARM"); return; }
  uint32_t timeout;
  if (!parseU32(tok[1], 10, UINT32_MAX, &timeout) || timeout == 0) {
    replyErr("BAD_VALUE", "timeout_ms");
    return;
  }
  armedAtMs = millis();
  armTimeoutMs = timeout;
  runLink = cmdLink;
  state = State::Armed;
  cmdLink->printf("ARMED %u\n", (unsigned)eventCount);
}

void cmdFire() {
  if (state != State::Armed) { replyErr("NOT_ARMED", stateName()); return; }
  anchorUs = micros();
  nextEvent = 0;
  resultsReported = 0;
  maxAbsErrUs = 0;
  failedSends = 0;
  anchorPending = true;
  state = State::Running;
}

// Serial only: credentials must never travel over the network they unlock.
void cmdWifi(char **tok, size_t n) {
  if (cmdLink != &Serial) { replyErr("NOT_ALLOWED", "tcp"); return; }
  if (state == State::Armed || state == State::Running) { replyErr("BUSY", stateName()); return; }
  if (n == 1) { wifiPrintStatus(Serial); return; }
  if (n > 3) { replyErr("BAD_ARGS", "WIFI"); return; }
  wifiSave(tok[1], n == 3 ? tok[2] : "");
  Serial.print("WIFI SAVED -\n");
}

void handleLine(char *s) {
  char *tok[kMaxTokens];
  size_t n = 0;
  char *save;
  for (char *t = strtok_r(s, " \t", &save); t != nullptr; t = strtok_r(nullptr, " \t", &save)) {
    if (n == kMaxTokens) { replyErr("BAD_ARGS", "too_many_fields"); return; }
    tok[n++] = t;
  }
  if (n == 0) return;

  const char *cmd = tok[0];
  bool bare = (n == 1);
  if (strcasecmp(cmd, "EV") == 0) cmdEv(tok, n);
  else if (strcasecmp(cmd, "ARM") == 0) cmdArm(tok, n);
  else if (strcasecmp(cmd, "WIFI") == 0) cmdWifi(tok, n);
  else if (strcasecmp(cmd, "PING") == 0) bare ? cmdPing() : replyErr("BAD_ARGS", "PING");
  else if (strcasecmp(cmd, "FIRE") == 0) bare ? cmdFire() : replyErr("BAD_ARGS", "FIRE");
  else if (strcasecmp(cmd, "ABORT") == 0) bare ? abortTimeline() : replyErr("BAD_ARGS", "ABORT");
  else replyErr("UNKNOWN_CMD", cmd);
}

// Handles at most one complete line per call so a burst of input can't starve the scheduler.
void pollLink(Stream &link, LineReader &r) {
  while (link.available() > 0) {
    int c = link.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (r.len < kLineMax) r.buf[r.len++] = (char)c;
      else r.overflow = true;
      continue;
    }
    cmdLink = &link;
    if (r.overflow) {
      replyErr("LINE_TOO_LONG", "-");
    } else {
      r.buf[r.len] = '\0';
      handleLine(r.buf);
    }
    r.len = 0;
    r.overflow = false;
    return;
  }
}

void runDueEvents() {
  while (nextEvent < eventCount) {
    const Event &ev = events[nextEvent];
    if ((uint32_t)(micros() - anchorUs) < ev.at_us) return;

    SendStamp st = pad.sendState(ev.buttons, ev.dpad, ev.lx, ev.ly, ev.rx, ev.ry);
    int32_t err = (int32_t)(st.before_us - (anchorUs + ev.at_us));
    uint32_t absErr = err < 0 ? 0u - (uint32_t)err : (uint32_t)err;
    if (absErr > maxAbsErrUs) maxAbsErrUs = absErr;
    if (!st.ok) failedSends++;
    results[nextEvent] = {st.before_us, st.after_us - st.before_us, err, st.ok};
    nextEvent++;
  }

  // Anything still held is released here; a timeline should normally end on a release.
  pad.sendNeutral();
  reportResults(true);
  runLink->printf("END %u %lu %u\n", (unsigned)eventCount, (unsigned long)maxAbsErrUs,
                  (unsigned)failedSends);
  resetTimeline();
}

void setup() {
  // A whole run's DONE lines (~64 x 40 bytes) fit in the TX buffer, so serial printing never blocks.
  Serial.setTxBufferSize(4096);
  Serial.setRxBufferSize(1024);
  Serial.begin(kBaud);
  pad.begin();
  Serial.printf("BOOT %s %u\n", kFirmwareVersion, kProtocolVersion);
  wifiBegin();
}

void loop() {
  pollLink(Serial, serialReader);

  if (hasSlack()) {
    bool fresh;
    Stream *tcp = wifiPoll(&fresh);
    if (fresh) tcpReader = {};
    if (tcp != nullptr) pollLink(*tcp, tcpReader);
  }

  if (state == State::Armed && millis() - armedAtMs >= armTimeoutMs) {
    cmdLink = runLink;
    replyErr("ARM_TIMEOUT", "-");
    abortTimeline();
  }

  if (state == State::Running) runDueEvents();

  if (hasSlack()) {
    if (state == State::Running) reportResults(false);
    // None while armed: the first event after FIRE must not queue behind a keep-alive.
    if (state != State::Armed && millis() - lastKeepAliveMs >= kKeepAliveMs) {
      lastKeepAliveMs = millis();
      pad.refresh();
    }
  }
}
