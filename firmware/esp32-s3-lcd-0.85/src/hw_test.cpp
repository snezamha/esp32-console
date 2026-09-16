#include "hw_test.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "board/board.h"
#include "services/radio_stream.h"
#include "net/wireless.h"

namespace {

constexpr const char* kNames[kTestCount] = {"Battery", "Memory", "Buttons", "LED",   "SD card",
                                            "Codec",   "Mic",    "Speaker", "Wi-Fi", "BLE"};
constexpr const char* kKeys[kTestCount] = {"battery", "memory", "buttons", "led",  "sd",
                                           "codec",   "mic",    "speaker", "wifi", "ble"};
// A broken peripheral or driver must never stop the remaining checks from running.
constexpr uint32_t kTestTimeoutMs = 15000;

const char* StatusKey(TestStatus status) {
  switch (status) {
    case TestStatus::Running: return "running";
    case TestStatus::Ok: return "ok";
    case TestStatus::Fail: return "fail";
    case TestStatus::Info: return "info";
    default: return "idle";
  }
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out += ' ';
    } else {
      out += c;
    }
  }
  return out;
}

std::string Format(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return buf;
}

float ToDbfs(double rms) { return rms < 1.0 ? -90.0f : 20.0f * std::log10(rms / 32768.0); }

}  // namespace

const char* HwTest::Name(TestId id) { return kNames[id]; }
const char* HwTest::Key(TestId id) { return kKeys[id]; }

bool HwTest::FromKey(const std::string& key, TestId& id) {
  for (int i = 0; i < kTestCount; i++) {
    if (key == kKeys[i]) {
      id = static_cast<TestId>(i);
      return true;
    }
  }
  return false;
}

void HwTest::Begin() {
  queue_ = xQueueCreate(16, sizeof(Job));
  xTaskCreatePinnedToCore(TaskMain, "hw_test", 8192, this, 2, nullptr, 0);
}

void HwTest::Run(TestId id) {
  Job job{JobType::Test, id, 0};
  xQueueSend(queue_, &job, 0);
}

void HwTest::RunAll() {
  Job job{JobType::All, 0, 0};
  xQueueSend(queue_, &job, 0);
}

void HwTest::PlayTone(int frequency_hz, int duration_ms) {
  Job job{JobType::Tone, frequency_hz, duration_ms};
  xQueueSend(queue_, &job, 0);
}

void HwTest::Record(int duration_ms) {
  Job job{JobType::Record, duration_ms, 0};
  xQueueSend(queue_, &job, 0);
}

void HwTest::Beep(int frequency_hz, int duration_ms) {
  if (busy_ || RadioStream::Get().Active() || uxQueueMessagesWaiting(queue_) > 0) return;
  Job job{JobType::Beep, frequency_hz, duration_ms};
  xQueueSend(queue_, &job, 0);
}

TestResult HwTest::Get(TestId id) {
  std::lock_guard<std::mutex> lock(mutex_);
  return results_[id];
}

void HwTest::SetResult(TestId id, TestStatus status, const std::string& detail,
                       const std::string& extra_json) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    results_[id].status = status;
    results_[id].detail = detail;
  }
  if (on_change_) on_change_();
  Serial.printf("{\"test\":\"%s\",\"status\":\"%s\",\"detail\":\"%s\"%s%s}\n", kKeys[id],
                StatusKey(status), JsonEscape(detail).c_str(), extra_json.empty() ? "" : ",",
                extra_json.c_str());
}

void HwTest::TaskMain(void* arg) {
  auto self = static_cast<HwTest*>(arg);
  Job job;
  for (;;) {
    if (xQueueReceive(self->queue_, &job, portMAX_DELAY) != pdTRUE) continue;
    // Beeps are UI feedback, not checks: they do not mark the tester busy.
    if (job.type == JobType::Beep) {
      if (!RadioStream::Get().Active() && self->EnsureCodec()) self->WriteTone(job.a, job.b);
      continue;
    }
    self->busy_ = true;
    switch (job.type) {
      case JobType::Test:
        self->RunGuarded(static_cast<TestId>(job.a));
        break;
      case JobType::All:
        for (int i = 0; i < kTestCount; i++) self->RunGuarded(static_cast<TestId>(i));
        Serial.println("{\"event\":\"tests_done\"}");
        break;
      case JobType::Tone: {
        if (!self->EnsureCodec()) break;
        float l, r;
        int pl, pr;
        auto codec = Board::GetInstance().GetAudioCodec();
        codec->EnableOutput(true);
        self->Measure(job.b, l, r, pl, pr, job.a);
        codec->EnableOutput(false);
        Serial.printf("{\"tone\":{\"hz\":%d,\"ms\":%d,\"mic_l_db\":%.1f,\"mic_r_db\":%.1f}}\n",
                      job.a, job.b, l, r);
        break;
      }
      case JobType::Beep:
        break;
      case JobType::Record: {
        if (!self->EnsureCodec()) break;
        float l, r;
        int pl, pr;
        self->Measure(job.a, l, r, pl, pr);
        Serial.printf(
            "{\"mic\":{\"ms\":%d,\"rms_l_db\":%.1f,\"rms_r_db\":%.1f,\"p2p_l\":%d,\"p2p_r\":%d}}\n",
            job.a, l, r, pl, pr);
        break;
      }
    }
    self->mic_level_ = 0;
    self->busy_ = false;
  }
}

void HwTest::TaskStep(void* arg) {
  const auto context = *static_cast<StepContext*>(arg);
  context.self->Execute(context.id);
  xTaskNotifyGive(context.supervisor);
  vTaskDelete(nullptr);
}

void HwTest::RunGuarded(TestId id) {
  // Discard a notification that raced the previous timeout boundary.
  ulTaskNotifyTake(pdTRUE, 0);
  StepContext context{this, id, xTaskGetCurrentTaskHandle()};
  TaskHandle_t step = nullptr;
  if (xTaskCreatePinnedToCore(TaskStep, "hw_step", 8192, &context, 2, &step, 0) != pdPASS) {
    SetResult(id, TestStatus::Fail, "cannot start");
    return;
  }
  if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kTestTimeoutMs)) == 0) {
    vTaskDelete(step);
    Board::GetInstance().GetAudioCodec()->EnableOutput(false);
    Board::GetInstance().ApplyLed();
    mic_level_ = 0;
    SetResult(id, TestStatus::Fail, "timeout after 15s");
    return;
  }
  // Every automatic check must leave a terminal result, even if a future implementation returns
  // early by mistake. Buttons are deliberately Info/OK because they are a manual check.
  if (Get(id).status == TestStatus::Running) SetResult(id, TestStatus::Fail, "finished without result");
}

void HwTest::Execute(TestId id) {
  if (id == kTestButtons) {
    // Interactive: the app marks buttons as they are pressed.
    if (Get(kTestButtons).status == TestStatus::Idle) {
      SetResult(kTestButtons, TestStatus::Info, "manual: press PWR + -");
    }
    return;
  }

  SetResult(id, TestStatus::Running, "...");
  switch (id) {
    case kTestBattery: TestBattery(); break;
    case kTestMemory: TestMemory(); break;
    case kTestLed: TestLed(); break;
    case kTestSdCard: TestSdCard(); break;
    case kTestCodec: TestCodec(); break;
    case kTestMic: TestMic(); break;
    case kTestSpeaker: TestSpeaker(); break;
    case kTestWifi: TestWifi(); break;
    case kTestBle: TestBle(); break;
    default: break;
  }
}

void HwTest::TestBattery() {
  auto power = Board::GetInstance().GetPowerManager();
  const float volts = power->GetBatteryVoltage();
  const bool charging = power->IsCharging();
  const std::string extra = Format("\"voltage\":%.2f,\"level\":%d,\"charging\":%s", volts,
                                   power->GetBatteryLevel(), charging ? "true" : "false");
  if (volts <= 0.0f) {
    SetResult(kTestBattery, TestStatus::Fail, "no ADC", extra);
  } else if (volts < 3.0f) {
    SetResult(kTestBattery, TestStatus::Info, Format("%.2fV none", volts), extra);
  } else {
    SetResult(kTestBattery, TestStatus::Ok, Format("%.2fV%s", volts, charging ? " chg" : ""), extra);
  }
}

void HwTest::TestMemory() {
  const uint32_t psram = ESP.getPsramSize();
  const uint32_t flash = ESP.getFlashChipSize();
  const std::string extra = Format("\"psram\":%lu,\"flash\":%lu,\"free_heap\":%lu",
                                   (unsigned long)psram, (unsigned long)flash,
                                   (unsigned long)ESP.getFreeHeap());

  bool psram_ok = false;
  if (psram > 0) {
    const size_t size = 1024 * 1024;
    auto block = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (block) {
      for (size_t i = 0; i < size; i++) block[i] = i * 31;
      psram_ok = true;
      for (size_t i = 0; i < size; i++) {
        if (block[i] != static_cast<uint8_t>(i * 31)) {
          psram_ok = false;
          break;
        }
      }
      heap_caps_free(block);
    }
  }

  const std::string detail = Format("%luM/%luM", (unsigned long)(psram / (1024 * 1024)),
                                    (unsigned long)(flash / (1024 * 1024)));
  SetResult(kTestMemory, psram_ok ? TestStatus::Ok : TestStatus::Fail, detail, extra);
}

void HwTest::TestLed() {
  auto ring = Board::GetInstance().GetLedRing();
  const struct {
    RgbColor color;
    const char* name;
  } steps[] = {{{32, 0, 0}, "red"}, {{0, 32, 0}, "green"}, {{0, 0, 32}, "blue"}, {{24, 24, 24}, "white"}};

  for (const auto& step : steps) {
    SetResult(kTestLed, TestStatus::Running, step.name);
    ring->SetAll(step.color);
    ring->Show();
    delay(500);
  }
  SetResult(kTestLed, TestStatus::Running, "chase");
  for (int i = 0; i < ring->count() * 2; i++) {
    ring->Clear();
    ring->SetPixel(i % ring->count(), {24, 24, 24});
    ring->Show();
    delay(70);
  }
  ring->Clear();
  ring->Show();
  SetResult(kTestLed, TestStatus::Ok, Format("%d LEDs", ring->count()));
}

void HwTest::TestSdCard() {
  auto sd = Board::GetInstance().GetSdCard();
  if (!sd->Mount()) {
    SetResult(kTestSdCard, TestStatus::Fail, sd->ProblemLabel());
    return;
  }

  static constexpr const char* kPath = "/hwtest.bin";
  std::vector<uint8_t> pattern(8192);
  for (size_t i = 0; i < pattern.size(); i++) pattern[i] = (i * 7) ^ 0x5A;

  bool ok = false;
  auto& fs = sd->fs();
  if (File f = fs.open(kPath, FILE_WRITE)) {
    ok = f.write(pattern.data(), pattern.size()) == pattern.size();
    f.close();
  }
  if (ok) {
    std::vector<uint8_t> readback(pattern.size());
    if (File f = fs.open(kPath, FILE_READ)) {
      ok = f.read(readback.data(), readback.size()) == readback.size() && readback == pattern;
      f.close();
    } else {
      ok = false;
    }
  }
  fs.remove(kPath);

  const uint64_t total_mb = sd->total_bytes() / (1024 * 1024);
  const std::string extra = Format("\"total\":%llu,\"used\":%llu,\"read_write\":%s",
                                   sd->total_bytes(), sd->used_bytes(), ok ? "true" : "false");
  const std::string size = total_mb >= 1024 ? Format("%lluGB", total_mb / 1024) : Format("%lluMB", total_mb);
  SetResult(kTestSdCard, ok ? TestStatus::Ok : TestStatus::Fail, size + (ok ? " rw" : " rw err"),
            extra);
}

bool HwTest::EnsureCodec() {
  auto codec = Board::GetInstance().GetAudioCodec();
  if (!codec->started()) codec->Start();
  return codec->started();
}

void HwTest::WriteTone(int frequency_hz, int duration_ms) {
  auto codec = Board::GetInstance().GetAudioCodec();
  const int rate = codec->sample_rate();
  const int frames = rate * duration_ms / 1000;
  const int fade = std::max(1, frames / 5);  // Soft edges avoid clicks
  std::vector<int16_t> samples(frames * 2);
  for (int i = 0; i < frames; i++) {
    const float envelope = std::min({1.0f, static_cast<float>(i) / fade,
                                     static_cast<float>(frames - 1 - i) / fade});
    const int16_t s = 9000 * envelope * std::sin(2.0 * M_PI * frequency_hz * i / rate);
    samples[i * 2] = s;
    samples[i * 2 + 1] = s;
  }
  codec->EnableOutput(true);
  codec->Write(samples.data(), frames);
  // Let the DMA buffers drain before the amplifier turns off.
  delay(40);
  codec->EnableOutput(false);
}

void HwTest::TestCodec() {
  auto codec = Board::GetInstance().GetAudioCodec();
  const bool ok = EnsureCodec() && codec->es8311_found() && codec->es7210_found();
  const std::string extra = Format("\"es8311\":%s,\"es7210\":%s,\"sample_rate\":%d",
                                   codec->es8311_found() ? "true" : "false",
                                   codec->es7210_found() ? "true" : "false", codec->sample_rate());
  std::string detail;
  if (ok) {
    detail = "8311+7210";
  } else {
    detail = codec->es8311_found() ? "no 7210" : (codec->es7210_found() ? "no 8311" : "none");
  }
  SetResult(kTestCodec, ok ? TestStatus::Ok : TestStatus::Fail, detail, extra);
}

void HwTest::Measure(int duration_ms, float& rms_l_db, float& rms_r_db, int& p2p_l, int& p2p_r,
                     int tone_hz) {
  auto codec = Board::GetInstance().GetAudioCodec();
  const int rate = codec->sample_rate();
  const int chunk = 240;
  std::vector<int16_t> in(chunk * 2);
  std::vector<int16_t> out(chunk * 2);

  double sum_l = 0, sum_r = 0;
  int min_l = INT16_MAX, max_l = INT16_MIN, min_r = INT16_MAX, max_r = INT16_MIN;
  long frames = 0;
  long phase = 0;
  const long total = static_cast<long>(rate) * duration_ms / 1000;
  const int amplitude = 12000;

  while (frames < total) {
    if (tone_hz > 0) {
      for (int i = 0; i < chunk; i++) {
        const int16_t s = amplitude * std::sin(2.0 * M_PI * tone_hz * (phase + i) / rate);
        out[i * 2] = s;
        out[i * 2 + 1] = s;
      }
      phase += chunk;
      codec->Write(out.data(), chunk);
    }

    const int n = codec->Read(in.data(), chunk);
    if (n <= 0) break;
    double chunk_sum = 0;
    for (int i = 0; i < n; i++) {
      const int l = in[i * 2], r = in[i * 2 + 1];
      sum_l += static_cast<double>(l) * l;
      sum_r += static_cast<double>(r) * r;
      chunk_sum += static_cast<double>(l) * l;
      min_l = std::min(min_l, l);
      max_l = std::max(max_l, l);
      min_r = std::min(min_r, r);
      max_r = std::max(max_r, r);
    }
    frames += n;
    const float db = ToDbfs(std::sqrt(chunk_sum / n));
    mic_level_ = std::max(0, std::min(100, static_cast<int>((db + 70.0f) * 100.0f / 70.0f)));
  }

  rms_l_db = frames ? ToDbfs(std::sqrt(sum_l / frames)) : -90.0f;
  rms_r_db = frames ? ToDbfs(std::sqrt(sum_r / frames)) : -90.0f;
  p2p_l = frames ? max_l - min_l : 0;
  p2p_r = frames ? max_r - min_r : 0;
}

void HwTest::TestMic() {
  if (!EnsureCodec() || !Board::GetInstance().GetAudioCodec()->es7210_found()) {
    SetResult(kTestMic, TestStatus::Fail, "no codec");
    return;
  }
  float l, r;
  int pl, pr;
  Measure(200, l, r, pl, pr);  // Settle
  Measure(1500, l, r, pl, pr);

  const std::string extra =
      Format("\"rms_l_db\":%.1f,\"rms_r_db\":%.1f,\"p2p_l\":%d,\"p2p_r\":%d", l, r, pl, pr);
  // A live microphone always shows some noise; a dead input reads flat.
  const bool ok = pl > 8 || pr > 8;
  SetResult(kTestMic, ok ? TestStatus::Ok : TestStatus::Fail,
            ok ? Format("%.0f/%.0fdB", l, r) : "silent", extra);
}

void HwTest::TestSpeaker() {
  auto codec = Board::GetInstance().GetAudioCodec();
  if (!EnsureCodec() || !codec->es8311_found()) {
    SetResult(kTestSpeaker, TestStatus::Fail, "no codec");
    return;
  }

  float base_l, base_r, tone_l, tone_r;
  int pl, pr;
  Measure(400, base_l, base_r, pl, pr);

  codec->EnableOutput(true);
  delay(50);
  Measure(1000, tone_l, tone_r, pl, pr, 1000);
  codec->EnableOutput(false);

  // The microphones pick up the speaker: compare levels with and without the tone.
  const float gain = std::max(tone_l - base_l, tone_r - base_r);
  const std::string extra =
      Format("\"volume\":%d,\"base_l_db\":%.1f,\"base_r_db\":%.1f,\"tone_l_db\":%.1f,"
             "\"tone_r_db\":%.1f",
             codec->output_volume(), base_l, base_r, tone_l, tone_r);
  if (gain >= 6.0f) {
    SetResult(kTestSpeaker, TestStatus::Ok, Format("+%.0fdB", gain), extra);
  } else {
    SetResult(kTestSpeaker, TestStatus::Info, "not heard", extra);
  }
}

void HwTest::TestWifi() {
  std::vector<WifiNetwork> networks;
  if (!Wireless::ScanWifi(networks)) {
    SetResult(kTestWifi, TestStatus::Fail, "radio err");
    return;
  }

  std::string list = "\"networks\":[";
  for (size_t i = 0; i < networks.size() && i < 10; i++) {
    if (i) list += ",";
    list += Format("{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d}",
                   JsonEscape(networks[i].ssid).substr(0, 40).c_str(), networks[i].rssi,
                   networks[i].channel);
  }
  list += "]";

  const std::string detail = networks.empty()
                                 ? "0 found"
                                 : Format("%d %ddBm", static_cast<int>(networks.size()), networks[0].rssi);
  SetResult(kTestWifi, TestStatus::Ok, detail, list);
}

void HwTest::TestBle() {
  std::vector<BleDevice> devices;
  if (!Wireless::ScanBle(devices, 3)) {
    SetResult(kTestBle, TestStatus::Fail, "radio err");
    return;
  }

  std::string list = "\"devices\":[";
  for (size_t i = 0; i < devices.size() && i < 10; i++) {
    if (i) list += ",";
    list += Format("{\"address\":\"%s\",\"name\":\"%s\",\"rssi\":%d}", devices[i].address.c_str(),
                   JsonEscape(devices[i].name).substr(0, 30).c_str(), devices[i].rssi);
  }
  list += "]";

  const std::string detail = devices.empty()
                                 ? "0 found"
                                 : Format("%d %ddBm", static_cast<int>(devices.size()), devices[0].rssi);
  SetResult(kTestBle, TestStatus::Ok, detail, list);
}
