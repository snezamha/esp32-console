#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>

enum TestId {
  kTestBattery,
  kTestMemory,
  kTestButtons,
  kTestLed,
  kTestSdCard,
  kTestCodec,
  kTestMic,
  kTestSpeaker,
  kTestWifi,
  kTestBle,
  kTestCount,
};

enum class TestStatus { Idle, Running, Ok, Fail, Info };

struct TestResult {
  TestStatus status = TestStatus::Idle;
  std::string detail;
};

// Runs hardware checks on a background task. Every result is also printed
// to Serial as one JSON line: {"test":"mic","status":"ok","detail":"…"}.
class HwTest {
 public:
  static HwTest& GetInstance() {
    static HwTest instance;
    return instance;
  }

  void Begin();
  // Called (from any task) whenever a result changes.
  void SetOnChange(std::function<void()> callback) { on_change_ = callback; }

  void Run(TestId id);
  void RunAll();
  void PlayTone(int frequency_hz, int duration_ms);
  void Record(int duration_ms);
  // Short silent-log beep; dropped while a check is running.
  void Beep(int frequency_hz = 2000, int duration_ms = 25);

  TestResult Get(TestId id);
  void SetResult(TestId id, TestStatus status, const std::string& detail,
                 const std::string& extra_json = "");
  bool IsBusy() const { return busy_; }
  // Live microphone level 0–100 while the mic or speaker test runs.
  int mic_level() const { return mic_level_; }

  static const char* Name(TestId id);
  static const char* Key(TestId id);
  static bool FromKey(const std::string& key, TestId& id);

 private:
  HwTest() = default;

  enum class JobType { Test, All, Tone, Record, Beep };
  struct Job {
    JobType type;
    int a;
    int b;
  };

  static void TaskMain(void* arg);
  void Execute(TestId id);

  void TestBattery();
  void TestMemory();
  void TestLed();
  void TestSdCard();
  void TestCodec();
  void TestMic();
  void TestSpeaker();
  void TestWifi();
  void TestBle();

  bool EnsureCodec();
  void WriteTone(int frequency_hz, int duration_ms);
  // Reads for `duration_ms`, returns RMS per channel in dBFS and peak-to-peak.
  void Measure(int duration_ms, float& rms_l_db, float& rms_r_db, int& p2p_l, int& p2p_r,
               int tone_hz = 0);

  QueueHandle_t queue_ = nullptr;
  std::mutex mutex_;
  TestResult results_[kTestCount];
  std::atomic<bool> busy_{false};
  std::atomic<int> mic_level_{0};
  std::function<void()> on_change_;
};
