// shiny_pad: executes a pre-uploaded timeline of pad states at exact
// microsecond offsets. Wire format: "Serial protocol" in the repo README.

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "timed_pad.h"

const char kFirmwareVersion[] = "0.1.0";
const unsigned kProtocolVersion = 1;
const uint32_t kBaud = 115200;
const size_t kMaxEvents = 64;
const uint32_t kMinSpacingUs = 2000;     // provisional until F3 measures the floor
const uint32_t kRefreshGuardUs = 10000;  // > worst-case 8 ms poll, so a keep-alive never delays an event
const size_t kLineMax = 96;
const size_t kMaxTokens = 10;
const uint8_t kStickCentre = 0x80;
const uint16_t kButtonMask = 0x3FFF;     // 14 buttons in the descriptor
const uint8_t kDpadCentred = 15;

struct Event {
  uint32_t at_us;
  uint16_t seq, buttons;
  uint8_t dpad, lx, ly, rx, ry;
};

enum class State : uint8_t { Idle, Loading, Armed, Running };

static TimedPad pad;
static Event events[kMaxEvents];
static size_t eventCount = 0;
static size_t nextEvent = 0;
static State state = State::Idle;

static uint32_t armedAtMs = 0;
static uint32_t armTimeoutMs = 0;
static uint32_t anchorUs = 0;
static uint32_t maxAbsErrUs = 0;
static size_t failedSends = 0;

static char line[kLineMax + 1];
static size_t lineLen = 0;
static bool lineOverflow = false;

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
  Serial.printf("ERR %s %s\n", code, detail);
}

void replyErrSeq(const char *code, uint32_t seq) {
  Serial.printf("ERR %s %lu\n", code, (unsigned long)seq);
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

void resetTimeline() {
  eventCount = 0;
  nextEvent = 0;
  state = State::Idle;
}

void abortTimeline() {
  resetTimeline();
  pad.sendNeutral();
  Serial.print("ABORTED\n");
}

void cmdPing() {
  Serial.printf("PONG %s %u %u\n", kFirmwareVersion, kProtocolVersion, (unsigned)freeSlots());
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
  Serial.printf("OK %lu %u\n", (unsigned long)seq, (unsigned)freeSlots());
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
  state = State::Armed;
  Serial.printf("ARMED %u\n", (unsigned)eventCount);
}

void cmdFire() {
  if (state != State::Armed) { replyErr("NOT_ARMED", stateName()); return; }
  anchorUs = micros();
  nextEvent = 0;
  maxAbsErrUs = 0;
  failedSends = 0;
  state = State::Running;
  Serial.printf("ANCHOR %lu\n", (unsigned long)anchorUs);
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
  else if (strcasecmp(cmd, "PING") == 0) bare ? cmdPing() : replyErr("BAD_ARGS", "PING");
  else if (strcasecmp(cmd, "FIRE") == 0) bare ? cmdFire() : replyErr("BAD_ARGS", "FIRE");
  else if (strcasecmp(cmd, "ABORT") == 0) bare ? abortTimeline() : replyErr("BAD_ARGS", "ABORT");
  else replyErr("UNKNOWN_CMD", cmd);
}

// Handles at most one complete line per call so a burst of input can't starve the scheduler.
void pollSerial() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (lineLen < kLineMax) line[lineLen++] = (char)c;
      else lineOverflow = true;
      continue;
    }
    if (lineOverflow) {
      replyErr("LINE_TOO_LONG", "-");
    } else {
      line[lineLen] = '\0';
      handleLine(line);
    }
    lineLen = 0;
    lineOverflow = false;
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

    Serial.printf("DONE %u %lu %ld %lu\n", (unsigned)ev.seq, (unsigned long)st.before_us,
                  (long)err, (unsigned long)(st.after_us - st.before_us));
    if (!st.ok) {
      failedSends++;
      replyErrSeq("SEND", ev.seq);
    }
    nextEvent++;
  }

  // Anything still held is released here; a timeline should normally end on a release.
  pad.sendNeutral();
  Serial.printf("END %u %lu %u\n", (unsigned)eventCount, (unsigned long)maxAbsErrUs,
                (unsigned)failedSends);
  resetTimeline();
}

bool refreshAllowed() {
  if (state != State::Running) return true;
  uint32_t elapsed = micros() - anchorUs;
  uint32_t due = events[nextEvent].at_us;
  return due > elapsed && due - elapsed > kRefreshGuardUs;
}

void setup() {
  // A whole run's DONE lines (~64 x 40 bytes) fit in the TX buffer, so printing never blocks mid-run.
  Serial.setTxBufferSize(4096);
  Serial.setRxBufferSize(1024);
  Serial.begin(kBaud);
  pad.begin();
  Serial.printf("BOOT %s %u\n", kFirmwareVersion, kProtocolVersion);
}

void loop() {
  pollSerial();

  if (state == State::Armed && millis() - armedAtMs >= armTimeoutMs) {
    replyErr("ARM_TIMEOUT", "-");
    abortTimeline();
  }

  if (state == State::Running) runDueEvents();

  if (refreshAllowed()) pad.refresh();
}
