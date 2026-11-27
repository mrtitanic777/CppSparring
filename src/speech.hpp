#pragma once
#include <windows.h>

#include <deque>
#include <string>
#include <vector>

struct ISpVoice;
struct ISpRecognizer;
struct ISpRecoContext;
struct ISpRecoGrammar;

namespace sparring {

// Bit's voice. Kokoro (a lifelike voice that runs on this computer, the same setup CodeMonkey uses) when it is there, else the
// voices Windows has. Text is said one sentence at a time; mouth() follows the sound so Bit's mouth moves with it.
struct Voice {
    // `notify` gets `msg` when the voice helper has something (call on_helper) or Windows' voice has events (call on_windows).
    void start(HWND notify, UINT msg_helper, UINT msg_windows);
    void stop_all();   // at exit
    void say(const std::string& text);
    void hush();       // stop talking now
    void on_helper(LPARAM lp);   // a line from the helper (a heap std::string*)
    void on_windows();
    void tick(float dt);         // every frame: plays the next sentence, moves the mouth
    float mouth() const { return mouth_; }
    bool talking() const;        // saying something, or about to
    bool waiting() const;        // the voice is still being made
    bool muted = false;
    std::string engine() const;  // "Kokoro", "Windows", or "" before it is known

    // Where Kokoro is: CPPSPARRING_SPEECH\kokoro, %LOCALAPPDATA%\CppSparring\speech\kokoro, or CodeMonkey's. Empty when none.
    static std::string kokoro_folder();
    static std::string python();

private:
    struct Clip {
        int gen = 0, index = 0;
        std::string path;   // empty until made
        bool failed = false;
    };
    HWND notify_ = nullptr;
    UINT msg_helper_ = 0, msg_windows_ = 0;
    enum class State { Unknown, Starting, Kokoro, Windows } state_ = State::Unknown;
    HANDLE process_ = nullptr, in_ = nullptr;
    int gen_ = 0;
    std::vector<std::string> sentences_;   // of the thing being said now
    std::deque<Clip> clips_;
    int next_ = 0;                         // the next sentence to play
    // playing a clip
    void* wave_ = nullptr;                 // HWAVEOUT
    void* header_ = nullptr;               // WAVEHDR*
    std::vector<char> pcm_;
    std::vector<float> envelope_;          // loudness per 10 ms, 0..1
    int rate_ = 24000;
    std::string playing_path_;
    float mouth_ = 0, mouth_target_ = 0;
    ISpVoice* windows_ = nullptr;
    bool windows_talking_ = false;

    void start_helper();
    void use_windows();
    void request(int index);
    bool play(const std::string& path);
    void stop_playing();
    bool playing_done() const;
    void say_windows(const std::string& text);
};

// Listening for a question with Windows' speech recognizer (on this computer, no internet). It only listens for the phrases it is
// given, so it hears "what does line five do" reliably and nothing else.
struct Ear {
    bool start(HWND notify, UINT msg, const std::vector<std::string>& phrases);
    void stop_all();
    bool ready() const { return context_ != nullptr; }
    void listen(bool on);
    bool listening() const { return on_; }
    // Handles the recognizer's events: true when it heard something (`heard`, empty for "something, but not a phrase it knows").
    bool on_event(std::string& heard);
    // What the recognizer hears in a WAV file with the same phrases (for testing without a microphone); empty for nothing.
    static std::string hear_file(const std::wstring& wav, const std::vector<std::string>& phrases);

private:
    ISpRecognizer* recognizer_ = nullptr;
    ISpRecoContext* context_ = nullptr;
    ISpRecoGrammar* grammar_ = nullptr;
    bool on_ = false;
};

// Sentences, for saying one at a time.
std::vector<std::string> sentences_of(const std::string& text);

}  // namespace sparring
