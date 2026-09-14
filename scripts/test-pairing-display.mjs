import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
const source = readFileSync("firmware/esp32-s3-lcd-0.85/src/app.cpp", "utf8");
const draw = source.slice(source.indexOf("void App::DrawHome("), source.indexOf("void App::UpdateStatus("));
const test = `
#include <string>
#include <vector>
#include <utility>
#include <cstdint>
#include <cassert>
#include <algorithm>
#include <iostream>
struct Theme { uint16_t text=1,muted=2,info=3,fail=4; };
struct Canvas {
 std::vector<std::string> texts;
 static int LineHeight(int scale=1) { return scale*8; }
 void TextCentered(int,int,const char* text,uint16_t,int=1) { texts.push_back(text); }
 void TextMarquee(int,int,int,const char* text,uint16_t,int,bool) { texts.push_back(text); }
 bool Has(const std::string& text) { return std::find(texts.begin(),texts.end(),text)!=texts.end(); }
};
struct Network {
 enum class WifiState { Off, Setup, Connecting, Connected };
 WifiState state=WifiState::Connected;
 static Network& GetInstance() { static Network n; return n; }
 WifiState State() { return state; }
 std::string DeviceName() { return "board"; }
 std::string AccessPointIp() { return "192.168.4.1"; }
 std::string WifiSsid() { return "WiFi"; }
 std::string WifiIp() { return "192.168.1.2"; }
 bool AccessPointActive() { return false; }
};
struct ConsoleClient {
 enum class State { Off, Offline, Connecting, Pairing, Linked, Error };
 State state=State::Pairing; bool linked=false;
 std::string code="123456",server="console.example.com",error,name;
 static ConsoleClient& GetInstance() { static ConsoleClient c; return c; }
 State GetState() { return state; }
 bool HasLink() { return linked; }
 const std::string& Code() { return code; }
 const std::string& Server() { return server; }
 std::string ServerHost() { return server; }
 const std::string& LastError() { return error; }
 const std::string& Name() { return name; }
};
struct ProjectRuntime {
 int draws=0;
 static ProjectRuntime& Get() { static ProjectRuntime r; return r; }
 bool Draw(Canvas&,int,int,int,int,const Theme&) { draws++;return true; }
};
struct App { void DrawHome(Canvas&,int,int,int,int,const Theme&); };
${draw}
int main() {
 App app; Theme theme; auto& console=ConsoleClient::GetInstance();auto& runtime=ProjectRuntime::Get();
 auto render=[&]() { runtime.draws=0;Canvas c;app.DrawHome(c,0,0,128,100,theme);return c; };
 auto c=render();assert(c.Has("123 456"));assert(runtime.draws==0);
 console.state=ConsoleClient::State::Error;console.error="HTTP 500";
 c=render();assert(c.Has("123 456"));assert(runtime.draws==0);
 console.code.clear();c=render();assert(c.Has("Pairing failed"));assert(c.Has("HTTP 500"));assert(runtime.draws==0);
 console.error.clear();console.state=ConsoleClient::State::Connecting;
 c=render();assert(c.Has("Getting code..."));assert(runtime.draws==0);
 console.server.clear();console.state=ConsoleClient::State::Off;
 c=render();assert(c.Has("Console not set"));assert(runtime.draws==0);
 console.linked=true;console.state=ConsoleClient::State::Linked;
 c=render();assert(runtime.draws==1);assert(c.texts.empty());
 Network::GetInstance().state=Network::WifiState::Setup;
 c=render();assert(runtime.draws==0);assert(c.Has("Join Wi-Fi"));
 std::cout << "Pairing code survives connection errors; setup messages precede projects; linked projects still draw\\n";
}
`;
const directory = mkdtempSync(join(tmpdir(), "esp32-pairing-test-"));
try {
  const cpp = join(directory, "test.cpp"), binary = join(directory, "test");
  writeFileSync(cpp, test);
  execFileSync(process.env.CXX || "c++", ["-std=c++17", cpp, "-o", binary], { stdio: "inherit" });
  execFileSync(binary, [], { stdio: "inherit" });
} finally { rmSync(directory, { recursive: true, force: true }); }
