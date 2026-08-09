#include "broker_exec/observability/structured_logger.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/logger.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/spdlog.h>

#include "broker_exec/domain/redaction.hpp"
#include "broker_exec/observability/audit_event.hpp"

namespace broker_exec::observability {

namespace {

// ── The provenance-id exemption, IMP-15 ──────────────────────────────────────
//
// THE DEFECT THIS FIXES: `domain::scrub`'s bare high-entropy rule redacts any
// run of >=20 token chars mixing letters and digits. A client_ref is
// `<strategy>-<sig8>-<uuid>` — one 51-char run of exactly that shape — so
// EVERY audit line used to render `"client_ref":"***REDACTED***"` (the whole
// ref, separators and all: '-' is a token char, so the run never split). The
// audit trail lost the one identifier it exists to carry, and an operator could
// not join a log line back to the store, the intent log or the ledger.
//
// THE FIX, and why it is shaped like this: the redaction binding stays EXACTLY
// where it was — ONE `domain::scrub` call over the WHOLE rendered line. The only
// change is that each TYPED PROVENANCE COLUMN whose value passes
// `domain::is_provenance_id_shape` is rendered as a SENTINEL, and the real value
// is put back AFTER the scrub. Because the scrub still runs over the whole line,
// `fields` and every other rendered byte keep byte-identical semantics — the
// key=value rule, the MPIN auth-context window and the bare-run rule all still
// see the same surrounding text they saw before. Nothing about free-form content
// is relaxed; the Story-4.2 lesson ("every consumer of `fields` scrubs itself")
// is untouched.
//
// A column that is NOT id-shaped is left holding its real value here, so it goes
// through the identical scrub it went through before (fail closed).

// One sentinel per typed column. Three properties make the swap invisible to
// scrub(), and all three are load-bearing:
//   * ALL LETTERS, no digit -> the bare high-entropy rule (>=20 chars mixing
//     letters AND digits) can never fire, so scrub() returns them verbatim. This
//     is the documented all-letter limitation in redaction.cpp, relied on here
//     deliberately rather than by accident.
//   * no sensitive-key needle (token/secret/password/api_key/mpin/totp/bearer)
//     and rendered in VALUE position (followed by `","`), so the key=value rule
//     cannot fire either.
//   * no "pin"/"otp"/"mpin"/"totp" substring, so swapping a real value for a
//     sentinel cannot change the 10-char auth-context window of anything after
//     it (which would alter how a neighbouring digit run is treated).
//
// AND THEY ARE UNGUESSABLE, which is a SECURITY property, not tidiness. When the
// sentinels were compile-time literals, any caller text that merely CONTAINED one
// (say `fields["reason"] = "rejected: PROVENANCESENTINELCLIENTREF"`) tripped the
// safety net below and threw the whole splice away, silently stripping provenance
// from that line. Repeated deliberately, that is ANTI-FORENSICS: an attacker
// picks which orders become un-correlatable. So each sentinel carries a
// per-process random tail (see make_sentinels) that no caller can know.
struct ProvenanceColumn {
  std::string_view key;              // the rendered JSON key
  std::string AuditEvent::* member;  // the typed column it renders from
  std::string_view tag;              // fixed, human-legible part of the sentinel
};

constexpr std::array<ProvenanceColumn, 5> kProvenanceColumns = {{
    {"strategy", &AuditEvent::strategy, "STRATEGY"},
    {"broker", &AuditEvent::broker, "BROKER"},
    {"account", &AuditEvent::account, "ACCOUNT"},
    {"client_ref", &AuditEvent::client_ref, "CLIENTREF"},
    {"broker_order_id", &AuditEvent::broker_order_id, "ORDERID"},
}};

constexpr std::string_view kSentinelPrefix = "PROVENANCESENTINEL";
constexpr std::size_t kSentinelRandomChars = 16;

// The random tail's alphabet: the uppercase letters MINUS 'E' and 'P'. Both
// exclusions are deliberate and keep EVERY line of the scrub reasoning above
// true no matter what is drawn:
//   * letters only  -> still no digit, so the bare high-entropy rule cannot fire.
//   * no 'E', no 'P' -> a random draw can never spell a sensitive-key needle
//     (token/secret/bearer all need an 'e'; password/api_key/apikey need a 'p')
//     nor an auth-context keyword (mpin/totp/otp/pin all need a 'p'). No
//     rejection sampling, no rare heisenbug where one process picks a tail that
//     changes how the surrounding line is scrubbed.
// Nor can a needle straddle the join: no needle starts with a suffix of any
// `PROVENANCESENTINEL<TAG>` (the only near miss, "...ACCOUNT" + "OKEN"/"OTP",
// needs the excluded 'E'/'P').
constexpr std::string_view kSentinelAlphabet = "ABCDFGHIJKLMNOQRSTUVWXYZ";

// Build the per-process sentinels ONCE. 24^16 (~1.8e22) possibilities per column
// means a caller cannot plant a colliding literal, so the safety net can no
// longer be tripped on purpose.
[[nodiscard]] std::array<std::string, kProvenanceColumns.size()> make_sentinels() {
  std::uint64_t seed = 0;
  try {
    std::random_device rd;
    seed = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
  } catch (...) {
    // A platform with no usable random_device must still start: fall back to the
    // monotonic clock. Weaker, but the value is still not a compile-time constant
    // a caller can read off the source.
    seed = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
  }
  std::mt19937_64 engine(seed);
  std::uniform_int_distribution<std::size_t> pick(0, kSentinelAlphabet.size() - 1);

  std::array<std::string, kProvenanceColumns.size()> out;
  for (std::size_t i = 0; i < out.size(); ++i) {
    std::string sentinel;
    sentinel.reserve(kSentinelPrefix.size() + kProvenanceColumns[i].tag.size() +
                     kSentinelRandomChars);
    sentinel.append(kSentinelPrefix);
    sentinel.append(kProvenanceColumns[i].tag);  // keeps the columns distinct and greppable
    for (std::size_t k = 0; k < kSentinelRandomChars; ++k) {
      sentinel.push_back(kSentinelAlphabet[pick(engine)]);
    }
    out[i] = std::move(sentinel);
  }
  return out;
}

// The one shared instance, built on first use (thread-safe magic static). Only
// reached from render_line, which the caller runs inside log()'s try/catch, so an
// allocation failure here still cannot escape the no-throw boundary.
[[nodiscard]] const std::array<std::string, kProvenanceColumns.size()>& sentinels() {
  static const std::array<std::string, kProvenanceColumns.size()> kSentinels = make_sentinels();
  return kSentinels;
}

// THE redaction binding, unchanged since Story 4.1: scrub the WHOLE rendered
// line, so a token-shaped run in ANY field (typed column or `fields`) is
// replaced before it reaches a sink. No secret in any observable sink (SEC-3).
[[nodiscard]] std::string scrubbed_line(const AuditEvent& ev) {
  return domain::scrub(to_json_line(ev));
}

// Render `"<key>":"<value>"` — the exact bytes nlohmann emits for a plain-ASCII
// key and an id-shaped value. An id-shaped value contains no '"' and no '\'
// (see the allowlist charset), so it needs NO JSON escaping and the spliced line
// stays valid JSON.
[[nodiscard]] std::string rendered_pair(std::string_view key, std::string_view value) {
  std::string pair;
  pair.reserve(key.size() + value.size() + 5);
  pair += '"';
  pair.append(key);
  pair += "\":\"";
  pair.append(value);
  pair += '"';
  return pair;
}

// Render `ev` to its scrubbed JSON line, keeping id-shaped typed provenance
// columns intact. Free-form content is scrubbed exactly as it was before.
[[nodiscard]] std::string render_line(AuditEvent ev) {
  const std::array<std::string, kProvenanceColumns.size()>& sentinel = sentinels();
  std::array<std::string, kProvenanceColumns.size()> preserved;
  bool any_preserved = false;

  for (std::size_t i = 0; i < kProvenanceColumns.size(); ++i) {
    std::string& column = ev.*(kProvenanceColumns[i].member);
    if (domain::is_provenance_id_shape(column)) {
      preserved[i] = column;  // non-empty iff swapped (an id shape is never empty)
      column = sentinel[i];
      any_preserved = true;
    }
  }
  if (!any_preserved) {
    return scrubbed_line(ev);  // nothing exempt — byte-for-byte the old path
  }

  std::string line = scrubbed_line(ev);

  bool spliced_cleanly = true;
  for (std::size_t i = 0; i < kProvenanceColumns.size() && spliced_cleanly; ++i) {
    if (preserved[i].empty()) {
      continue;
    }
    // Match the sentinel TOGETHER WITH ITS KEY. A copy of the sentinel sitting
    // inside some caller value cannot forge this pattern, because nlohmann
    // escapes the quotes of any `"..."` nested in a string value.
    const std::string pattern = rendered_pair(kProvenanceColumns[i].key, sentinel[i]);
    const std::size_t at = line.find(pattern);
    if (at == std::string::npos) {
      spliced_cleanly = false;  // unreachable in practice; treat it as a miss anyway
      break;
    }
    line.replace(at, pattern.size(), rendered_pair(kProvenanceColumns[i].key, preserved[i]));
  }

  // THE SAFETY NET. A splice that missed, or a sentinel that is still in the
  // line, means the line is not the line we think it is. Throw the whole attempt
  // away and emit the LEGACY whole-line scrub: a redacted id is merely a lost id,
  // whereas an emitted sentinel (or a value spliced into the wrong slot) would be
  // a WRONG id, and a wrong id in an audit trail is worse than a missing one.
  //
  // It is a NET, not a lever a caller can pull: the sentinels carry a per-process
  // random tail, so caller text can no longer contain one and force this
  // fallback. Planting the fixed `PROVENANCESENTINEL<TAG>` prefix in `fields`
  // matches nothing here — that prefix is not a sentinel.
  if (spliced_cleanly) {
    for (std::size_t i = 0; i < kProvenanceColumns.size(); ++i) {
      if (line.find(sentinel[i]) != std::string::npos) {
        spliced_cleanly = false;
        break;
      }
    }
  }
  if (!spliced_cleanly) {
    for (std::size_t i = 0; i < kProvenanceColumns.size(); ++i) {
      if (!preserved[i].empty()) {
        ev.*(kProvenanceColumns[i].member) = preserved[i];
      }
    }
    return scrubbed_line(ev);
  }

  return line;
}

}  // namespace

StructuredLogger::StructuredLogger(std::shared_ptr<spdlog::logger> logger,
                                   const ports::ClockPort& clock)
    : logger_(std::move(logger)), clock_(clock) {}

void StructuredLogger::log(AuditEvent ev) {
  // Stamp an unset timestamp from the injected clock (never read the wall clock
  // directly — FR-23). Empty == the system_clock epoch default.
  if (ev.ts == std::chrono::system_clock::time_point{}) {
    ev.ts = clock_.now_wall();
  }

  const EventType type = ev.type;  // read before the render consumes `ev`

  // The WHOLE render + scrub + emit is contained here: rendering (to_json_line)
  // or scrubbing (domain::scrub) could throw std::bad_alloc, and a misbehaving
  // sink (full disk, broken pipe) could throw on emit — so swallow everything,
  // logging is best-effort and nothing throws across the boundary. The
  // provenance-id splice below allocates too, so it lives inside the same try.
  try {
    const std::string line = render_line(std::move(ev));

    if (logger_ == nullptr) {
      return;
    }
    switch (type) {
      case EventType::Error:
        logger_->error(line);
        break;
      case EventType::Override:
        logger_->warn(line);
        break;
      default:
        logger_->info(line);
        break;
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch) — logging is best-effort.
    // Deliberately swallowed: a logging failure must not propagate.
  }
}

std::shared_ptr<spdlog::logger> StructuredLogger::make_logger(
    std::shared_ptr<spdlog::sinks::sink> sink, std::string name) {
  auto logger = std::make_shared<spdlog::logger>(std::move(name), std::move(sink));
  // Raw passthrough: we emit our own JSON, so spdlog must NOT prepend a
  // timestamp/level/logger-name pattern. "%v" is just the message verbatim.
  logger->set_pattern("%v");
  return logger;
}

}  // namespace broker_exec::observability
