#include "speech.hpp"

#include <windows.h>
#include <mmsystem.h>
#include <sapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <filesystem>
#include <fstream>
#include <thread>

#include "render.hpp"   // widen / narrow
#include "teacher.hpp"

namespace sparring {

namespace stdfs = std::filesystem;

std::vector<std::string> sentences_of(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    bool quote = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        cur += c;
        if (c == '"') quote = !quote;
        const bool end = (c == '.' || c == '!' || c == '?') && !quote && (i + 1 == text.size() || text[i + 1] == ' ');
        if (end || (c == '"' && !quote && i > 0 && (text[i - 1] == '.' || text[i - 1] == '!' || text[i - 1] == '?'))) {
            while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        }
    }
    while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
    if (!cur.empty()) out.push_back(cur);
    // very short bits are said with the next one
    std::vector<std::string> merged;
    for (std::string& s : out) {
        if (!merged.empty() && merged.back().size() < 14) merged.back() += " " + s;
        else merged.push_back(std::move(s));
    }
    return merged;
}

namespace {

std::wstring env(const wchar_t* name) {
    wchar_t buf[1024] = {};
    const DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    return n && n < 1024 ? std::wstring(buf, n) : std::wstring();
}

stdfs::path exe_dir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return stdfs::path(buf).parent_path();
}

bool has_kokoro(const stdfs::path& dir) {
    std::error_code ec;
    if (!stdfs::is_directory(dir, ec)) return false;
    bool model = false, voices = false;
    for (const auto& e : stdfs::directory_iterator(dir, ec)) {
        const std::wstring n = e.path().filename().wstring();
        if (n.rfind(L"kokoro-", 0) == 0 && e.path().extension() == L".onnx") model = true;
        if (n.rfind(L"voices-", 0) == 0 && e.path().extension() == L".bin") voices = true;
    }
    return model && voices;
}

void free_event(SPEVENT& e) {
    switch (e.elParamType) {
    case SPET_LPARAM_IS_TOKEN:
    case SPET_LPARAM_IS_OBJECT:
        if (e.lParam) reinterpret_cast<IUnknown*>(e.lParam)->Release();
        break;
    case SPET_LPARAM_IS_POINTER:
    case SPET_LPARAM_IS_STRING:
        if (e.lParam) CoTaskMemFree(reinterpret_cast<void*>(e.lParam));
        break;
    default: break;
    }
}

// The default token of a SAPI category (the default microphone, the default recognizer).
ISpObjectToken* default_token(const wchar_t* category) {
    ISpObjectTokenCategory* cat = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&cat)))) return nullptr;
    ISpObjectToken* token = nullptr;
    WCHAR* id = nullptr;
    if (SUCCEEDED(cat->SetId(category, FALSE)) && SUCCEEDED(cat->GetDefaultTokenId(&id)) && id) {
        if (SUCCEEDED(CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&token))) && FAILED(token->SetId(nullptr, id, FALSE))) {
            token->Release();
            token = nullptr;
        }
        CoTaskMemFree(id);
    }
    cat->Release();
    return token;
}

}  // namespace

// ---- the voice

std::string Voice::kokoro_folder() {
    std::vector<stdfs::path> places;
    if (const std::wstring s = env(L"CPPSPARRING_SPEECH"); !s.empty()) places.push_back(stdfs::path(s) / "kokoro");
    if (const std::wstring s = env(L"LOCALAPPDATA"); !s.empty()) {
        places.push_back(stdfs::path(s) / "CppSparring" / "speech" / "kokoro");
        places.push_back(stdfs::path(s) / "CodeMonkey" / "speech" / "kokoro");
    }
    for (const stdfs::path& p : places)
        if (has_kokoro(p)) return narrow(p.wstring());
    return {};
}

std::string Voice::python() {
    for (const wchar_t* name : {L"python.exe", L"py.exe"}) {
        wchar_t buf[MAX_PATH] = {};
        if (SearchPathW(nullptr, name, nullptr, MAX_PATH, buf, nullptr)) {
            const std::wstring p(buf);
            if (p.find(L"WindowsApps") == std::wstring::npos) return narrow(p);   // (the Store's stand-in would open the Store)
        }
    }
    return {};
}

std::string Voice::engine() const {
    switch (state_) {
    case State::Kokoro: return "Kokoro";
    case State::Windows: return "Windows";
    default: return "";
    }
}

void Voice::start(HWND notify, UINT msg_helper, UINT msg_windows) {
    notify_ = notify;
    msg_helper_ = msg_helper;
    msg_windows_ = msg_windows;
    start_helper();
}

void Voice::start_helper() {
    if (state_ != State::Unknown) return;
    const std::string folder = kokoro_folder(), py = python();
    const stdfs::path script = exe_dir() / "voice.py";
    std::error_code ec;
    if (folder.empty() || py.empty() || !stdfs::exists(script, ec)) {
        use_windows();
        return;
    }
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out_r = nullptr, out_w = nullptr, in_r = nullptr, in_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&in_r, &in_w, &sa, 0)) {
        use_windows();
        return;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + widen(py) + L"\" \"" + script.wstring() + L"\" \"" + widen(folder) + L"\" af_heart 1.0";
    // the helper goes when CppSparring goes
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
    CloseHandle(out_w);
    CloseHandle(in_r);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(out_r);
        CloseHandle(in_w);
        if (job) CloseHandle(job);
        use_windows();
        return;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);   // (the job handle is kept open for the app's life on purpose)
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    in_ = in_w;
    state_ = State::Starting;
    HWND h = notify_;
    const UINT msg = msg_helper_;
    std::thread([out_r, h, msg] {
        std::string line;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(out_r, buf, sizeof buf, &n, nullptr) && n) {
            for (DWORD i = 0; i < n; ++i) {
                if (buf[i] == '\n') {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    auto* s = new std::string(std::move(line));
                    if (!PostMessageW(h, msg, 0, reinterpret_cast<LPARAM>(s))) delete s;
                    line.clear();
                } else {
                    line += buf[i];
                }
            }
        }
        CloseHandle(out_r);
        auto* s = new std::string("EXIT");
        if (!PostMessageW(h, msg, 0, reinterpret_cast<LPARAM>(s))) delete s;
    }).detach();
}

void Voice::use_windows() {
    const bool was_kokoro = state_ == State::Starting || state_ == State::Kokoro;
    state_ = State::Windows;
    if (in_) CloseHandle(in_), in_ = nullptr;
    if (process_) CloseHandle(process_), process_ = nullptr;
    if (!windows_ && SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows_)))) {
        windows_->SetNotifyWindowMessage(notify_, msg_windows_, 0, 0);
        const ULONGLONG want = SPFEI(SPEI_VISEME) | SPFEI(SPEI_END_INPUT_STREAM) | SPFEI(SPEI_START_INPUT_STREAM);
        windows_->SetInterest(want, want);
    }
    // what was waiting for Kokoro is said by Windows instead
    if (was_kokoro && next_ < static_cast<int>(sentences_.size())) {
        std::string rest;
        for (std::size_t i = static_cast<std::size_t>(next_); i < sentences_.size(); ++i) rest += sentences_[i] + " ";
        clips_.clear();
        say_windows(rest);
    }
}

void Voice::stop_all() {
    hush();
    if (in_) CloseHandle(in_), in_ = nullptr;
    if (process_) {
        TerminateProcess(process_, 0);
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (windows_) windows_->Release(), windows_ = nullptr;
}

void Voice::request(int index) {
    if (!in_) return;
    std::string line = std::to_string(gen_) + "." + std::to_string(index) + " " + sentences_[static_cast<std::size_t>(index)];
    std::replace(line.begin(), line.end(), '\n', ' ');
    line += "\n";
    DWORD wrote = 0;
    if (!WriteFile(in_, line.data(), static_cast<DWORD>(line.size()), &wrote, nullptr)) use_windows();
}

void Voice::say(const std::string& text) {
    hush();
    if (muted || state_ == State::Unknown) return;   // (not started: the self-test)
    sentences_ = sentences_of(spoken(text));
    next_ = 0;
    if (state_ == State::Windows) {
        say_windows(spoken(text));
        return;
    }
    for (int i = 0; i < static_cast<int>(sentences_.size()); ++i) {
        clips_.push_back(Clip{gen_, i, {}, false});
        request(i);
    }
}

void Voice::say_windows(const std::string& text) {
    if (!windows_ || text.empty()) return;
    windows_->Speak(widen(text).c_str(), SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, nullptr);
    windows_talking_ = true;
}

void Voice::hush() {
    ++gen_;
    for (const Clip& c : clips_)
        if (!c.path.empty()) DeleteFileW(widen(c.path).c_str());
    clips_.clear();
    sentences_.clear();
    next_ = 0;
    stop_playing();
    if (windows_ && windows_talking_) windows_->Speak(L"", SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
    windows_talking_ = false;
    mouth_target_ = 0;
}

void Voice::on_helper(LPARAM lp) {
    std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(lp));
    const std::string& line = *s;
    if (line.rfind("READY", 0) == 0) {
        if (state_ == State::Starting) state_ = State::Kokoro;
    } else if (line.rfind("NO", 0) == 0 || line == "EXIT") {
        if (state_ == State::Starting || state_ == State::Kokoro) use_windows();
    } else if (line.rfind("WAV ", 0) == 0 || line.rfind("ERR ", 0) == 0) {
        const std::size_t sp = line.find(' ', 4);
        const std::string id = line.substr(4, sp == std::string::npos ? std::string::npos : sp - 4);
        const std::string rest = sp == std::string::npos ? "" : line.substr(sp + 1);
        const int gen = std::atoi(id.c_str());
        const int index = std::atoi(id.c_str() + std::min(id.size(), id.find('.') + 1));
        const bool wav = line[0] == 'W';
        bool used = false;
        for (Clip& c : clips_)
            if (c.gen == gen && c.index == index) {
                if (wav) c.path = rest;
                else c.failed = true;
                used = true;
            }
        if (wav && !used) DeleteFileW(widen(rest).c_str());   // for something no longer being said
    }
}

void Voice::on_windows() {
    if (!windows_) return;
    SPEVENT e{};
    ULONG got = 0;
    while (windows_->GetEvents(1, &e, &got) == S_OK && got) {
        if (e.eEventId == SPEI_VISEME) {
            const int v = LOWORD(e.lParam);
            // silence, then mouth shapes from closed (b, m, p) to wide (a)
            static const float open[] = {0.0f, 0.9f, 1.0f, 0.8f, 0.6f, 0.6f, 0.4f, 0.4f, 0.7f, 0.5f, 0.5f, 0.8f, 0.4f, 0.5f, 0.4f, 0.4f, 0.4f, 0.3f, 0.2f, 0.4f, 0.3f, 0.0f};
            mouth_target_ = v >= 0 && v < 22 ? open[v] : 0.4f;
        } else if (e.eEventId == SPEI_END_INPUT_STREAM) {
            windows_talking_ = false;
            mouth_target_ = 0;
        }
        free_event(e);
    }
}

bool Voice::play(const std::string& path) {
    std::ifstream f(stdfs::path(widen(path)), std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < 44 || std::string(data.data(), 4) != "RIFF") return false;
    WAVEFORMATEX fmt{};
    std::size_t at = 12, pcm_at = 0, pcm_len = 0;
    while (at + 8 <= data.size()) {
        const std::string id(data.data() + at, 4);
        std::uint32_t len = 0;
        std::memcpy(&len, data.data() + at + 4, 4);
        if (id == "fmt " && len >= 16) std::memcpy(&fmt, data.data() + at + 8, 16);
        if (id == "data") {
            pcm_at = at + 8;
            pcm_len = std::min<std::size_t>(len, data.size() - pcm_at);
            break;
        }
        at += 8 + len + (len & 1);
    }
    if (!pcm_len || fmt.wFormatTag != WAVE_FORMAT_PCM || fmt.wBitsPerSample != 16) return false;
    fmt.cbSize = 0;
    pcm_.assign(data.begin() + static_cast<std::ptrdiff_t>(pcm_at), data.begin() + static_cast<std::ptrdiff_t>(pcm_at + pcm_len));
    rate_ = static_cast<int>(fmt.nSamplesPerSec);
    // how loud each 10 ms is, for the mouth
    envelope_.clear();
    const auto* samples = reinterpret_cast<const std::int16_t*>(pcm_.data());
    const std::size_t n = pcm_.size() / 2 / std::max<WORD>(1, fmt.nChannels);
    const std::size_t step = std::max<std::size_t>(1, static_cast<std::size_t>(rate_) / 100);
    float loudest = 0.05f;
    for (std::size_t i = 0; i < n; i += step) {
        double sum = 0;
        const std::size_t end = std::min(n, i + step);
        for (std::size_t j = i; j < end; ++j) {
            const double v = samples[j * fmt.nChannels] / 32768.0;
            sum += v * v;
        }
        const float rms = static_cast<float>(std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1, end - i))));
        envelope_.push_back(rms);
        loudest = std::max(loudest, rms);
    }
    for (float& v : envelope_) v = std::min(1.0f, std::sqrt(v / (loudest * 0.8f)));
    HWAVEOUT h = nullptr;
    if (waveOutOpen(&h, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return false;
    auto* hdr = new WAVEHDR{};
    hdr->lpData = pcm_.data();
    hdr->dwBufferLength = static_cast<DWORD>(pcm_.size());
    waveOutPrepareHeader(h, hdr, sizeof(WAVEHDR));
    waveOutWrite(h, hdr, sizeof(WAVEHDR));
    wave_ = h;
    header_ = hdr;
    playing_path_ = path;
    return true;
}

bool Voice::playing_done() const {
    return header_ && (static_cast<WAVEHDR*>(header_)->dwFlags & WHDR_DONE);
}

void Voice::stop_playing() {
    if (wave_) {
        HWAVEOUT h = static_cast<HWAVEOUT>(wave_);
        waveOutReset(h);
        waveOutUnprepareHeader(h, static_cast<WAVEHDR*>(header_), sizeof(WAVEHDR));
        waveOutClose(h);
        delete static_cast<WAVEHDR*>(header_);
        wave_ = header_ = nullptr;
    }
    if (!playing_path_.empty()) DeleteFileW(widen(playing_path_).c_str());
    playing_path_.clear();
    pcm_.clear();
    envelope_.clear();
}

void Voice::tick(float dt) {
    if (state_ != State::Windows) {
        if (wave_ && playing_done()) {
            stop_playing();
            if (!clips_.empty()) clips_.pop_front();
            ++next_;
        }
        if (!wave_) {
            while (!clips_.empty() && clips_.front().gen == gen_ && clips_.front().failed) {
                clips_.pop_front();
                ++next_;
            }
            if (!clips_.empty() && clips_.front().gen == gen_ && !clips_.front().path.empty()) {
                const std::string path = clips_.front().path;
                if (!play(path)) {
                    DeleteFileW(widen(path).c_str());
                    clips_.pop_front();
                    ++next_;
                }
            }
        }
        mouth_target_ = 0;
        if (wave_ && !envelope_.empty()) {
            MMTIME t{};
            t.wType = TIME_SAMPLES;
            waveOutGetPosition(static_cast<HWAVEOUT>(wave_), &t, sizeof t);
            const std::size_t at = static_cast<std::size_t>(t.u.sample) * 100 / static_cast<std::size_t>(std::max(1, rate_));
            mouth_target_ = envelope_[std::min(at, envelope_.size() - 1)];
        }
    } else if (windows_talking_) {
        mouth_target_ *= std::pow(0.02f, dt);   // between visemes it closes a little
    }
    mouth_ += (mouth_target_ - mouth_) * std::min(1.0f, dt * 28);
    if (mouth_ < 0.01f) mouth_ = 0;
}

bool Voice::talking() const {
    if (state_ == State::Windows) return windows_talking_;
    return next_ < static_cast<int>(sentences_.size());
}

bool Voice::waiting() const {
    return (state_ == State::Starting || state_ == State::Kokoro) && !wave_ && next_ < static_cast<int>(sentences_.size());
}

// ---- listening

bool Ear::start(HWND notify, UINT msg, const std::vector<std::string>& phrases) {
    if (FAILED(CoCreateInstance(CLSID_SpInprocRecognizer, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&recognizer_)))) return false;
    ISpObjectToken* engine = default_token(SPCAT_RECOGNIZERS);
    ISpObjectToken* mic = default_token(SPCAT_AUDIOIN);
    const bool ok = engine && mic && SUCCEEDED(recognizer_->SetRecognizer(engine)) && SUCCEEDED(recognizer_->SetInput(mic, TRUE)) &&
                    SUCCEEDED(recognizer_->CreateRecoContext(&context_));
    if (engine) engine->Release();
    if (mic) mic->Release();
    if (!ok) {
        stop_all();
        return false;
    }
    context_->SetNotifyWindowMessage(notify, msg, 0, 0);
    const ULONGLONG want = SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_FALSE_RECOGNITION);
    context_->SetInterest(want, want);
    SPSTATEHANDLE top = nullptr;
    if (FAILED(context_->CreateGrammar(1, &grammar_)) || FAILED(grammar_->GetRule(L"ask", 0, SPRAF_TopLevel | SPRAF_Active, TRUE, &top))) {
        stop_all();
        return false;
    }
    for (const std::string& p : phrases) grammar_->AddWordTransition(top, nullptr, widen(p).c_str(), L" ", SPWT_LEXICAL, 1.0f, nullptr);
    if (FAILED(grammar_->Commit(0))) {
        stop_all();
        return false;
    }
    grammar_->SetRuleState(nullptr, nullptr, SPRS_INACTIVE);
    recognizer_->SetRecoState(SPRST_INACTIVE);   // the microphone is only open while listening
    return true;
}

void Ear::listen(bool on) {
    if (!context_) return;
    on_ = on;
    grammar_->SetRuleState(nullptr, nullptr, on ? SPRS_ACTIVE : SPRS_INACTIVE);
    recognizer_->SetRecoState(on ? SPRST_ACTIVE : SPRST_INACTIVE);
}

bool Ear::on_event(std::string& heard) {
    if (!context_) return false;
    bool got_any = false;
    SPEVENT e{};
    ULONG got = 0;
    while (context_->GetEvents(1, &e, &got) == S_OK && got) {
        if (e.eEventId == SPEI_RECOGNITION && e.elParamType == SPET_LPARAM_IS_OBJECT && e.lParam) {
            auto* result = reinterpret_cast<ISpRecoResult*>(e.lParam);
            SPPHRASE* phrase = nullptr;
            bool sure = true;
            if (SUCCEEDED(result->GetPhrase(&phrase)) && phrase) {
                sure = phrase->Rule.Confidence != SP_LOW_CONFIDENCE;
                CoTaskMemFree(phrase);
            }
            WCHAR* text = nullptr;
            if (SUCCEEDED(result->GetText(SP_GETWHOLEPHRASE, SP_GETWHOLEPHRASE, TRUE, &text, nullptr)) && text) {
                heard = sure ? narrow(text) : std::string();
                CoTaskMemFree(text);
            }
            got_any = true;
        } else if (e.eEventId == SPEI_FALSE_RECOGNITION) {
            heard.clear();
            got_any = true;
        }
        free_event(e);
    }
    return got_any;
}

std::string Ear::hear_file(const std::wstring& wav, const std::vector<std::string>& phrases) {
    std::string heard;
    ISpRecognizer* rec = nullptr;
    ISpStream* stream = nullptr;
    ISpRecoContext* ctx = nullptr;
    ISpRecoGrammar* g = nullptr;
    ISpObjectToken* engine = default_token(SPCAT_RECOGNIZERS);
    if (engine && SUCCEEDED(CoCreateInstance(CLSID_SpInprocRecognizer, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&rec))) &&
        SUCCEEDED(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&stream))) &&
        SUCCEEDED(stream->BindToFile(wav.c_str(), SPFM_OPEN_READONLY, nullptr, nullptr, 0)) && SUCCEEDED(rec->SetRecognizer(engine)) &&
        SUCCEEDED(rec->SetInput(stream, TRUE)) && SUCCEEDED(rec->CreateRecoContext(&ctx)) && SUCCEEDED(ctx->SetNotifyWin32Event()) &&
        SUCCEEDED(ctx->SetInterest(SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_FALSE_RECOGNITION) | SPFEI(SPEI_END_SR_STREAM),
                                   SPFEI(SPEI_RECOGNITION) | SPFEI(SPEI_FALSE_RECOGNITION) | SPFEI(SPEI_END_SR_STREAM))) &&
        SUCCEEDED(ctx->CreateGrammar(1, &g))) {
        SPSTATEHANDLE top = nullptr;
        g->GetRule(L"ask", 0, SPRAF_TopLevel | SPRAF_Active, TRUE, &top);
        for (const std::string& p : phrases) g->AddWordTransition(top, nullptr, widen(p).c_str(), L" ", SPWT_LEXICAL, 1.0f, nullptr);
        g->Commit(0);
        g->SetRuleState(nullptr, nullptr, SPRS_ACTIVE);
        rec->SetRecoState(SPRST_ACTIVE);
        bool done = false;
        for (int waits = 0; waits < 300 && !done; ++waits) {   // up to 30 s
            if (ctx->WaitForNotifyEvent(100) != S_OK) continue;
            SPEVENT e{};
            ULONG got = 0;
            while (ctx->GetEvents(1, &e, &got) == S_OK && got) {
                if (e.eEventId == SPEI_RECOGNITION && e.elParamType == SPET_LPARAM_IS_OBJECT && e.lParam) {
                    WCHAR* text = nullptr;
                    if (SUCCEEDED(reinterpret_cast<ISpRecoResult*>(e.lParam)->GetText(SP_GETWHOLEPHRASE, SP_GETWHOLEPHRASE, TRUE, &text, nullptr)) && text) {
                        heard = narrow(text);
                        CoTaskMemFree(text);
                    }
                    done = true;
                } else if (e.eEventId == SPEI_END_SR_STREAM) {
                    done = true;
                }
                free_event(e);
            }
        }
    }
    if (g) g->Release();
    if (ctx) ctx->Release();
    if (rec) rec->Release();
    if (stream) stream->Close(), stream->Release();
    if (engine) engine->Release();
    return heard;
}

void Ear::stop_all() {
    on_ = false;
    if (grammar_) grammar_->Release(), grammar_ = nullptr;
    if (context_) context_->Release(), context_ = nullptr;
    if (recognizer_) {
        recognizer_->SetRecoState(SPRST_INACTIVE_WITH_PURGE);
        recognizer_->Release();
        recognizer_ = nullptr;
    }
}

}  // namespace sparring
